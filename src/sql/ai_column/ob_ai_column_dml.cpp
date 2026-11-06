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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/ai_column/ob_ai_column_dml.h"
#include "sql/ai_column/ob_ai_column_util.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

namespace
{
// Positions of the AI column helpers and primary key columns in the rows of one DML operator.
struct ObAIColumnDMLCtx
{
  bool valid_;
  uint64_t table_id_;
  int64_t column_cnt_;
  uint64_t *column_ids_;
  int64_t *request_idx_;
  int64_t *max_retries_;
  int64_t pk_cnt_;
  ObString *pk_names_;
  int64_t *pk_idx_;
};

int64_t find_row_idx(const UIntFixedArray &column_ids, const uint64_t column_id)
{
  int64_t idx = -1;
  for (int64_t i = 0; idx < 0 && i < column_ids.count(); ++i) {
    if (column_ids.at(i) == column_id) {
      idx = i;
    }
  }
  return idx;
}

int build_ctx(ObTableModifyOp &op, const ObDMLBaseCtDef &dml_ctdef, ObAIColumnDMLCtx *&ctx)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = op.get_exec_ctx().get_allocator();
  ObSchemaGetterGuard *schema_guard = nullptr;
  const ObTableSchema *table_schema = nullptr;
  ObSEArray<ObAIColumnSpec, 4> specs;
  ObSEArray<const ObColumnSchemaV2 *, 4> pk_columns;
  void *buf = nullptr;
  ctx = nullptr;
  if (OB_ISNULL(op.get_exec_ctx().get_sql_ctx())
      || OB_ISNULL(schema_guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard->get_table_schema(dml_ctdef.das_base_ctdef_.index_tid_, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(ObAIColumnUtil::get_all_specs(allocator, *table_schema, specs))) {
  } else if (OB_ISNULL(buf = allocator.alloc(sizeof(ObAIColumnDMLCtx)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ctx = new (buf) ObAIColumnDMLCtx();
    ctx->valid_ = true;
    ctx->table_id_ = table_schema->get_table_id();
    ctx->column_cnt_ = specs.count();
    // the primary key identifies the row in task predicates; heap tables flag it on the columns
    for (ObTableSchema::const_column_iterator iter = table_schema->column_begin();
         OB_SUCC(ret) && iter != table_schema->column_end(); ++iter) {
      if (OB_NOT_NULL(*iter) && (*iter)->is_heap_table_primary_key_column()
          && OB_FAIL(pk_columns.push_back(*iter))) {
      }
    }
    const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
    for (int64_t i = 0; OB_SUCC(ret) && pk_columns.empty() && i < rowkey_info.get_size(); ++i) {
      uint64_t column_id = OB_INVALID_ID;
      const ObColumnSchemaV2 *column = nullptr;
      if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
      } else if (OB_ISNULL(column = table_schema->get_column_schema(column_id))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(pk_columns.push_back(column))) {
      }
    }
    ctx->pk_cnt_ = pk_columns.count();
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(ctx->column_ids_ = static_cast<uint64_t *>(allocator.alloc(sizeof(uint64_t) * (ctx->column_cnt_ + 1))))
               || OB_ISNULL(ctx->request_idx_ = static_cast<int64_t *>(allocator.alloc(sizeof(int64_t) * (ctx->column_cnt_ + 1))))
               || OB_ISNULL(ctx->max_retries_ = static_cast<int64_t *>(allocator.alloc(sizeof(int64_t) * (ctx->column_cnt_ + 1))))
               || OB_ISNULL(ctx->pk_names_ = static_cast<ObString *>(allocator.alloc(sizeof(ObString) * (ctx->pk_cnt_ + 1))))
               || OB_ISNULL(ctx->pk_idx_ = static_cast<int64_t *>(allocator.alloc(sizeof(int64_t) * (ctx->pk_cnt_ + 1))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ctx->column_cnt_; ++i) {
      const ObColumnSchemaV2 *result_column = table_schema->get_column_schema(specs.at(i).column_name_);
      const ObColumnSchemaV2 *request_column = nullptr;
      ObString request_name;
      if (OB_ISNULL(result_column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIColumnUtil::build_helper_name(allocator, ObAIColumnUtil::REQUEST_PREFIX,
                                                           specs.at(i).column_name_, request_name))) {
      } else if (OB_ISNULL(request_column = table_schema->get_column_schema(request_name))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ctx->column_ids_[i] = result_column->get_column_id();
        ctx->request_idx_[i] = find_row_idx(dml_ctdef.column_ids_, request_column->get_column_id());
        ctx->max_retries_[i] = specs.at(i).max_retries_;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ctx->pk_cnt_; ++i) {
      new (&ctx->pk_names_[i]) ObString();
      if (OB_FAIL(ob_write_string(allocator, pk_columns.at(i)->get_column_name_str(), ctx->pk_names_[i]))) {
      } else if ((ctx->pk_idx_[i] = find_row_idx(dml_ctdef.column_ids_, pk_columns.at(i)->get_column_id())) < 0) {
        ctx->valid_ = false;
        LOG_WARN("primary key column is not in the DML row, AI column tasks are skipped",
                 K(dml_ctdef.column_ids_), "column", pk_columns.at(i)->get_column_name_str());
      }
    }
  }
  return ret;
}

int append_predicate(ObTableModifyOp &op, const ObAIColumnDMLCtx &ctx, const ObExprPtrIArray &row,
                     ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnPred");
  ObSqlString predicate;
  for (int64_t i = 0; OB_SUCC(ret) && i < ctx.pk_cnt_; ++i) {
    ObDatum *datum = nullptr;
    ObObj obj;
    char *buf = nullptr;
    int64_t len = 0;
    int64_t pos = 0;
    const ObExpr *expr = row.at(ctx.pk_idx_[i]);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr->eval(op.get_eval_ctx(), datum))) {
    } else if (OB_FAIL(datum->to_obj(obj, expr->obj_meta_))) {
    } else if (OB_FAIL(obj.print_sql_literal(buf, len, pos, allocator, op.get_obj_print_params()))) {
    } else if (OB_FAIL(predicate.append_fmt("%s`%.*s` = %.*s", i > 0 ? " and " : "",
                                            ctx.pk_names_[i].length(), ctx.pk_names_[i].ptr(),
                                            static_cast<int>(pos), buf))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ObAIColumnUtil::append_quoted(sql, predicate.string()))) {
  }
  return ret;
}

int get_request(ObTableModifyOp &op, const ObExprPtrIArray &row, const int64_t idx, ObString &request)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;
  request.reset();
  if (idx < 0 || idx >= row.count() || OB_ISNULL(row.at(idx))) {
  } else if (OB_FAIL(row.at(idx)->eval(op.get_eval_ctx(), datum))) {
  } else if (!datum->is_null()) {
    request = datum->get_string();
  }
  return ret;
}

int append_task(ObTableModifyOp &op, const ObAIColumnDMLCtx &ctx, const int64_t col_idx,
                const ObExprPtrIArray &row, const ObString &request, const bool first, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (first && OB_FAIL(sql.append("INSERT INTO oceanbase.__all_ai_column_task (table_id, column_id,"
                                  " rowkey_predicate, request_id, state, retry_count, max_retries,"
                                  " eligible_ts) VALUES "))) {
  } else if (OB_FAIL(sql.append_fmt("%s(%lu, %lu, ", first ? "" : ", ", ctx.table_id_,
                                    ctx.column_ids_[col_idx]))) {
  } else if (OB_FAIL(append_predicate(op, ctx, row, sql))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_quoted(sql, request))) {
  } else if (OB_FAIL(sql.append_fmt(", %d, 0, %ld, now(6))", AI_TASK_PENDING, ctx.max_retries_[col_idx]))) {
  }
  return ret;
}

int finish_upsert(ObSqlString &sql)
{
  // a newer change of the same row replaces the pending task of that row
  return sql.append(" ON DUPLICATE KEY UPDATE request_id = VALUES(request_id), state = VALUES(state),"
                    " retry_count = 0, max_retries = VALUES(max_retries), eligible_ts = VALUES(eligible_ts),"
                    " claim_token = NULL, claim_ts = NULL, result = NULL, result_state = NULL,"
                    " last_error = NULL");
}

int execute(ObTableModifyOp &op, const ObIArray<ObSqlString *> &stmts)
{
  int ret = OB_SUCCESS;
  ObDASCtx &das_ctx = op.get_exec_ctx().get_das_ctx();
  // like foreign key cascades, the task statements run as nested sql of the user statement
  const bool save_fk_cascading = das_ctx.is_fk_cascading_;
  if (stmts.empty()) {
  } else if (FALSE_IT(das_ctx.is_fk_cascading_ = true)) {
  } else if (OB_FAIL(op.begin_nested_session(false))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < stmts.count(); ++i) {
      if (!stmts.at(i)->empty() && OB_FAIL(op.execute_write(stmts.at(i)->ptr()))) {
        LOG_WARN("failed to maintain AI column task", K(ret), "sql", stmts.at(i)->string());
      }
    }
    int end_ret = op.end_nested_session();
    if (OB_SUCCESS != end_ret) {
      LOG_WARN("failed to end nested session", K(end_ret));
      ret = (OB_SUCCESS == ret) ? end_ret : ret;
    }
  }
  das_ctx.is_fk_cascading_ = save_fk_cascading;
  return ret;
}
} // namespace

