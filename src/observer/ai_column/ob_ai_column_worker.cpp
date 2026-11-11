/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER
#include "observer/ai_column/ob_ai_column_worker.h"
#include "sql/ai_column/ob_ai_column_util.h"
#include "share/ob_server_struct.h"
#include "share/ob_server_status.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "query/vector/ob_vector_index_util.h"
#include "lib/charset/ob_charset.h"
#include "lib/time/ob_time_utility.h"
#include "observer/ob_server.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace share::schema;
using namespace sql;
namespace observer
{

namespace
{
// text results of inner sql carry a lob header, read the characters behind it
int read_text_value(ObIAllocator &allocator, const ObObj &obj, ObString &str)
{
  int ret = OB_SUCCESS;
  if (obj.has_lob_header()) {
    ObILobReadService *lob_read_service = ObServer::get_instance().lob_read_service();
    if (OB_ISNULL(lob_read_service)) {
      ret = OB_NOT_INIT;
    } else {
      const ObLobReadOptions options(*lob_read_service);
      ObString data = obj.get_string();
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(options, &allocator, obj, data))) {
        LOG_WARN("failed to read text value", K(ret));
      } else if (OB_FAIL(ob_write_string(allocator, data, str))) {
      }
    }
  } else if (OB_FAIL(ob_write_string(allocator, obj.get_string(), str))) {
  }
  return ret;
}
} // namespace

ObAIColumnWorker &ObAIColumnWorker::get_instance()
{
  static ObAIColumnWorker instance;
  return instance;
}

ObAIColumnWorker::ObAIColumnWorker()
  : lib::ThreadPool(THREAD_COUNT), claim_seq_(0)
{
}

int ObAIColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(lib::ThreadPool::start())) {
    LOG_WARN("failed to start AI column worker", K(ret));
  } else {
    LOG_INFO("AI column worker started");
  }
  return ret;
}

void ObAIColumnWorker::stop()
{
  lib::ThreadPool::stop();
}

void ObAIColumnWorker::wait()
{
  lib::ThreadPool::wait();
}

void ObAIColumnWorker::run1()
{
  int ret = OB_SUCCESS;
  const bool is_maintainer = (0 == get_thread_idx());
  int64_t last_maintenance_ts = 0;
  bool recovered = false;
  lib::set_thread_name("AIColumnWorker");
  int64_t idle_interval_us = IDLE_INTERVAL_US;
  while (!has_set_stop()) {
    bool found = false;
    if (SS_SERVING != GCTX.status_ || OB_ISNULL(GCTX.sql_proxy_) || OB_ISNULL(GCTX.schema_service_)) {
      ob_usleep(IDLE_INTERVAL_US * 10);
      continue;
    }
    if (is_maintainer) {
      const int64_t now = ObTimeUtility::current_time();
      if (!recovered) {
        // claims of the previous run of this server cannot be finished any more
        if (OB_FAIL(recover_claims(true))) {
          LOG_WARN("failed to recover AI column task claims", K(ret));
        } else {
          recovered = true;
        }
      } else if (now - last_maintenance_ts > MAINTENANCE_INTERVAL_US) {
        if (OB_FAIL(recover_claims(false))) {
          LOG_WARN("failed to recover stale AI column task claims", K(ret));
        } else if (OB_FAIL(clean_orphans())) {
          LOG_WARN("failed to clean AI column tasks of dropped tables", K(ret));
        }
        last_maintenance_ts = now;
      }
      ret = OB_SUCCESS;
    }
    if (OB_FAIL(process_one(found))) {
      LOG_WARN("failed to process AI column task", K(ret));
      ret = OB_SUCCESS;
    }
    if (!found) {
      ob_usleep(idle_interval_us);
      if (idle_interval_us < MAX_IDLE_INTERVAL_US) {
        idle_interval_us = MIN(idle_interval_us * 2, MAX_IDLE_INTERVAL_US);
      }
    } else {
      idle_interval_us = IDLE_INTERVAL_US;
    }
  }
}

int ObAIColumnWorker::append_group_condition(const GroupKey &key, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt(" WHERE table_id = %lu AND rowkey_predicate = ", key.table_id_))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, key.rowkey_predicate_))) {
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, key.request_id_))) {
  }
  return ret;
}

