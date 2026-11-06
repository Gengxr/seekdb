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
#include "sql/resolver/cmd/ob_describe_query_resolver.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
const int64_t EXPLAIN_STMT_CHILD = 4;

int append_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\'' == c || '\\' == c) {
      ret = sql.append_fmt("%c%c", c, c);
    } else {
      ret = sql.append_fmt("%c", c);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

const char *sql_type_name(const ObRawExprResType &type)
{
  const char *name = "VARCHAR";
  const ObObjType obj_type = type.get_type();
  if (ObNullType == obj_type) {
    name = "NULL";
  } else if (ObTinyIntType == obj_type && 1 == type.get_precision()) {
    name = "BOOLEAN";
  } else if (ob_is_integer_type(obj_type)) {
    name = "BIGINT";
  } else if (ob_is_float_tc(obj_type) || ob_is_double_tc(obj_type)) {
    name = "DOUBLE";
  } else if (ob_is_number_tc(obj_type) || ob_is_decimal_int_tc(obj_type)) {
    name = "DECIMAL";
  } else if (ObDateType == obj_type || ObMySQLDateType == obj_type) {
    name = "DATE";
  } else if (ob_is_datetime_tc(obj_type) || ObMySQLDateTimeType == obj_type) {
    name = "DATETIME";
  } else if (ob_is_time_tc(obj_type)) {
    name = "TIME";
  } else if (ob_is_json(obj_type)) {
    name = "JSON";
  }
  return name;
}
} // namespace

bool ObDescribeQueryResolver::contains_file_table(const ParseNode *node)
{
  bool found = false;
  if (OB_NOT_NULL(node)) {
    found = (T_FILE_TABLE == node->type_);
    for (int64_t i = 0; !found && i < node->num_child_; ++i) {
      found = contains_file_table(node->children_[i]);
    }
  }
  return found;
}

bool ObDescribeQueryResolver::is_describe_file_query(const ParseNode &parse_tree)
{
  return T_EXPLAIN == parse_tree.type_ && 1 == parse_tree.reserved_
         && parse_tree.num_child_ > EXPLAIN_STMT_CHILD
         && OB_NOT_NULL(parse_tree.children_[EXPLAIN_STMT_CHILD])
         && T_SELECT == parse_tree.children_[EXPLAIN_STMT_CHILD]->type_
         && contains_file_table(parse_tree.children_[EXPLAIN_STMT_CHILD]);
}

int ObDescribeQueryResolver::describe_column(const ObSelectStmt &stmt, const SelectItem &item,
                                             ObString &type_name, bool &nullable)
{
  int ret = OB_SUCCESS;
  const ObRawExpr *expr = item.expr_;
  bool found = false;
  nullable = true;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (expr->is_column_ref_expr()) {
    // a column of a file keeps the inferred type and nullability
    const ObColumnRefRawExpr *col = static_cast<const ObColumnRefRawExpr *>(expr);
    const TableItem *table = stmt.get_table_item_by_id(col->get_table_id());
    if (OB_NOT_NULL(table) && table->is_json_table() && OB_NOT_NULL(table->json_table_def_)
        && MulModeTableType::OB_FILE_TABLE_TYPE == table->json_table_def_->table_type_) {
      const ObJsonTableDef &def = *table->json_table_def_;
      for (int64_t i = 0; !found && i < def.all_cols_.count(); ++i) {
        const ObJtColBaseInfo *info = def.all_cols_.at(i);
        if (OB_NOT_NULL(info) && COL_TYPE_FILE == info->col_type_
            && static_cast<uint64_t>(info->id_) == col->get_column_id()) {
          found = true;
          type_name = ObString::make_string(
              ObFileSqlUtil::type_name(static_cast<ObFileColType>(info->on_mismatch_type_)));
          nullable = (1 == info->on_empty_);
        }
      }
    }
  }
  if (OB_SUCC(ret) && !found) {
    type_name = ObString::make_string(sql_type_name(expr->get_result_type()));
  }
  return ret;
}

int ObDescribeQueryResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const ParseNode *query_node = NULL;
  ObSelectStmt *query_stmt = NULL;
  if (!is_describe_file_query(parse_tree) || OB_ISNULL(session_info_) || OB_ISNULL(params_.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(query_node = parse_tree.children_[EXPLAIN_STMT_CHILD])) {
  } else {
    HEAP_VAR(ObSelectResolver, query_resolver, params_) {
      if (OB_FAIL(query_resolver.resolve(*query_node))) {
        LOG_WARN("failed to resolve the described query", K(ret));
      } else if (OB_ISNULL(query_stmt = query_resolver.get_select_stmt())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(sql.append("SELECT column_name, column_type, `null`, `key`, `default`, extra FROM ("))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < query_stmt->get_select_item_size(); ++i) {
        const SelectItem &item = query_stmt->get_select_item(i);
        ObString type_name;
        bool nullable = true;
        if (OB_FAIL(describe_column(*query_stmt, item, type_name, nullable))) {
        } else if (OB_FAIL(sql.append_fmt("%sSELECT %ld AS pos, ", 0 == i ? "" : " UNION ALL ", i))) {
        } else if (OB_FAIL(append_literal(sql, item.alias_name_))) {
        } else if (OB_FAIL(sql.append(" AS column_name, "))) {
        } else if (OB_FAIL(append_literal(sql, type_name))) {
        } else if (OB_FAIL(sql.append_fmt(" AS column_type, '%s' AS `null`, NULL AS `key`,"
                                          " NULL AS `default`, NULL AS extra", nullable ? "YES" : "NO"))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append(") describe_result ORDER BY pos"))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    ParseResult parse_result;
    ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
    ObString sql_text;
    if (OB_FAIL(ob_write_string(*params_.allocator_, sql.string(), sql_text))) {
    } else if (OB_FAIL(parser.parse(sql_text, parse_result))) {
      LOG_WARN("failed to parse the DESCRIBE result query", K(ret), K(sql_text));
    } else if (OB_ISNULL(parse_result.result_tree_) || 1 != parse_result.result_tree_->num_child_
               || OB_ISNULL(parse_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObSelectResolver::resolve(*parse_result.result_tree_->children_[0]))) {
      LOG_WARN("failed to resolve the DESCRIBE result query", K(ret), K(sql_text));
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
