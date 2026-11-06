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

#define USING_LOG_PREFIX SQL
#include "sql/ai_column/ob_ai_column_util.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/charset/ob_charset.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ob_server_struct.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

bool ObAIColumnUtil::is_identifier_char(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
         || '_' == c || '$' == c || (static_cast<unsigned char>(c) >= 0x80);
}

bool ObAIColumnUtil::is_helper_column_name(const ObString &name)
{
  return name.prefix_match_ci(STATE_PREFIX)
         || name.prefix_match_ci(REQUEST_PREFIX)
         || name.prefix_match_ci(SPEC_PREFIX);
}

int ObAIColumnUtil::build_helper_name(ObIAllocator &allocator, const char *prefix,
                                      const ObString &column_name, ObString &name)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  if (OB_FAIL(buf.append_fmt("%s%.*s", prefix, column_name.length(), column_name.ptr()))) {
  } else if (buf.length() > OB_MAX_COLUMN_NAME_LENGTH) {
    ret = OB_ERR_TOO_LONG_IDENT;
    LOG_USER_ERROR(OB_ERR_TOO_LONG_IDENT, column_name.length(), column_name.ptr());
  } else if (OB_FAIL(ob_write_string(allocator, buf.string(), name))) {
  }
  return ret;
}

namespace
{
// Returns the position after a quoted token that starts at pos, honoring backslash escapes
// and doubled quote characters.
int64_t skip_quoted(const ObString &text, int64_t pos)
{
  const char quote = text.ptr()[pos];
  int64_t i = pos + 1;
  while (i < text.length()) {
    const char c = text.ptr()[i];
    if ('\\' == c && '`' != quote) {
      i += 2;
    } else if (c == quote) {
      if (i + 1 < text.length() && text.ptr()[i + 1] == quote) {
        i += 2;
      } else {
        return i + 1;
      }
    } else {
      ++i;
    }
  }
  return text.length();
}

ObString trim(const ObString &str)
{
  ObString res = str;
  return res.trim();
}

int unquote_string_literal(ObIAllocator &allocator, const ObString &literal, ObString &value)
{
  int ret = OB_SUCCESS;
  ObString text = trim(literal);
  if (text.length() < 2 || ('\'' != text.ptr()[0] && '"' != text.ptr()[0])
      || text.ptr()[text.length() - 1] != text.ptr()[0]) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char quote = text.ptr()[0];
    ObSqlString buf;
    for (int64_t i = 1; OB_SUCC(ret) && i < text.length() - 1; ++i) {
      char c = text.ptr()[i];
      if ('\\' == c && i + 1 < text.length() - 1) {
        c = text.ptr()[++i];
      } else if (c == quote && i + 1 < text.length() - 1 && text.ptr()[i + 1] == quote) {
        ++i;
      }
      if (OB_FAIL(buf.append(&c, 1))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, buf.string(), value))) {
    }
  }
  return ret;
}

int append_json_string(ObSqlString &buf, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(buf.append("\""))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value.ptr()[i]);
    if ('"' == c || '\\' == c) {
      ret = buf.append_fmt("\\%c", c);
    } else if ('\n' == c) {
      ret = buf.append("\\n");
    } else if ('\r' == c) {
      ret = buf.append("\\r");
    } else if ('\t' == c) {
      ret = buf.append("\\t");
    } else if (c < 0x20) {
      ret = buf.append_fmt("\\u%04x", c);
    } else {
      ret = buf.append(reinterpret_cast<const char *>(&c), 1);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(buf.append("\""))) {
  }
  return ret;
}

int get_json_string(ObIAllocator &allocator, ObJsonObject &obj, const char *key,
                    ObString &value)
{
  int ret = OB_SUCCESS;
  ObJsonNode *node = obj.get_value(ObString::make_string(key));
  if (OB_ISNULL(node) || ObJsonNodeType::J_STRING != node->json_type()) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ob_write_string(allocator,
                                     ObString(node->get_data_length(), node->get_data()),
                                     value))) {
  }
  return ret;
}