int ObAIColumnWorker::process_one(bool &found)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnWorker");
  ObSEArray<GroupKey, CANDIDATE_LIMIT> candidates;
  ObSqlString sql;
  found = false;
  if (OB_FAIL(sql.append_fmt("SELECT table_id, rowkey_predicate, request_id FROM oceanbase.__all_ai_column_task"
                             " WHERE state IN (%d, %d) AND eligible_ts <= now(6)"
                             " ORDER BY eligible_ts LIMIT %ld",
                             AI_TASK_PENDING, AI_TASK_RETRY_WAIT, CANDIDATE_LIMIT))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        GroupKey key;
        int64_t table_id = 0;
        ObString predicate;
        ObString request_id;
        bool duplicated = false;
        if (OB_FAIL(result->get_int(static_cast<int64_t>(0), table_id))) {
        } else if (OB_FAIL(result->get_varchar(1L, predicate))) {
        } else if (OB_FAIL(result->get_varchar(2L, request_id))) {
        } else if (OB_FAIL(ob_write_string(allocator, predicate, key.rowkey_predicate_))) {
        } else if (OB_FAIL(ob_write_string(allocator, request_id, key.request_id_))) {
        } else {
          key.table_id_ = static_cast<uint64_t>(table_id);
          for (int64_t i = 0; !duplicated && i < candidates.count(); ++i) {
            duplicated = candidates.at(i).table_id_ == key.table_id_
                         && candidates.at(i).rowkey_predicate_ == key.rowkey_predicate_
                         && candidates.at(i).request_id_ == key.request_id_;
          }
          if (!duplicated && OB_FAIL(candidates.push_back(key))) {
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  // the threads start from different candidates so that they do not all race for the first one
  const int64_t offset = candidates.empty() ? 0 : static_cast<int64_t>(get_thread_idx()) % candidates.count();
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < candidates.count(); ++i) {
    const GroupKey &key = candidates.at((i + offset) % candidates.count());
    char token_buf[64];
    const int64_t token_len = snprintf(token_buf, sizeof(token_buf), "%lu-%ld-%ld",
                                       get_thread_idx(), ObTimeUtility::current_time(),
                                       ATOMIC_AAF(&claim_seq_, 1));
    const ObString token(static_cast<int32_t>(token_len), token_buf);
    bool claimed = false;
    if (OB_FAIL(claim_group(key, token, claimed))) {
    } else if (claimed) {
      found = true;
      if (OB_FAIL(process_group(allocator, key, token))) {
        LOG_WARN("failed to process AI column request", K(ret), K(key.table_id_),
                 K(key.rowkey_predicate_), K(key.request_id_));
        // the claim is released by the recovery of stale claims
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::claim_group(const GroupKey &key, const ObString &token, bool &claimed)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  claimed = false;
  if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state = %d, claim_ts = now(6),"
                             " claim_token = ", AI_TASK_RUNNING))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, token))) {
  } else if (OB_FAIL(append_group_condition(key, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND state IN (%d, %d) AND eligible_ts <= now(6)",
                                    AI_TASK_PENDING, AI_TASK_RETRY_WAIT))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
  } else {
    claimed = affected_rows > 0;
  }
  return ret;
}

