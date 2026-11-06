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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/cmd/ob_copy_to_resolver.h"
#include "sql/resolver/cmd/ob_copy_to_stmt.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"
#include <sys/stat.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
int get_char_option(const ParseNode *value, char &result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(value) || (T_VARCHAR != value->type_ && T_CHAR != value->type_) || 1 != value->str_len_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    result = value->str_value_[0];
  }
  return ret;
}

int append_ident(ObSqlString &sql, const ParseNode *node)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < node->str_len_; ++i) {
    const char c = node->str_value_[i];
    ret = ('`' == c) ? sql.append("``") : sql.append_fmt("%c", c);
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}
} // namespace

int ObCopyToResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObCopyToStmt *stmt = NULL;
  int64_t enabled = 0;
  if (OB_ISNULL(session_info_) || OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
  } else if (T_COPY_TO != parse_tree.type_ || 4 != parse_tree.num_child_ || OB_ISNULL(parse_tree.children_[2])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (0 == enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY TO while enable_file_sql is OFF");
  } else if (OB_ISNULL(stmt = create_stmt<ObCopyToStmt>())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(resolve_options(parse_tree.children_[3], *stmt))) {
  } else if (OB_FAIL(resolve_target(ObString(static_cast<int32_t>(parse_tree.children_[2]->str_len_),
                                             parse_tree.children_[2]->str_value_), *stmt))) {
  } else if (OB_FAIL(resolve_query(parse_tree, *stmt))) {
  }
  return ret;
}

int ObCopyToResolver::resolve_query(const ParseNode &parse_tree, ObCopyToStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const ParseNode *query = parse_tree.children_[0];
  const ParseNode *table = parse_tree.children_[1];
  if (OB_NOT_NULL(query)) {
    ret = sql.append(query->str_value_, query->str_len_);
  } else if (OB_ISNULL(table) || T_RELATION_FACTOR != table->type_ || table->num_child_ < 2) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append("SELECT * FROM "))) {
  } else if (OB_NOT_NULL(table->children_[0])
             && (OB_FAIL(append_ident(sql, table->children_[0])) || OB_FAIL(sql.append(".")))) {
  } else if (OB_FAIL(append_ident(sql, table->children_[1]))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(*allocator_, sql.string(), stmt.query_sql_))) {
  }
  // the names of the result columns become the header
  if (OB_SUCC(ret)) {
    ParseResult parse_result;
    ObParser parser(*allocator_, session_info_->get_sql_mode());
    if (OB_FAIL(parser.parse(stmt.query_sql_, parse_result))) {
    } else if (OB_ISNULL(parse_result.result_tree_) || 1 != parse_result.result_tree_->num_child_
               || OB_ISNULL(parse_result.result_tree_->children_[0])
               || T_SELECT != parse_result.result_tree_->children_[0]->type_) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      HEAP_VAR(ObSelectResolver, select_resolver, params_) {
        ObSelectStmt *select_stmt = NULL;
        if (OB_FAIL(select_resolver.resolve(*parse_result.result_tree_->children_[0]))) {
          LOG_WARN("failed to resolve the query of COPY", K(ret));
        } else if (OB_ISNULL(select_stmt = select_resolver.get_select_stmt())) {
          ret = OB_ERR_UNEXPECTED;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_select_item_size(); ++i) {
          ObString name;
          if (OB_FAIL(ob_write_string(*allocator_, select_stmt->get_select_item(i).alias_name_, name))) {
          } else if (OB_FAIL(stmt.column_names_.push_back(name))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObCopyToResolver::resolve_target(const ObString &path, ObCopyToStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObString secure_file_priv;
  ObSqlString dir;
  ObSqlString full_path;
  char real_dir[PATH_MAX + 1];
  struct stat st;
  const char *slash = path.reverse_find('/');
  const ObString file_name = OB_ISNULL(slash) ? path
                             : ObString(static_cast<int32_t>(path.ptr() + path.length() - slash - 1), slash + 1);
  if (path.empty() || path.length() > PATH_MAX || file_name.empty()
      || 0 == file_name.compare(".") || 0 == file_name.compare("..")) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY TO file name");
  } else if (OB_FAIL(dir.append(OB_ISNULL(slash) ? ObString::make_string(".")
                                : ObString(static_cast<int32_t>(slash - path.ptr() + 1), path.ptr())))) {
  } else if (OB_ISNULL(::realpath(dir.ptr(), real_dir))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("directory of the COPY target does not exist", K(ret), K(path));
  } else if (OB_FAIL(full_path.append_fmt("%s%s%.*s", real_dir, '/' == real_dir[strlen(real_dir) - 1] ? "" : "/",
                                          file_name.length(), file_name.ptr()))) {
  } else if (OB_FAIL(session_info_->get_secure_file_priv(secure_file_priv))) {
  } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, full_path.string()))) {
    LOG_WARN("COPY target is outside secure_file_priv", K(ret), K(full_path));
  } else if (0 == ::lstat(full_path.ptr(), &st)) {
    // an existing file is never overwritten
    ret = OB_FILE_ALREADY_EXIST;
    LOG_WARN("COPY target already exists", K(ret), K(full_path));
  } else if (OB_FAIL(ob_write_string(*allocator_, full_path.string(), stmt.path_, true))) {
  }
  return ret;
}

int ObCopyToResolver::resolve_options(const ParseNode *options, ObCopyToStmt &stmt)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(options) && i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    const ParseNode *value = NULL;
    ObString name;
    if (OB_ISNULL(option) || T_COPY_OPTION != option->type_ || 2 != option->num_child_
        || OB_ISNULL(option->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      value = option->children_[1];
      name.assign_ptr(option->children_[0]->str_value_, static_cast<int32_t>(option->children_[0]->str_len_));
      if (0 == name.case_compare("format")) {
        ObString format;
        if (OB_ISNULL(value)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (FALSE_IT(format.assign_ptr(value->str_value_, static_cast<int32_t>(value->str_len_)))) {
        } else if (0 != format.case_compare("csv")) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY TO formats other than CSV");
        }
      } else if (0 == name.case_compare("header")) {
        if (OB_ISNULL(value)) {
          stmt.header_ = true;
        } else if (T_BOOL == value->type_ || T_INT == value->type_) {
          stmt.header_ = (0 != value->value_);
        } else {
          ret = OB_INVALID_ARGUMENT;
        }
      } else if (0 == name.case_compare("delimiter") || 0 == name.case_compare("delim")
                 || 0 == name.case_compare("sep")) {
        ret = get_char_option(value, stmt.delim_);
      } else if (0 == name.case_compare("quote")) {
        ret = get_char_option(value, stmt.quote_);
      } else if (0 == name.case_compare("escape")) {
        ret = get_char_option(value, stmt.escape_);
      } else if (0 == name.case_compare("nullstr") || 0 == name.case_compare("null")) {
        if (OB_ISNULL(value) || (T_VARCHAR != value->type_ && T_CHAR != value->type_)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(ob_write_string(*allocator_, ObString(static_cast<int32_t>(value->str_len_),
                                                                 value->str_value_), stmt.null_str_))) {
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unknown COPY option", K(ret), K(name));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this COPY TO option");
      }
      if (OB_INVALID_ARGUMENT == ret) {
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY TO");
      }
    }
  }
  if (OB_SUCC(ret) && (stmt.delim_ == stmt.quote_ || '\n' == stmt.delim_ || '\r' == stmt.delim_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY TO");
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