int get_json_int(ObJsonObject &obj, const char *key, int64_t &value)
{
  int ret = OB_SUCCESS;
  ObJsonNode *node = obj.get_value(ObString::make_string(key));
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_DATA;
  } else if (ObJsonNodeType::J_INT == node->json_type()) {
    value = node->get_int();
  } else if (ObJsonNodeType::J_UINT == node->json_type()) {
    value = static_cast<int64_t>(node->get_uint());
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

int get_json_string_array(ObIAllocator &allocator, ObJsonObject &obj, const char *key,
                          ObIArray<ObString> &values)
{
  int ret = OB_SUCCESS;
  ObJsonNode *node = obj.get_value(ObString::make_string(key));
  if (OB_ISNULL(node) || ObJsonNodeType::J_ARRAY != node->json_type()) {
    ret = OB_INVALID_DATA;
  } else {
    for (uint64_t i = 0; OB_SUCC(ret) && i < node->element_count(); ++i) {
      ObJsonNode *item = static_cast<ObJsonArray *>(node)->get_value(i);
      ObString value;
      if (OB_ISNULL(item) || ObJsonNodeType::J_STRING != item->json_type()) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(ob_write_string(allocator,
                                         ObString(item->get_data_length(), item->get_data()),
                                         value))) {
      } else if (OB_FAIL(values.push_back(value))) {
      }
    }
  }
  return ret;
}
} // namespace

int ObAIColumnUtil::parse_expr(ObIAllocator &allocator, const ObString &expr, ObAIColumnSpec &spec)
{
  int ret = OB_SUCCESS;
  const ObString text = trim(expr);
  int64_t open_pos = -1;
  for (int64_t i = 0; i < text.length(); ++i) {
    if ('(' == text.ptr()[i]) {
      open_pos = i;
      break;
    }
  }
  spec.args_.reset();
  spec.deps_.reset();
  if (open_pos <= 0 || ')' != text.ptr()[text.length() - 1]) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI column expression other than AI_COMPLETE or AI_EMBED");
  } else {
    ObString func_name = trim(ObString(open_pos, text.ptr()));
    if (0 == func_name.case_compare(FUNC_COMPLETE)) {
      spec.function_name_ = ObString::make_string(FUNC_COMPLETE);
    } else if (0 == func_name.case_compare(FUNC_EMBED)) {
      spec.function_name_ = ObString::make_string(FUNC_EMBED);
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI column expression other than AI_COMPLETE or AI_EMBED");
    }
  }
  // Split the top-level arguments and collect identifiers that are not function names.
  int64_t depth = 0;
  int64_t arg_start = open_pos + 1;
  const int64_t body_end = text.length() - 1;
  for (int64_t i = open_pos + 1; OB_SUCC(ret) && i <= body_end; ) {
    const char c = text.ptr()[i];
    if ('\'' == c || '"' == c) {
      i = skip_quoted(text, i);
    } else if ('`' == c) {
      const int64_t end = skip_quoted(text, i);
      ObString name;
      if (end - i >= 2 && OB_FAIL(ob_write_string(allocator, ObString(end - i - 2, text.ptr() + i + 1), name))) {
      } else if (end - i >= 2 && OB_FAIL(spec.deps_.push_back(name))) {
      }
      i = end;
    } else if ('(' == c) {
      ++depth;
      ++i;
    } else if (')' == c && i < body_end) {
      --depth;
      ++i;
    } else if ((',' == c && 0 == depth) || i == body_end) {
      ObString arg = trim(ObString(i - arg_start, text.ptr() + arg_start));
      ObString copied;
      if (arg.empty()) {
        if (i != body_end || spec.args_.count() > 0) {
          ret = OB_ERR_PARSER_SYNTAX;
        }
      } else if (OB_FAIL(ob_write_string(allocator, arg, copied))) {
      } else if (OB_FAIL(spec.args_.push_back(copied))) {
      }
      arg_start = i + 1;
      ++i;
    } else if (is_identifier_char(c) && !(c >= '0' && c <= '9')) {
      int64_t end = i;
      while (end < body_end && is_identifier_char(text.ptr()[end])) {
        ++end;
      }
      int64_t next = end;
      while (next < body_end && (' ' == text.ptr()[next] || '\t' == text.ptr()[next]
                                 || '\n' == text.ptr()[next])) {
        ++next;
      }
      const bool is_function = (next < text.length() && '(' == text.ptr()[next]);
      const bool is_qualifier = (end < body_end && '.' == text.ptr()[end]);
      ObString name;
      if (!is_function && !is_qualifier) {
        if (OB_FAIL(ob_write_string(allocator, ObString(end - i, text.ptr() + i), name))) {
        } else if (OB_FAIL(spec.deps_.push_back(name))) {
        }
      }
      i = end;
    } else {
      ++i;
    }
  }
  if (OB_SUCC(ret)) {
    if (spec.args_.count() < 2 || spec.args_.count() > 3) {
      ret = OB_ERR_PARAM_SIZE;
      LOG_USER_ERROR(OB_ERR_PARAM_SIZE, spec.function_name_.length(), spec.function_name_.ptr());
    } else if (OB_FAIL(unquote_string_literal(allocator, spec.args_.at(0), spec.model_name_))) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI column model name other than a string literal");
    } else if (OB_FAIL(ob_write_string(allocator, text, spec.expr_))) {
    }
  }
  return ret;
}