int ObAIColumnWorker::process_group(ObIAllocator &allocator, const GroupKey &key, const ObString &token)
{
  int ret = OB_SUCCESS;
  ObSEArray<ColumnTask, 4> tasks;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table_schema = nullptr;
  const ObDatabaseSchema *database_schema = nullptr;
  ObSqlString sql;
  if (OB_FAIL(sql.append("SELECT column_id, retry_count, max_retries FROM oceanbase.__all_ai_column_task"))) {
  } else if (OB_FAIL(append_group_condition(key, sql))) {
  } else if (OB_FAIL(sql.append(" AND claim_token = "))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, token))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        ColumnTask task;
        int64_t column_id = 0;
        task.succeeded_ = false;
        if (OB_FAIL(result->get_int(static_cast<int64_t>(0), column_id))) {
        } else if (OB_FAIL(result->get_int(1L, task.retry_count_))) {
        } else if (OB_FAIL(result->get_int(2L, task.max_retries_))) {
        } else if (FALSE_IT(task.column_id_ = static_cast<uint64_t>(column_id))) {
        } else if (OB_FAIL(tasks.push_back(task))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_table_schema(key.table_id_, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    // the table was dropped
    if (OB_FAIL(drop_claimed_tasks(key, token))) {
    }
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(), database_schema))) {
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    bool row_changed = false;
    bool still_owned = true;
    for (int64_t i = 0; OB_SUCC(ret) && !row_changed && i < tasks.count(); ++i) {
      if (OB_FAIL(evaluate_column(allocator, key, database_schema->get_database_name_str(),
                                  *table_schema, tasks.at(i), row_changed))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (row_changed) {
      // the row was deleted or changed again; the newer change has its own request
      if (OB_FAIL(drop_claimed_tasks(key, token))) {
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && still_owned && i < tasks.count(); ++i) {
        if (OB_FAIL(record_outcome(key, token, tasks.at(i), still_owned))) {
        }
      }
      if (OB_SUCC(ret) && still_owned
          && OB_FAIL(try_publish(key, database_schema->get_database_name_str(), *table_schema))) {
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::evaluate_column(ObIAllocator &allocator, const GroupKey &key,
                                      const ObString &database_name,
                                      const ObTableSchema &table_schema,
                                      ColumnTask &task, bool &row_changed)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table_schema.get_column_schema(task.column_id_);
  ObAIColumnSpec spec;
  bool is_ai = false;
  ObString expr;
  ObSqlString sql;
  row_changed = false;
  task.succeeded_ = false;
  if (OB_ISNULL(column)) {
    row_changed = true;  // the AI column was dropped
  } else if (OB_FAIL(ObAIColumnUtil::get_spec(allocator, table_schema, column->get_column_name_str(),
                                              is_ai, &spec))) {
  } else if (!is_ai) {
    row_changed = true;
  } else if (OB_FAIL(ObAIColumnUtil::build_normalized_expr(allocator, spec, *column, expr))) {
  } else if (OB_FAIL(sql.append_fmt("SELECT CAST(%.*s AS CHAR) FROM `%.*s`.`%.*s` WHERE (%.*s) AND `%s%.*s` = ",
                                    expr.length(), expr.ptr(),
                                    database_name.length(), database_name.ptr(),
                                    table_schema.get_table_name_str().length(),
                                    table_schema.get_table_name_str().ptr(),
                                    key.rowkey_predicate_.length(), key.rowkey_predicate_.ptr(),
                                    ObAIColumnUtil::REQUEST_PREFIX,
                                    spec.column_name_.length(), spec.column_name_.ptr()))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, key.request_id_))) {
  } else {
    int eval_ret = OB_SUCCESS;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      ObObj value;
      if (OB_SUCCESS != (eval_ret = GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        eval_ret = OB_ERR_UNEXPECTED;
      } else if (OB_SUCCESS != (eval_ret = result->next())) {
        if (OB_ITER_END == eval_ret) {
          row_changed = true;
          eval_ret = OB_SUCCESS;
        }
      } else if (OB_SUCCESS != (eval_ret = result->get_obj(0L, value, nullptr, &allocator))) {
      } else if (value.is_null()) {
        eval_ret = OB_INVALID_DATA;
      } else if (OB_FAIL(read_text_value(allocator, value, task.value_))) {
      } else if (OB_FAIL(check_value(allocator, table_schema, task.column_id_, task.value_, task.error_))) {
      } else {
        task.succeeded_ = task.error_.empty();
      }
    }
    if (OB_SUCC(ret) && OB_SUCCESS != eval_ret) {
      // a model or conversion error is an outcome of this attempt, not a worker failure
      char buf[256];
      const int64_t len = snprintf(buf, sizeof(buf), "%d: %s", eval_ret, ob_strerror(eval_ret));
      if (OB_FAIL(ob_write_string(allocator, ObString(static_cast<int32_t>(MIN(len, (int64_t)sizeof(buf) - 1)), buf),
                                  task.error_))) {
      }
      LOG_INFO("AI column request failed", K(eval_ret), K(key.table_id_), K(key.rowkey_predicate_),
               K(task.column_id_));
    }
  }
  return ret;
}

int ObAIColumnWorker::check_value(ObIAllocator &allocator, const ObTableSchema &table_schema,
                                  const uint64_t column_id, const ObString &value, ObString &error)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table_schema.get_column_schema(column_id);
  const ObObjType type = OB_ISNULL(column) ? ObNullType : column->get_data_type();
  const ObString text = value.trim();
  const char *reason = nullptr;
  error.reset();
  if (OB_ISNULL(column)) {
    reason = "AI column is missing";
  } else if (ob_is_json(type)) {
    ObIJsonBase *j_base = nullptr;
    if (OB_SUCCESS != ObJsonBaseFactory::get_json_base(&allocator, text, ObJsonInType::JSON_TREE,
                                                       ObJsonInType::JSON_TREE, j_base)) {
      reason = "result is not a valid JSON value";
    }
  } else if (ob_is_integer_type(type)) {
    bool valid = !text.empty();
    for (int64_t i = 0; valid && i < text.length(); ++i) {
      const char c = text.ptr()[i];
      valid = (c >= '0' && c <= '9') || (0 == i && (c == '-' || c == '+') && text.length() > 1);
    }
    if (!valid) {
      reason = "result is not a base-10 integer";
    }
  } else if (ob_is_float_tc(type) || ob_is_double_tc(type) || ob_is_number_tc(type)
             || ob_is_decimal_int_tc(type)) {
    char buf[128];
    char *end = nullptr;
    if (text.empty() || text.length() >= static_cast<int32_t>(sizeof(buf))) {
      reason = "result is not a number";
    } else {
      MEMCPY(buf, text.ptr(), text.length());
      buf[text.length()] = '\0';
      (void) strtod(buf, &end);
      if (end != buf + text.length()) {
        reason = "result is not a number";
      }
    }
  } else if (column->is_collection()) {
    ObIJsonBase *j_base = nullptr;
    int64_t dim = 0;
    if (OB_SUCCESS != ObJsonBaseFactory::get_json_base(&allocator, text, ObJsonInType::JSON_TREE,
                                                       ObJsonInType::JSON_TREE, j_base)
        || OB_ISNULL(j_base) || ObJsonNodeType::J_ARRAY != j_base->json_type()) {
      reason = "result is not a vector";
    } else if (OB_SUCCESS != ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                                 column->get_extended_type_info(), dim)
               || static_cast<int64_t>(j_base->element_count()) != dim) {
      reason = "vector dimension does not match the column";
    }
  } else if (ob_is_string_tc(type)) {
    const int64_t char_len = ObCharset::strlen_char(column->get_collation_type(), value.ptr(), value.length());
    if (column->get_data_length() > 0 && char_len > column->get_data_length()) {
      reason = "result is longer than the column";
    }
  }
  if (OB_NOT_NULL(reason) && OB_FAIL(ob_write_string(allocator, ObString::make_string(reason), error))) {
  }
  return ret;
}