int ObAIColumnDMLHandle::do_handle(ObTableModifyOp &op,
                                   const ObDMLBaseCtDef &dml_ctdef,
                                   ObDMLBaseRtDef &dml_rtdef,
                                   const ObDmlEventType dml_event)
{
  int ret = OB_SUCCESS;
  ObAIColumnDMLCtx *ctx = static_cast<ObAIColumnDMLCtx *>(dml_rtdef.ai_column_ctx_);
  ObSqlString delete_sql;
  ObSqlString upsert_sql;
  ObSEArray<ObSqlString *, 2> stmts;
  const ObExprPtrIArray &old_row = dml_ctdef.old_row_;
  const ObExprPtrIArray &new_row = dml_ctdef.new_row_;
  if (OB_ISNULL(ctx)) {
    if (OB_FAIL(build_ctx(op, dml_ctdef, ctx))) {
    } else {
      dml_rtdef.ai_column_ctx_ = ctx;
    }
  }
  if (OB_FAIL(ret) || OB_ISNULL(ctx) || !ctx->valid_) {
  } else if (DE_INSERTING == dml_event) {
    bool first = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < ctx->column_cnt_; ++i) {
      ObString request;
      if (OB_FAIL(get_request(op, new_row, ctx->request_idx_[i], request))) {
      } else if (request.empty()) {
      } else if (OB_FAIL(append_task(op, *ctx, i, new_row, request, first, upsert_sql))) {
      } else {
        first = false;
      }
    }
  } else if (DE_UPDATING == dml_event) {
    bool first = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < ctx->column_cnt_; ++i) {
      ObString old_request;
      ObString new_request;
      if (OB_FAIL(get_request(op, old_row, ctx->request_idx_[i], old_request))) {
      } else if (OB_FAIL(get_request(op, new_row, ctx->request_idx_[i], new_request))) {
      } else if (new_request.empty() || 0 == old_request.compare(new_request)) {
        // the change does not invalidate this AI column
      } else {
        // the task of the old row version goes away; the primary key may have changed
        if (delete_sql.empty()
            && OB_FAIL(delete_sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task"
                                             " WHERE table_id = %lu AND (", ctx->table_id_))) {
        } else if (OB_FAIL(delete_sql.append_fmt("%s(column_id = %lu AND rowkey_predicate = ",
                                                 first ? "" : " OR ", ctx->column_ids_[i]))) {
        } else if (OB_FAIL(append_predicate(op, *ctx, old_row, delete_sql))) {
        } else if (OB_FAIL(delete_sql.append(")"))) {
        } else if (OB_FAIL(append_task(op, *ctx, i, new_row, new_request, first, upsert_sql))) {
        } else {
          first = false;
        }
      }
    }
    if (OB_SUCC(ret) && !delete_sql.empty() && OB_FAIL(delete_sql.append(")"))) {
    }
  } else if (DE_DELETING == dml_event) {
    if (OB_FAIL(delete_sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu"
                                      " AND rowkey_predicate = ", ctx->table_id_))) {
    } else if (OB_FAIL(append_predicate(op, *ctx, old_row, delete_sql))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!upsert_sql.empty() && OB_FAIL(finish_upsert(upsert_sql))) {
  } else if (!delete_sql.empty() && OB_FAIL(stmts.push_back(&delete_sql))) {
  } else if (!upsert_sql.empty() && OB_FAIL(stmts.push_back(&upsert_sql))) {
  } else if (OB_FAIL(execute(op, stmts))) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