int ObAIColumnUtil::spec_to_json(ObIAllocator &allocator, const ObAIColumnSpec &spec, ObString &json)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  if (OB_FAIL(buf.append("{\"function\":"))) {
  } else if (OB_FAIL(append_json_string(buf, spec.function_name_))) {
  } else if (OB_FAIL(buf.append(",\"model\":"))) {
  } else if (OB_FAIL(append_json_string(buf, spec.model_name_))) {
  } else if (OB_FAIL(buf.append(",\"expr\":"))) {
  } else if (OB_FAIL(append_json_string(buf, spec.expr_))) {
  } else if (OB_FAIL(buf.append(",\"args\":["))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < spec.args_.count(); ++i) {
    if (i > 0 && OB_FAIL(buf.append(","))) {
    } else if (OB_FAIL(append_json_string(buf, spec.args_.at(i)))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(buf.append("],\"deps\":["))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < spec.deps_.count(); ++i) {
    if (i > 0 && OB_FAIL(buf.append(","))) {
    } else if (OB_FAIL(append_json_string(buf, spec.deps_.at(i)))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(buf.append("],\"sync_mode\":"))) {
  } else if (OB_FAIL(append_json_string(buf, spec.sync_mode_))) {
  } else if (OB_FAIL(buf.append_fmt(",\"max_retries\":%ld,\"version\":%ld}",
                                    spec.max_retries_, spec.version_))) {
  } else if (OB_FAIL(ob_write_string(allocator, buf.string(), json))) {
  }
  return ret;
}

int ObAIColumnUtil::spec_from_json(ObIAllocator &allocator, const ObString &json, ObAIColumnSpec &spec)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = nullptr;
  spec.args_.reset();
  spec.deps_.reset();
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, json, ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE, j_base))) {
    LOG_WARN("invalid AI column definition", K(ret), K(json));
  } else if (OB_ISNULL(j_base) || ObJsonNodeType::J_OBJECT != j_base->json_type()) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonObject &obj = *static_cast<ObJsonObject *>(j_base);
    if (OB_FAIL(get_json_string(allocator, obj, "function", spec.function_name_))) {
    } else if (OB_FAIL(get_json_string(allocator, obj, "model", spec.model_name_))) {
    } else if (OB_FAIL(get_json_string(allocator, obj, "expr", spec.expr_))) {
    } else if (OB_FAIL(get_json_string_array(allocator, obj, "args", spec.args_))) {
    } else if (OB_FAIL(get_json_string_array(allocator, obj, "deps", spec.deps_))) {
    } else if (OB_FAIL(get_json_string(allocator, obj, "sync_mode", spec.sync_mode_))) {
    } else if (OB_FAIL(get_json_int(obj, "max_retries", spec.max_retries_))) {
    } else if (OB_FAIL(get_json_int(obj, "version", spec.version_))) {
    }
  }
  return ret;
}

bool ObAIColumnUtil::has_ai_column(const ObTableSchema &table_schema)
{
  bool found = false;
  for (ObTableSchema::const_column_iterator iter = table_schema.column_begin();
       !found && iter != table_schema.column_end(); ++iter) {
    const ObColumnSchemaV2 *column = *iter;
    if (OB_NOT_NULL(column) && column->is_hidden()
        && column->get_column_name_str().prefix_match_ci(SPEC_PREFIX)) {
      found = true;
    }
  }
  return found;
}

