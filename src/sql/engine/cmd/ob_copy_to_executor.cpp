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
#include "sql/engine/cmd/ob_copy_to_executor.h"
#include "sql/resolver/cmd/ob_copy_to_stmt.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/object/ob_obj_cast.h"
#include "query/api/query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/time/ob_time_utility.h"
#include <fcntl.h>
#include <unistd.h>

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
namespace sql
{

int ObCopyToExecutor::append_field(const ObCopyToStmt &stmt, const ObString &value, ObSqlString &line)
{
  int ret = OB_SUCCESS;
  bool need_quote = value.empty() || value == stmt.null_str_;
  for (int64_t i = 0; !need_quote && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    need_quote = (c == stmt.delim_ || c == stmt.quote_ || c == stmt.escape_ || '\n' == c || '\r' == c);
  }
  if (!need_quote) {
    ret = line.append(value);
  } else if (OB_FAIL(line.append_fmt("%c", stmt.quote_))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      if (c == stmt.quote_ || (c == stmt.escape_ && stmt.escape_ != stmt.quote_)) {
        ret = line.append_fmt("%c%c", stmt.escape_, c);
      } else {
        ret = line.append_fmt("%c", c);
      }
    }
    if (OB_SUCC(ret)) {
      ret = line.append_fmt("%c", stmt.quote_);
    }
  }
  return ret;
}

int ObCopyToExecutor::write_all(const int fd, const ObSqlString &data)
{
  int ret = OB_SUCCESS;
  int64_t done = 0;
  while (OB_SUCC(ret) && done < data.length()) {
    const ssize_t n = ::write(fd, data.ptr() + done, data.length() - done);
    if (n < 0 && EINTR == errno) {
    } else if (n <= 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to write the COPY file", K(ret), K(errno));
    } else {
      done += n;
    }
  }
  return ret;
}

int ObCopyToExecutor::execute(ObExecContext &ctx, ObCopyToStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  ObSqlString tmp_path;
  ObSqlString buf;
  int fd = -1;
  bool tmp_created = false;
  int64_t rows = 0;
  const int64_t FLUSH_SIZE = 1L << 20;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(tmp_path.append_fmt("%.*s.tmp.%d.%ld", stmt.path_.length(), stmt.path_.ptr(),
                                         static_cast<int>(getpid()), ObTimeUtility::current_time()))) {
  } else if ((fd = ::open(tmp_path.ptr(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0) {
    ret = OB_IO_ERROR;
    LOG_WARN("failed to create the COPY file", K(ret), K(tmp_path), K(errno));
  } else {
    tmp_created = true;
  }
  for (int64_t i = 0; OB_SUCC(ret) && stmt.header_ && i < stmt.column_names_.count(); ++i) {
    if (i > 0 && OB_FAIL(buf.append_fmt("%c", stmt.delim_))) {
    } else if (OB_FAIL(append_field(stmt, stmt.column_names_.at(i), buf))) {
    }
  }
  if (OB_SUCC(ret) && stmt.header_ && OB_FAIL(buf.append("\n"))) {
  }
  if (OB_SUCC(ret)) {
    ObISQLConnectionGuard conn_guard;
    ObISQLConnection *conn = NULL;
    const ObDataTypeCastParams dtc_params = session->get_dtc_params();
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, conn_guard))) {
    } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        ObMySQLResult *result = NULL;
        ObArenaAllocator row_allocator("FileSqlCopy");
        if (OB_FAIL(conn->execute_read(stmt.query_sql_, res, true))) {
          LOG_WARN("failed to run the query of COPY", K(ret), K(stmt.query_sql_));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        }
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          row_allocator.reuse();
          for (int64_t i = 0; OB_SUCC(ret) && i < result->get_column_count(); ++i) {
            ObObj obj;
            ObString text;
            bool is_null = false;
            if (i > 0 && OB_FAIL(buf.append_fmt("%c", stmt.delim_))) {
            } else if (OB_FAIL(result->get_obj(i, obj, NULL, &row_allocator))) {
            } else if (obj.is_null()) {
              is_null = true;
            } else if (ob_is_string_type(obj.get_type()) && obj.has_lob_header()) {
              text = obj.get_string();
              ret = ObTextStringHelper::read_real_string_data(ctx, &row_allocator, obj, text);
            } else if (ob_is_string_type(obj.get_type())) {
              text = obj.get_string();
            } else {
              ObCastCtx cast_ctx(&row_allocator, &dtc_params, CM_NONE, CS_TYPE_UTF8MB4_BIN);
              ObObj text_obj;
              if (OB_FAIL(ObObjCaster::to_type(ObVarcharType, cast_ctx, obj, text_obj))) {
                LOG_WARN("failed to format a value", K(ret), K(obj));
              } else {
                text = text_obj.get_string();
              }
            }
            if (OB_FAIL(ret)) {
            } else if (is_null) {
              ret = buf.append(stmt.null_str_);
            } else {
              ret = append_field(stmt, text, buf);
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(buf.append("\n"))) {
          } else if (OB_SUCC(ret) && buf.length() >= FLUSH_SIZE) {
            if (OB_SUCC(write_all(fd, buf))) {
              buf.reuse();
            }
          }
          ++rows;
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(write_all(fd, buf))) {
  } else if (OB_SUCC(ret) && 0 != ::fsync(fd)) {
    ret = OB_IO_ERROR;
  }
  if (fd >= 0) {
    if (0 != ::close(fd) && OB_SUCC(ret)) {
      ret = OB_IO_ERROR;
    }
  }
  // link fails if the target appeared meanwhile, the target is never replaced
  if (OB_SUCC(ret) && 0 != ::link(tmp_path.ptr(), stmt.path_.ptr())) {
    ret = EEXIST == errno ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR;
    LOG_WARN("failed to create the COPY target", K(ret), K(stmt.path_), K(errno));
  }
  // the temporary file is removed whether or not the target was created
  if (tmp_created) {
    ::unlink(tmp_path.ptr());
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(ctx.get_physical_plan_ctx())) {
    ctx.get_physical_plan_ctx()->set_affected_rows(rows);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
