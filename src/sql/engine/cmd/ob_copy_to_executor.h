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

#ifndef OCEANBASE_SQL_ENGINE_CMD_OB_COPY_TO_EXECUTOR_H_
#define OCEANBASE_SQL_ENGINE_CMD_OB_COPY_TO_EXECUTOR_H_

#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace sql
{
class ObExecContext;
class ObCopyToStmt;

// Writes the rows of a query to a new CSV file. The rows go to a temporary file in the target
// directory that becomes the target only when the query succeeded; the target is never replaced.
class ObCopyToExecutor
{
public:
  ObCopyToExecutor() {}
  ~ObCopyToExecutor() {}
  int execute(ObExecContext &ctx, ObCopyToStmt &stmt);

private:
  int append_field(const ObCopyToStmt &stmt, const common::ObString &value, common::ObSqlString &line);
  int write_all(const int fd, const common::ObSqlString &data);
  DISALLOW_COPY_AND_ASSIGN(ObCopyToExecutor);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_CMD_OB_COPY_TO_EXECUTOR_H_
