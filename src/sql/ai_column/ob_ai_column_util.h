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

#ifndef OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_UTIL_H_
#define OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_UTIL_H_

#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;
}
}
namespace sql
{

// An AI column is a visible result column plus three hidden helper columns:
//   __ai_state_<col>  row state (ObAIColumnRowState)
//   __ai_req_<col>    request id of the change that last invalidated the result
//   __ai_spec_<col>   definition version used by that request; its column comment
//                     stores the AI column definition as JSON.
struct ObAIColumnSpec
{
  ObAIColumnSpec() : max_retries_(0), version_(1) {}
  TO_STRING_KV(K_(column_name), K_(function_name), K_(model_name), K_(expr), K_(args), K_(deps),
               K_(sync_mode), K_(max_retries), K_(version));
  common::ObString column_name_;
  common::ObString function_name_;   // AI_COMPLETE or AI_EMBED, upper case
  common::ObString model_name_;
  common::ObString expr_;            // expression text as written by the user
  common::ObSEArray<common::ObString, 4> args_;  // top-level argument texts
  common::ObSEArray<common::ObString, 4> deps_;  // referenced column names
  common::ObString sync_mode_;
  int64_t max_retries_;
  int64_t version_;
};

enum ObAIColumnRowState
{
  AI_ROW_PENDING = 0,
  AI_ROW_READY = 1,
  AI_ROW_FAILED = 2,
};

enum ObAIColumnTaskState
{
  AI_TASK_PENDING = 0,
  AI_TASK_RUNNING = 1,
  AI_TASK_DONE = 2,        // finished, waiting for the other columns of the same request
  AI_TASK_RETRY_WAIT = 3,
};

class ObAIColumnUtil
{
public:
  static constexpr const char *STATE_PREFIX = "__ai_state_";
  static constexpr const char *REQUEST_PREFIX = "__ai_req_";
  static constexpr const char *SPEC_PREFIX = "__ai_spec_";
  static constexpr const char *FUNC_COMPLETE = "AI_COMPLETE";
  static constexpr const char *FUNC_EMBED = "AI_EMBED";
  static constexpr const char *SYNC_MODE_ASYNC = "async";
  static const int64_t DEFAULT_MAX_RETRIES = 3;
  static const int64_t MAX_MAX_RETRIES = 100;
  static const int64_t REQUEST_ID_LENGTH = 64;

  static bool is_helper_column_name(const common::ObString &name);
  static int build_helper_name(common::ObIAllocator &allocator, const char *prefix,
                               const common::ObString &column_name, common::ObString &name);
  // Splits "FUNC(arg, ...)" into the function name, its arguments and the identifiers it uses.
  static int parse_expr(common::ObIAllocator &allocator, const common::ObString &expr,
                        ObAIColumnSpec &spec);
  static int spec_to_json(common::ObIAllocator &allocator, const ObAIColumnSpec &spec,
                          common::ObString &json);
  static int spec_from_json(common::ObIAllocator &allocator, const common::ObString &json,
                            ObAIColumnSpec &spec);
  static bool has_ai_column(const share::schema::ObTableSchema &table_schema);
  // is_ai is false when column_name is not an AI result column.
  static int get_spec(common::ObIAllocator &allocator,
                      const share::schema::ObTableSchema &table_schema,
                      const common::ObString &column_name,
                      bool &is_ai,
                      ObAIColumnSpec *spec = nullptr);
  static int get_all_specs(common::ObIAllocator &allocator,
                           const share::schema::ObTableSchema &table_schema,
                           common::ObIArray<ObAIColumnSpec> &specs);
  // Instruction that makes a completion parseable as the result column type, or NULL.
  static const char *get_type_instruction(const share::schema::ObColumnSchemaV2 &column);
  // The expression the worker evaluates, with the type instruction made explicit.
  static int build_normalized_expr(common::ObIAllocator &allocator, const ObAIColumnSpec &spec,
                                   const share::schema::ObColumnSchemaV2 &column,
                                   common::ObString &expr);
  // Replaces the unquoted STATUS pseudo column with the textual state of state_column.
  static int rewrite_status_predicate(common::ObIAllocator &allocator,
                                      const common::ObString &predicate,
                                      const common::ObString &state_column,
                                      common::ObString &result);
  // Writes the AI column definitions found in schema (all of them, or only the named columns)
  // to __all_ai_column, looking up the column ids of the committed table.
  static int sync_metadata(const common::ObString &database_name,
                           const common::ObString &table_name,
                           const share::schema::ObTableSchema &schema,
                           const common::ObIArray<common::ObString> *columns);
  // Requests generation for the rows that existed before the AI columns were added.
  static int backfill(const common::ObString &database_name,
                      const common::ObString &table_name,
                      const common::ObIArray<common::ObString> &columns);
  // ALTER TABLE ... RETRY AI COLUMN: invalidates the selected rows of one AI column.
  static int retry(const common::ObString &database_name,
                   const common::ObString &table_name, const ObAIColumnSpec &spec,
                   const common::ObString &where, int64_t &affected_rows);
  // ai_column_strict_read: fails when a row of the column is not READY.
  static int check_column_ready(const common::ObString &database_name,
                                const common::ObString &table_name,
                                const common::ObString &column_name,
                                const uint64_t table_id, const uint64_t column_id);
  static int append_quoted(common::ObSqlString &sql, const common::ObString &value);
  static bool is_identifier_char(const char c);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_UTIL_H_