int ObAIColumnUtil::get_spec(ObIAllocator &allocator, const ObTableSchema &table_schema,
                             const ObString &column_name, bool &is_ai, ObAIColumnSpec *spec)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_allocator("AIColumnSpec");
  ObString spec_name;
  const ObColumnSchemaV2 *spec_column = nullptr;
  is_ai = false;
  if (is_helper_column_name(column_name)) {
    // helper columns are never AI result columns
  } else if (OB_FAIL(build_helper_name(tmp_allocator, SPEC_PREFIX, column_name, spec_name))) {
    if (OB_ERR_TOO_LONG_IDENT == ret) {
      ret = OB_SUCCESS;
    }
  } else if (OB_ISNULL(spec_column = table_schema.get_column_schema(spec_name))
             || !spec_column->is_hidden()) {
  } else {
    is_ai = true;
    if (OB_NOT_NULL(spec)) {
      if (OB_FAIL(spec_from_json(allocator, spec_column->get_comment_str(), *spec))) {
      } else {
        const ObColumnSchemaV2 *result_column = table_schema.get_column_schema(column_name);
        spec->column_name_ = OB_NOT_NULL(result_column) ? result_column->get_column_name_str()
                                                        : column_name;
      }
    }
  }
  return ret;
}

int ObAIColumnUtil::get_all_specs(ObIAllocator &allocator, const ObTableSchema &table_schema,
                                  ObIArray<ObAIColumnSpec> &specs)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = STRLEN(SPEC_PREFIX);
  for (ObTableSchema::const_column_iterator iter = table_schema.column_begin();
       OB_SUCC(ret) && iter != table_schema.column_end(); ++iter) {
    const ObColumnSchemaV2 *column = *iter;
    if (OB_NOT_NULL(column) && column->is_hidden()
        && column->get_column_name_str().prefix_match_ci(SPEC_PREFIX)) {
      const ObString &name = column->get_column_name_str();
      ObString result_name(name.length() - prefix_len, name.ptr() + prefix_len);
      const ObColumnSchemaV2 *result_column = table_schema.get_column_schema(result_name);
      ObAIColumnSpec spec;
      if (OB_ISNULL(result_column)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("AI result column is missing", K(ret), K(name));
      } else if (OB_FAIL(spec_from_json(allocator, column->get_comment_str(), spec))) {
      } else if (FALSE_IT(spec.column_name_ = result_column->get_column_name_str())) {
      } else if (OB_FAIL(specs.push_back(spec))) {
      }
    }
  }
  return ret;
}

const char *ObAIColumnUtil::get_type_instruction(const ObColumnSchemaV2 &column)
{
  const char *instruction = nullptr;
  const ObObjType type = column.get_data_type();
  if (ob_is_json(type)) {
    instruction = "Return exactly one valid JSON value and nothing else.";
  } else if (ob_is_integer_type(type)) {
    instruction = "Return exactly one base-10 integer and nothing else.";
  } else if (ob_is_float_tc(type) || ob_is_double_tc(type) || ob_is_number_tc(type)
             || ob_is_decimal_int_tc(type)) {
    instruction = "Return exactly one decimal number and nothing else.";
  } else if (ob_is_date_tc(type) || ob_is_mysql_date_tc(type)) {
    instruction = "Return exactly one date in YYYY-MM-DD format and nothing else.";
  } else if (ob_is_datetime_tc(type) || ob_is_mysql_datetime_tc(type)) {
    instruction = "Return exactly one datetime in YYYY-MM-DD HH:MM:SS format and nothing else.";
  }
  return instruction;
}

int ObAIColumnUtil::build_normalized_expr(ObIAllocator &allocator, const ObAIColumnSpec &spec,
                                          const ObColumnSchemaV2 &column, ObString &expr)
{
  int ret = OB_SUCCESS;
  const char *instruction = (0 == spec.function_name_.case_compare(FUNC_COMPLETE))
                            ? get_type_instruction(column) : nullptr;
  if (OB_ISNULL(instruction)) {
    if (OB_FAIL(ob_write_string(allocator, spec.expr_, expr))) {
    }
  } else {
    ObSqlString config;
    ObSqlString buf;
    if (OB_FAIL(config.append("{\"system_prompt\":"))) {
    } else if (OB_FAIL(append_json_string(config, ObString::make_string(instruction)))) {
    } else if (OB_FAIL(config.append("}"))) {
    } else if (OB_FAIL(buf.append_fmt("%s(%.*s, %.*s, ", FUNC_COMPLETE,
                                      spec.args_.at(0).length(), spec.args_.at(0).ptr(),
                                      spec.args_.at(1).length(), spec.args_.at(1).ptr()))) {
    } else if (spec.args_.count() > 2) {
      if (OB_FAIL(buf.append_fmt("JSON_MERGE_PATCH(%.*s, ", spec.args_.at(2).length(),
                                 spec.args_.at(2).ptr()))) {
      } else if (OB_FAIL(append_quoted(buf, config.string()))) {
      } else if (OB_FAIL(buf.append(")"))) {
      }
    } else if (OB_FAIL(append_quoted(buf, config.string()))) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(buf.append(")"))) {
    } else if (OB_FAIL(ob_write_string(allocator, buf.string(), expr))) {
    }
  }
  return ret;
}