int ObAIColumnWorker::record_outcome(const GroupKey &key, const ObString &token,
                                     const ColumnTask &task, bool &still_owned)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET "))) {
  } else if (task.succeeded_) {
    if (OB_FAIL(sql.append_fmt("state = %d, result_state = %d, last_error = NULL, result = ",
                               AI_TASK_DONE, AI_ROW_READY))) {
    } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, task.value_))) {
    }
  } else if (task.retry_count_ < task.max_retries_) {
    const int64_t delay = MIN(RETRY_BASE_DELAY_US << MIN(task.retry_count_, 16L), RETRY_MAX_DELAY_US);
    if (OB_FAIL(sql.append_fmt("state = %d, retry_count = retry_count + 1, claim_token = NULL,"
                               " claim_ts = NULL, eligible_ts = DATE_ADD(now(6), INTERVAL %ld MICROSECOND),"
                               " last_error = ", AI_TASK_RETRY_WAIT, delay))) {
    } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, task.error_))) {
    }
  } else {
    if (OB_FAIL(sql.append_fmt("state = %d, result_state = %d, result = NULL, last_error = ",
                               AI_TASK_DONE, AI_ROW_FAILED))) {
    } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, task.error_))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(append_group_condition(key, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND column_id = %lu AND claim_token = ", task.column_id_))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, token))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
  } else {
    // a newer change of the row replaced the task while the model was running
    still_owned = affected_rows > 0;
  }
  return ret;
}

