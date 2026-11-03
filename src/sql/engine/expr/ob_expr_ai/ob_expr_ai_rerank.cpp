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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include <cmath>
#include "lib/utility/ob_sort.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint_info;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    // A NULL document key is the same as omitting it: documents are plain strings.
    const bool has_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null();
    ObString doc_key = has_doc_key ? arg_doc_key->get_string() : ObString();
    ObJsonArray *document_array = nullptr;
    ObJsonArray *text_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    ObArray<RerankScore> scores;
    ObJsonArray *result_array = nullptr;
    if (model_id.empty() || query.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query is empty");
    } else if (has_doc_key && doc_key.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key is empty");
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                                                      j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (FALSE_IT(document_array = static_cast<ObJsonArray *>(j_base))) {
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else if (OB_FAIL(get_document_texts(temp_allocator, document_array, has_doc_key, doc_key, text_array))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(info) || OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, temp_allocator, endpoint_info))) {
    } else if (OB_FAIL(rerank_in_batches(temp_allocator, *info, endpoint_info, query, text_array, scores))) {
    } else if (OB_FAIL(build_result(temp_allocator, document_array, has_doc_key, scores, result_array))) {
    } else {
      ObString raw_str;
      if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_document_texts(ObIAllocator &allocator, ObJsonArray *document_array,
                                       const bool has_doc_key, const ObString &doc_key,
                                       ObJsonArray *&text_array)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, text_array))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
    ObJsonNode *doc_node = document_array->get_value(i);
    ObJsonNode *text_node = doc_node;
    if (OB_ISNULL(doc_node)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
    } else if (has_doc_key) {
      if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
      } else if (OB_ISNULL(text_node = static_cast<ObJsonObject *>(doc_node)->get_value(doc_key))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
      }
    }
    if (OB_FAIL(ret)) {
    } else if (text_node->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_ARGUMENT;
      if (has_doc_key) {
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
      } else {
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
      }
    } else if (OB_FAIL(text_array->append(text_node))) {
    }
  }
  return ret;
}

int ObExprAIRerank::rerank_in_batches(ObIAllocator &allocator,
                                      const ObAIFuncExprInfo &info,
                                      const share::ObAiModelEndpointInfo &endpoint_info,
                                      ObString &query,
                                      ObJsonArray *text_array,
                                      ObArray<RerankScore> &scores)
{
  INIT_SUCC(ret);
  ObAIFuncModel model(allocator, info, endpoint_info);
  const int64_t total = text_array->element_count();
  for (int64_t begin = 0; OB_SUCC(ret) && begin < total; begin += MAX_BATCH_DOCUMENTS) {
    const int64_t end = MIN(begin + MAX_BATCH_DOCUMENTS, total);
    ObJsonArray *batch = nullptr;
    ObJsonArray *batch_result = nullptr;
    if (OB_FAIL(construct_batch_document_array(allocator, text_array, begin, end, batch))) {
    } else if (OB_FAIL(model.call_rerank(query, batch, batch_result))) {
    } else if (OB_FAIL(collect_batch_scores(batch_result, begin, end - begin, scores))) {
    }
  }
  if (OB_SUCC(ret)) {
    // Highest score first; equal scores keep the original document order.
    lib::ob_sort(scores.begin(), scores.end(), [](const RerankScore &l, const RerankScore &r) {
      return l.score_ > r.score_ || (l.score_ == r.score_ && l.index_ < r.index_);
    });
  }
  return ret;
}

int ObExprAIRerank::collect_batch_scores(ObJsonArray *batch_result, const int64_t base,
                                         const int64_t batch_count, ObArray<RerankScore> &scores)
{
  INIT_SUCC(ret);
  ObArray<bool> seen;
  if (OB_ISNULL(batch_result) || batch_result->json_type() != ObJsonNodeType::J_ARRAY
      || batch_result->element_count() != batch_count) {
    ret = OB_INVALID_DATA;
    LOG_WARN("rerank response must score every document of the batch", K(ret), K(batch_count));
  } else if (OB_FAIL(seen.prepare_allocate(batch_count))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < batch_count; i++) {
    seen.at(i) = false;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < batch_count; i++) {
    ObJsonNode *item = batch_result->get_value(i);
    ObJsonNode *index_node = nullptr;
    ObJsonNode *score_node = nullptr;
    int64_t index = -1;
    double score = 0;
    if (OB_ISNULL(item) || item->json_type() != ObJsonNodeType::J_OBJECT
        || OB_ISNULL(index_node = static_cast<ObJsonObject *>(item)->get_value(INDEX_KEY))
        || OB_ISNULL(score_node = static_cast<ObJsonObject *>(item)->get_value(SCORE_KEY))) {
      ret = OB_INVALID_DATA;
    } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
      index = static_cast<ObJsonInt *>(index_node)->get_int();
    } else if (index_node->json_type() == ObJsonNodeType::J_UINT) {
      index = static_cast<int64_t>(static_cast<ObJsonUint *>(index_node)->get_uint());
    } else {
      ret = OB_INVALID_DATA;
    }
    if (OB_FAIL(ret)) {
    } else if (index < 0 || index >= batch_count || seen.at(index)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("rerank response index is out of range or duplicated", K(ret), K(index), K(batch_count));
    } else if (!is_numeric_json(score_node->json_type()) || OB_FAIL(score_node->to_double(score))
               || std::isnan(score) || std::isinf(score)) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(scores.push_back(RerankScore(base + index, score)))) {
    } else {
      seen.at(index) = true;
    }
  }
  return ret;
}

bool ObExprAIRerank::is_numeric_json(const ObJsonNodeType type)
{
  return ObJsonNodeType::J_INT == type || ObJsonNodeType::J_UINT == type
      || ObJsonNodeType::J_DOUBLE == type || ObJsonNodeType::J_DECIMAL == type
      || ObJsonNodeType::J_OFLOAT == type || ObJsonNodeType::J_ODOUBLE == type;
}

int ObExprAIRerank::build_result(ObIAllocator &allocator, ObJsonArray *document_array,
                                 const bool has_doc_key, const ObArray<RerankScore> &scores,
                                 ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < scores.count(); i++) {
    const RerankScore &item = scores.at(i);
    ObJsonDouble *score_node = OB_NEWx(ObJsonDouble, &allocator, item.score_);
    ObJsonObject *obj = nullptr;
    if (OB_ISNULL(score_node)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (has_doc_key) {
      // Object documents are returned whole, in rank order, with the model score attached.
      ObJsonNode *origin = document_array->get_value(item.index_);
      if (OB_ISNULL(origin) || OB_ISNULL(obj = static_cast<ObJsonObject *>(origin->clone(&allocator, true)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(obj->add(MODEL_SCORE_KEY, score_node))) {
      }
    } else {
      ObJsonInt *index_node = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, item.index_, index_node))) {
      } else if (OB_FAIL(obj->add(INDEX_KEY, index_node))) {
      } else if (OB_FAIL(obj->add(SCORE_KEY, score_node))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(result_array->append(obj))) {
    }
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; OB_SUCC(ret) && i < end_idx; i++) {
      ObJsonNode *node = document_array->get_value(i);
      ObJsonNode *copy = OB_ISNULL(node) ? nullptr : node->clone(&allocator, true);
      if (OB_ISNULL(copy)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(result_array->append(copy))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