int ObAIColumnUtil::rewrite_status_predicate(ObIAllocator &allocator, const ObString &predicate,
                                             const ObString &state_column, ObString &result)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  int64_t copied = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < predicate.length(); ) {
    const char c = predicate.ptr()[i];
    if ('\'' == c || '"' == c || '`' == c) {
      i = skip_quoted(predicate, i);
    } else if (is_identifier_char(c)) {
      int64_t end = i;
      while (end < predicate.length() && is_identifier_char(predicate.ptr()[end])) {
        ++end;
      }
      const bool qualified = (i > 0 && '.' == predicate.ptr()[i - 1])
                             || (end < predicate.length() && '.' == predicate.ptr()[end]);
      if (!qualified && 0 == ObString(end - i, predicate.ptr() + i).case_compare("status")) {
        if (OB_FAIL(buf.append(predicate.ptr() + copied, i - copied))) {
        } else if (OB_FAIL(buf.append_fmt("(CASE `%.*s` WHEN %d THEN 'PENDING' WHEN %d THEN 'READY'"
                                          " WHEN %d THEN 'FAILED' END)",
                                          state_column.length(), state_column.ptr(),
                                          AI_ROW_PENDING, AI_ROW_READY, AI_ROW_FAILED))) {
        }
        copied = end;
      }
      i = end;
    } else {
      ++i;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(buf.append(predicate.ptr() + copied, predicate.length() - copied))) {
  } else if (OB_FAIL(ob_write_string(allocator, buf.string(), result))) {
  }
  return ret;
}

int ObAIColumnUtil::sync_metadata(const ObString &database_name,
                                  const ObString &table_name, const ObTableSchema &schema,
                                  const ObIArray<ObString> *columns)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnMeta");
  ObSEArray<ObAIColumnSpec, 4> specs;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(get_all_specs(allocator, schema, specs))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < specs.count(); ++i) {
    const ObAIColumnSpec &spec = specs.at(i);
    const ObColumnSchemaV2 *result_column = schema.get_column_schema(spec.column_name_);
    bool selected = OB_ISNULL(columns);
    for (int64_t j = 0; !selected && j < columns->count(); ++j) {
      selected = (0 == columns->at(j).case_compare(spec.column_name_));
    }
    ObString normalized;
    ObString helper_names[3];
    const char *prefixes[3] = {STATE_PREFIX, REQUEST_PREFIX, SPEC_PREFIX};
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (!selected) {
      continue;
    } else if (OB_ISNULL(result_column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(build_normalized_expr(allocator, spec, *result_column, normalized))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < 3; ++j) {
      if (OB_FAIL(build_helper_name(allocator, prefixes[j], spec.column_name_, helper_names[j]))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append("REPLACE INTO oceanbase.__all_ai_column (table_id, column_id, column_name,"
                                  " state_column_id, request_column_id, result_spec_column_id, function_name,"
                                  " model_name, sync_mode, max_retries, dependency_column_ids, result_type,"
                                  " original_expr, normalized_expr, definition_version)"
                                  " SELECT t.table_id, r.column_id, r.column_name, s.column_id, q.column_id,"
                                  " p.column_id, "))) {
    } else if (OB_FAIL(append_quoted(sql, spec.function_name_))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_quoted(sql, spec.model_name_))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_quoted(sql, spec.sync_mode_))) {
    } else if (OB_FAIL(sql.append_fmt(", %ld, IFNULL((SELECT GROUP_CONCAT(c.column_id ORDER BY c.column_id"
                                      " SEPARATOR ',') FROM oceanbase.__all_column c"
                                      " WHERE c.table_id = t.table_id AND c.column_name IN ('', ",
                                      spec.max_retries_))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < spec.deps_.count(); ++j) {
      if (j > 0 && OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(append_quoted(sql, spec.deps_.at(j)))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (spec.deps_.empty() && OB_FAIL(sql.append("''"))) {
    } else if (OB_FAIL(sql.append(")), ''), "))) {
    } else if (OB_FAIL(append_quoted(sql, ObString::make_string(ob_obj_type_str(result_column->get_data_type()))))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_quoted(sql, spec.expr_))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_quoted(sql, normalized))) {
    } else if (OB_FAIL(sql.append_fmt(", %ld FROM oceanbase.__all_table t"
                                      " JOIN oceanbase.__all_database d ON d.database_id = t.database_id",
                                      spec.version_))) {
    }
    const char *aliases[4] = {"r", "s", "q", "p"};
    for (int64_t j = 0; OB_SUCC(ret) && j < 4; ++j) {
      if (OB_FAIL(sql.append_fmt(" JOIN oceanbase.__all_column %s ON %s.table_id = t.table_id"
                                 " AND %s.column_name = ", aliases[j], aliases[j], aliases[j]))) {
      } else if (OB_FAIL(append_quoted(sql, 0 == j ? spec.column_name_ : helper_names[j - 1]))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append(" WHERE d.database_name = "))) {
    } else if (OB_FAIL(append_quoted(sql, database_name))) {
    } else if (OB_FAIL(sql.append(" AND t.table_name = "))) {
    } else if (OB_FAIL(append_quoted(sql, table_name))) {
    } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
      LOG_WARN("failed to write AI column metadata", K(ret), K(sql));
    }
  }
  return ret;
}

