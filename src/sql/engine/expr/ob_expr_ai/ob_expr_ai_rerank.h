/*
 * Copyright (c) 2025 OceanBase.
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

#ifndef OCEANBASE_SQL_OB_EXPR_AI_RERANK_H_
#define OCEANBASE_SQL_OB_EXPR_AI_RERANK_H_
 
#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/ob_exec_context.h"
#include "ob_ai_func_client.h"
#include "ob_ai_func.h"
#include "ob_ai_func_utils.h"
 
namespace oceanbase 
{
namespace sql 
{
class ObExprAIRerank : public ObFuncExprOperator 
{
public:
  explicit ObExprAIRerank(common::ObIAllocator &alloc);
  virtual ~ObExprAIRerank();
  virtual int calc_result_typeN(ObExprResType &type, ObExprResType *types_array,
                                int64_t param_num,
                                common::ObExprTypeCtx &type_ctx) const override;
  static int eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);

  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  virtual bool need_rt_ctx() const override { return true; }
 
private:
  struct RerankScore
  {
    RerankScore() : index_(0), score_(0) {}
    RerankScore(const int64_t index, const double score) : index_(index), score_(score) {}
    TO_STRING_KV(K_(index), K_(score));
    int64_t index_;   // position in the caller's document array
    double score_;
  };
  static int get_document_texts(common::ObIAllocator &allocator, common::ObJsonArray *document_array,
                                const bool has_doc_key, const common::ObString &doc_key,
                                common::ObJsonArray *&text_array);
  static int rerank_in_batches(common::ObIAllocator &allocator,
                               const common::ObAIFuncExprInfo &info,
                               const share::ObAiModelEndpointInfo &endpoint_info,
                               common::ObString &query,
                               common::ObJsonArray *text_array,
                               common::ObArray<RerankScore> &scores);
  static int collect_batch_scores(common::ObJsonArray *batch_result, const int64_t base,
                                  const int64_t batch_count, common::ObArray<RerankScore> &scores);
  static bool is_numeric_json(const common::ObJsonNodeType type);
  static int build_result(common::ObIAllocator &allocator, common::ObJsonArray *document_array,
                          const bool has_doc_key, const common::ObArray<RerankScore> &scores,
                          common::ObJsonArray *&result_array);
  static int construct_config_json(common::ObIAllocator &allocator, int64_t top_k, int64_t return_doc, common::ObJsonObject *&config_json);
  static int construct_batch_document_array(common::ObIAllocator &allocator, common::ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, common::ObJsonArray *&batch_document_array);
  // Upper bound of documents scored by one rerank request.
  static constexpr int64_t MAX_BATCH_DOCUMENTS = 20;
  static constexpr int MODEL_IDX = 0;
  static constexpr int QUERY_IDX = 1;
  static constexpr int DOCUMENTS_IDX = 2;
  static constexpr int DOC_KEY_IDX = 3;
  static constexpr char MODEL_SCORE_KEY[20] = "_model_score";
  static constexpr char INDEX_KEY[20] = "index";
  static constexpr char SCORE_KEY[20] = "relevance_score";
  DISALLOW_COPY_AND_ASSIGN(ObExprAIRerank);
};

} // namespace sql
} // namespace oceanbase
#endif // OCEANBASE_SQL_OB_EXPR_AI_RERANK_H_
  
