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

#ifndef OCEANBASE_SQL_RESOLVER_CMD_OB_DESCRIBE_QUERY_RESOLVER_H_
#define OCEANBASE_SQL_RESOLVER_CMD_OB_DESCRIBE_QUERY_RESOLVER_H_

#include "sql/resolver/dml/ob_select_resolver.h"

namespace oceanbase
{
namespace sql
{

// DESCRIBE SELECT ... FROM read_csv(...): one row per result column with the columns
// column_name, column_type, null, key, default and extra. The query is resolved to find its
// columns and the answer is resolved as a SELECT over constants; the query is not executed.
class ObDescribeQueryResolver : public ObSelectResolver
{
public:
  explicit ObDescribeQueryResolver(ObResolverParams &params) : ObSelectResolver(params) {}
  virtual ~ObDescribeQueryResolver() {}
  virtual int resolve(const ParseNode &parse_tree) override;
  // Whether a DESCRIBE statement describes a query that reads a file.
  static bool is_describe_file_query(const ParseNode &parse_tree);

private:
  static bool contains_file_table(const ParseNode *node);
  int describe_column(const ObSelectStmt &stmt, const SelectItem &item,
                      common::ObString &type_name, bool &nullable);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_CMD_OB_DESCRIBE_QUERY_RESOLVER_H_