int ObAIColumnUtil::backfill(const ObString &database_name,
                             const ObString &table_name, const ObIArray<ObString> &columns)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (columns.empty()) {
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET ", database_name.length(), database_name.ptr(),
                                    table_name.length(), table_name.ptr()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (OB_FAIL(sql.append_fmt("%s`%s%.*s` = UUID()", i > 0 ? ", " : "", REQUEST_PREFIX,
                               columns.at(i).length(), columns.at(i).ptr()))) {
    }
  }
  if (OB_SUCC(ret) && !columns.empty()
      && OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to request AI column generation for existing rows", K(ret), K(sql));
  }
  return ret;
}

int ObAIColumnUtil::retry(const ObString &database_name,
                          const ObString &table_name, const ObAIColumnSpec &spec,
                          const ObString &where, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnRetry");
  ObString state_column;
  ObString predicate;
  ObSqlString sql;
  affected_rows = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(build_helper_name(allocator, STATE_PREFIX, spec.column_name_, state_column))) {
  } else if (!where.empty()
             && OB_FAIL(rewrite_status_predicate(allocator, where, state_column, predicate))) {
  } else if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%s%.*s` = %d,"
                                    " `%s%.*s` = UUID(), `%s%.*s` = %ld",
                                    database_name.length(), database_name.ptr(),
                                    table_name.length(), table_name.ptr(),
                                    spec.column_name_.length(), spec.column_name_.ptr(),
                                    STATE_PREFIX, spec.column_name_.length(), spec.column_name_.ptr(),
                                    AI_ROW_PENDING,
                                    REQUEST_PREFIX, spec.column_name_.length(), spec.column_name_.ptr(),
                                    SPEC_PREFIX, spec.column_name_.length(), spec.column_name_.ptr(),
                                    spec.version_))) {
  } else if (!predicate.empty()
             && OB_FAIL(sql.append_fmt(" WHERE (%.*s)", predicate.length(), predicate.ptr()))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to retry AI column", K(ret), K(sql));
  }
  return ret;
}

int ObAIColumnUtil::check_column_ready(const ObString &database_name, const ObString &table_name,
                                       const ObString &column_name, const uint64_t table_id,
                                       const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt("SELECT 1 FROM `%.*s`.`%.*s` WHERE `%s%.*s` IS NULL OR `%s%.*s` <> %d LIMIT 1",
                                    database_name.length(), database_name.ptr(),
                                    table_name.length(), table_name.ptr(),
                                    STATE_PREFIX, column_name.length(), column_name.ptr(),
                                    STATE_PREFIX, column_name.length(), column_name.ptr(),
                                    AI_ROW_READY))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to check AI column", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        ret = OB_AI_COLUMN_UNFINISHED_TASKS;
        LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASKS, table_id, column_id);
      }
    }
  }
  return ret;
}

int ObAIColumnUtil::append_quoted(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if ('\\' == c) {
      ret = sql.append("\\\\");
    } else if ('\0' == c) {
      ret = sql.append("\\0");
    } else {
      ret = sql.append(&c, 1);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