int ObAIColumnWorker::try_publish(const GroupKey &key, const ObString &database_name,
                                  const ObTableSchema &table_schema, const bool retry_on_error)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObArenaAllocator allocator("AIColumnPublish");
  ObSqlString sql;
  bool complete = true;
  struct Outcome
  {
    uint64_t column_id_;
    bool ready_;
    bool has_value_;
    ObString value_;
  };
  ObSEArray<Outcome, 4> outcomes;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
  } else if (OB_FAIL(sql.append("SELECT column_id, state, result_state, result FROM oceanbase.__all_ai_column_task"))) {
  } else if (OB_FAIL(append_group_condition(key, sql))) {
  } else if (OB_FAIL(sql.append(" FOR UPDATE"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        Outcome outcome;
        int64_t column_id = 0;
        int64_t state = 0;
        int64_t result_state = 0;
        ObObj value;
        if (OB_FAIL(result->get_int(static_cast<int64_t>(0), column_id))) {
        } else if (OB_FAIL(result->get_int(1L, state))) {
        } else if (AI_TASK_DONE != state) {
          complete = false;
        } else if (OB_FAIL(result->get_int(2L, result_state))) {
        } else if (OB_FAIL(result->get_obj(3L, value, nullptr, &allocator))) {
        } else {
          outcome.column_id_ = static_cast<uint64_t>(column_id);
          outcome.ready_ = (AI_ROW_READY == result_state);
          outcome.has_value_ = !value.is_null();
          if (outcome.has_value_ && OB_FAIL(read_text_value(allocator, value, outcome.value_))) {
          } else if (OB_FAIL(outcomes.push_back(outcome))) {
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  // all columns of the request become visible together
  for (int64_t i = 0; OB_SUCC(ret) && complete && i < outcomes.count(); ++i) {
    const Outcome &outcome = outcomes.at(i);
    const ObColumnSchemaV2 *column = table_schema.get_column_schema(outcome.column_id_);
    int64_t affected_rows = 0;
    sql.reset();
    if (OB_ISNULL(column)) {
    } else if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET `%.*s` = ",
                                      database_name.length(), database_name.ptr(),
                                      table_schema.get_table_name_str().length(),
                                      table_schema.get_table_name_str().ptr(),
                                      column->get_column_name_str().length(),
                                      column->get_column_name_str().ptr()))) {
    } else if (outcome.ready_ && outcome.has_value_ ? OB_FAIL(ObAIColumnUtil::append_quoted(sql, outcome.value_))
                                                     : OB_FAIL(sql.append("NULL"))) {
    } else if (OB_FAIL(sql.append_fmt(", `%s%.*s` = %d WHERE (%.*s) AND `%s%.*s` = ",
                                      ObAIColumnUtil::STATE_PREFIX,
                                      column->get_column_name_str().length(),
                                      column->get_column_name_str().ptr(),
                                      outcome.ready_ ? AI_ROW_READY : AI_ROW_FAILED,
                                      key.rowkey_predicate_.length(), key.rowkey_predicate_.ptr(),
                                      ObAIColumnUtil::REQUEST_PREFIX,
                                      column->get_column_name_str().length(),
                                      column->get_column_name_str().ptr()))) {
    } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, key.request_id_))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
      LOG_WARN("failed to publish AI column result", K(ret), K(sql));
    }
  }
  if (OB_SUCC(ret) && complete && !outcomes.empty()) {
    int64_t affected_rows = 0;
    sql.reset();
    if (OB_FAIL(sql.append("DELETE FROM oceanbase.__all_ai_column_task"))) {
    } else if (OB_FAIL(append_group_condition(key, sql))) {
    } else if (OB_FAIL(sql.append_fmt(" AND state = %d", AI_TASK_DONE))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    }
  }
  if (trans.is_started()) {
    int tmp_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to end AI column publish transaction", K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
  if (OB_FAIL(ret) && complete && !outcomes.empty() && retry_on_error) {
    // a result the column cannot store fails the request instead of blocking it forever
    ObSqlString fail_sql;
    int64_t affected_rows = 0;
    if (OB_FAIL(fail_sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET result = NULL,"
                                    " result_state = %d", AI_ROW_FAILED))) {
    } else if (OB_FAIL(append_group_condition(key, fail_sql))) {
    } else if (OB_FAIL(GCTX.sql_proxy_->write(fail_sql.ptr(), affected_rows))) {
    } else if (OB_FAIL(try_publish(key, database_name, table_schema, false))) {
    }
  }
  return ret;
}

int ObAIColumnWorker::drop_claimed_tasks(const GroupKey &key, const ObString &token)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append("DELETE FROM oceanbase.__all_ai_column_task"))) {
  } else if (OB_FAIL(append_group_condition(key, sql))) {
  } else if (OB_FAIL(sql.append(" AND claim_token = "))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, token))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
  }
  return ret;
}

int ObAIColumnWorker::recover_claims(const bool all)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state = %d, claim_token = NULL,"
                             " claim_ts = NULL WHERE state = %d", AI_TASK_PENDING, AI_TASK_RUNNING))) {
  } else if (!all && OB_FAIL(sql.append_fmt(" AND claim_ts < DATE_SUB(now(6), INTERVAL %ld SECOND)",
                                            CLAIM_TIMEOUT_S))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
  } else if (affected_rows > 0) {
    LOG_INFO("released AI column task claims", K(affected_rows), K(all));
  }
  return ret;
}

int ObAIColumnWorker::clean_orphans()
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_FAIL(GCTX.sql_proxy_->write("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id NOT IN"
                                     " (SELECT table_id FROM oceanbase.__all_table)", affected_rows))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write("DELETE FROM oceanbase.__all_ai_column WHERE table_id NOT IN"
                                            " (SELECT table_id FROM oceanbase.__all_table)", affected_rows))) {
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
