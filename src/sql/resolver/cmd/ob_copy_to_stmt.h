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

#ifndef OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_STMT_H_
#define OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_STMT_H_

#include "sql/resolver/cmd/ob_cmd_stmt.h"

namespace oceanbase
{
namespace sql
{

// COPY (query) TO 'file' (FORMAT csv, HEADER, DELIMITER ',', QUOTE '"', ESCAPE '"', NULLSTR '')
// COPY table TO 'file' (...)
class ObCopyToStmt : public ObCMDStmt
{
public:
  ObCopyToStmt()
    : ObCMDStmt(stmt::T_FILE_COPY), query_sql_(), path_(), header_(true), delim_(','),
      quote_('"'), escape_('"'), null_str_(), column_names_() {}
  virtual ~ObCopyToStmt() {}
  common::ObString query_sql_;      // the rows to write
  common::ObString path_;           // target file, inside secure_file_priv
  bool header_;
  char delim_;
  char quote_;
  char escape_;
  common::ObString null_str_;
  common::ObSEArray<common::ObString, 16> column_names_;
  TO_STRING_KV(K_(query_sql), K_(path), K_(header), K_(delim), K_(quote), K_(escape), K_(null_str));
private:
  DISALLOW_COPY_AND_ASSIGN(ObCopyToStmt);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_CMD_OB_COPY_TO_STMT_H_
