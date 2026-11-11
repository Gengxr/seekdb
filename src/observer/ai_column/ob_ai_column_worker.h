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

#ifndef OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_

#include "lib/thread/thread_pool.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
}
}
namespace observer
{

// Generates AI column results in the background.
//
// The rows of __all_ai_column_task created by one row change share (table_id, rowkey_predicate,
// request_id) and form a request group. A worker claims the group, evaluates the AI expression of
// every claimed column against the row version that requested it, and records the outcome on the
// task rows. When every column of the group has a final outcome, all results and row states are
// published in one transaction; each column is only written while its request id still matches,
// so a result of an outdated or deleted row version never overwrites newer data.
class ObAIColumnWorker : public lib::ThreadPool
{
public:
  static ObAIColumnWorker &get_instance();
  int start() override;
  void stop() override;
  void wait() override;
  void run1() override;

private:
  struct GroupKey
  {
    uint64_t table_id_;
    common::ObString rowkey_predicate_;
    common::ObString request_id_;
  };
  struct ColumnTask
  {
    uint64_t column_id_;
    int64_t retry_count_;
    int64_t max_retries_;
    bool succeeded_;
    common::ObString value_;
    common::ObString error_;
  };
  static const int64_t THREAD_COUNT = 4;
  static const int64_t IDLE_INTERVAL_US = 50L * 1000;
  static const int64_t MAX_IDLE_INTERVAL_US = 1000L * 1000;
  static const int64_t MAINTENANCE_INTERVAL_US = 10L * 1000 * 1000;
  static const int64_t CLAIM_TIMEOUT_S = 600;
  static const int64_t CANDIDATE_LIMIT = 16;
  static const int64_t RETRY_BASE_DELAY_US = 1000L * 1000;
  static const int64_t RETRY_MAX_DELAY_US = 10L * 1000 * 1000;

  ObAIColumnWorker();
  int process_one(bool &found);
  int claim_group(const GroupKey &key, const common::ObString &token, bool &claimed);
  int process_group(common::ObIAllocator &allocator, const GroupKey &key,
                    const common::ObString &token);
  int evaluate_column(common::ObIAllocator &allocator, const GroupKey &key,
                      const common::ObString &database_name,
                      const share::schema::ObTableSchema &table_schema,
                      ColumnTask &task, bool &row_changed);
  int check_value(common::ObIAllocator &allocator,
                  const share::schema::ObTableSchema &table_schema,
                  const uint64_t column_id, const common::ObString &value,
                  common::ObString &error);
  int record_outcome(const GroupKey &key, const common::ObString &token,
                     const ColumnTask &task, bool &still_owned);
  int try_publish(const GroupKey &key, const common::ObString &database_name,
                  const share::schema::ObTableSchema &table_schema,
                  const bool retry_on_error = true);
  int drop_claimed_tasks(const GroupKey &key, const common::ObString &token);
  int recover_claims(const bool all);
  int clean_orphans();
  int append_group_condition(const GroupKey &key, common::ObSqlString &sql);

  int64_t claim_seq_;
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
