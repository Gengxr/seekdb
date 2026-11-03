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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::get_dimension(const ObDatum *arg_dim, int64_t &dim)
{
  int ret = OB_SUCCESS;
  dim = 0;
  if (OB_ISNULL(arg_dim)) {
    // no explicit dimension: the model default applies
  } else if (arg_dim->is_null() || arg_dim->get_int() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  } else {
    dim = arg_dim->get_int();
  }
  return ret;
}

int ObExprAIEmbed::embed_contents(ObIAllocator &allocator,
                                  const ObString &model_id,
                                  const int64_t dim,
                                  ObArray<ObString> &contents,
                                  ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint_info;
  ObJsonObject *config = nullptr;
  ObJsonInt *dim_json = nullptr;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  if (model_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
  } else if (dim > 0 && (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))
                         || OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))
                         || OB_FAIL(config->add("dimensions", dim_json)))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, allocator, endpoint_info))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint_info);
    // Rows are sent in bounded requests; each request maps its results back by position.
    for (int64_t begin = 0; OB_SUCC(ret) && begin < contents.count(); begin += MAX_BATCH_INPUTS) {
      const int64_t end = MIN(begin + MAX_BATCH_INPUTS, contents.count());
      ObArray<ObString> chunk;
      ObArray<ObString> chunk_results;
      for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
        if (contents.at(i).empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        } else if (OB_FAIL(chunk.push_back(contents.at(i)))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(model.call_dense_embedding_vector_v2(chunk, config, chunk_results))) {
      } else if (chunk_results.count() != chunk.count()) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(append(results, chunk_results))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  int64_t dim = 0;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (OB_FAIL(get_dimension(arg_dim, dim))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // NULL input propagates as a NULL embedding without calling the model.
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObArray<ObString> contents;
    ObArray<ObString> results;
    if (OB_FAIL(contents.push_back(arg_content->get_string()))) {
    } else if (OB_FAIL(embed_contents(temp_allocator, arg_model_id->get_string(), dim, contents, results))) {
    } else if (results.count() != 1) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObDatumVector res_datum = expr.locate_expr_datumvector(ctx);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObDatumVector model_datum = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector content_datum = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObArray<int64_t> pending;
    ObArray<int64_t> dims;
    for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; ++j) {
      int64_t dim = 0;
      if (skip.at(j) || eval_flags.at(j)) {
      } else if (OB_FAIL(get_dimension(expr.arg_cnt_ == 3
                                           ? expr.args_[DIM_IDX]->locate_expr_datumvector(ctx).at(j)
                                           : nullptr,
                                       dim))) {
      } else if (model_datum.at(j)->is_null() || content_datum.at(j)->is_null()) {
        res_datum.at(j)->set_null();
        eval_flags.set(j);
      } else if (OB_FAIL(pending.push_back(j)) || OB_FAIL(dims.push_back(dim))) {
      }
    }
    // Requests are grouped by (model, dimension) so each one targets a single endpoint.
    ObArray<bool> done;
    if (OB_SUCC(ret) && OB_FAIL(done.prepare_allocate(pending.count()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < pending.count(); i++) {
      done.at(i) = false;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < pending.count(); i++) {
      if (done.at(i)) {
        continue;
      }
      const ObString model_id = model_datum.at(pending.at(i))->get_string();
      const int64_t dim = dims.at(i);
      ObArray<int64_t> rows;
      ObArray<ObString> contents;
      ObArray<ObString> results;
      for (int64_t k = i; OB_SUCC(ret) && k < pending.count(); k++) {
        if (!done.at(k) && dims.at(k) == dim
            && 0 == model_datum.at(pending.at(k))->get_string().compare(model_id)) {
          done.at(k) = true;
          if (OB_FAIL(rows.push_back(pending.at(k)))) {
          } else if (OB_FAIL(contents.push_back(content_datum.at(pending.at(k))->get_string()))) {
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(embed_contents(temp_allocator, model_id, dim, contents, results))) {
      } else if (results.count() != rows.count()) {
        ret = OB_ERR_UNEXPECTED;
      }
      for (int64_t k = 0; OB_SUCC(ret) && k < rows.count(); k++) {
        const int64_t row = rows.at(k);
        const ObString &value = results.at(k);
        char *buf = expr.get_str_res_mem(ctx, value.length(), row);
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, value.ptr(), value.length());
          res_datum.at(row)->set_string(buf, value.length());
          eval_flags.set(row);
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   ObObj const_data;
  //   bool got_data = false;
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
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
