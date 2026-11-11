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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"
#include "lib/hash/ob_hashset.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_project_relevance()
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    // a count of the matching documents needs only the union of the posting lists
    union_only_ = !param.base_param_->need_project_relevance() && !param.base_param_->need_filter()
                  && !function_lookup_mode_ && param.relevance_collector_->accepts_single_match();
    count_only_ = param.base_param_->need_count_only();
    count_computed_ = false;
    count_only_total_ = 0;
    count_only_returned_ = 0;
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!count_only_)) {
    ret = ObSRDaaTIterImpl::get_next_rows(capacity, count);
  } else if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (!count_computed_ && OB_FAIL(compute_count_only_total())) {
  } else {
    count_computed_ = true;
    const int64_t batch_size = MIN(capacity, iter_param_->max_batch_size_);
    const int64_t remaining = count_only_total_ - count_only_returned_;
    count = MIN(remaining, batch_size);
    count_only_returned_ += count;
    output_row_cnt_ += count;
    if (0 == count) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  return union_only_ ? do_one_union_round(count) : ObSRDaaTIterImpl::do_one_merge_round(count);
}

int ObTextDaaTIter::do_one_union_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  // advance the iterators whose current id was returned by the previous round
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDaaTDimIter *dim_iter = dim_iters_->at(iter_idx);
    if (OB_FAIL(dim_iter->get_next_row())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        iter_domain_ids_[iter_idx] = nullptr;
      }
    } else if (OB_FAIL(dim_iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
    }
  }
  // the smallest current id is the next result, every iterator positioned on it moves on
  const ObDatum *min_id = nullptr;
  next_round_cnt_ = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    const ObDatum *id = iter_domain_ids_[i];
    int cmp = 0;
    if (OB_ISNULL(id)) {
    } else if (OB_ISNULL(min_id)) {
      min_id = id;
      next_round_iter_idxes_[next_round_cnt_++] = i;
    } else if (OB_FAIL(merge_cmp_.cmp_datum(*id, *min_id, cmp))) {
    } else if (cmp < 0) {
      min_id = id;
      next_round_cnt_ = 0;
      next_round_iter_idxes_[next_round_cnt_++] = i;
    } else if (0 == cmp) {
      next_round_iter_idxes_[next_round_cnt_++] = i;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(min_id)) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(cache_result(count, *min_id, 0.0))) {
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  union_only_ = false;
  count_only_ = false;
  count_computed_ = false;
  count_only_total_ = 0;
  count_only_returned_ = 0;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  count_computed_ = false;
  count_only_total_ = 0;
  count_only_returned_ = 0;
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}


namespace {
const int64_t COUNT_ONLY_BITMAP_INIT_BITS = 1LL << 18; // 262144 bits = 32KB
const int64_t COUNT_ONLY_BITMAP_MAX_BITS = 1LL << 26;  // 8MB

class CountOnlyBitmap
{
public:
  CountOnlyBitmap()
    : allocator_(nullptr), bits_(nullptr), bits_count_(0), overflow_(), overflow_inited_(false) {}
  ~CountOnlyBitmap() {}
  int init(ObIAllocator *allocator)
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(allocator)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      allocator_ = allocator;
      if (OB_FAIL(ensure_bits(COUNT_ONLY_BITMAP_INIT_BITS))) {
      }
    }
    return ret;
  }
  int add(const int64_t id)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(id < 0)) {
      ret = add_overflow(id);
    } else if (id < bits_count_) {
      bits_[id >> 6] |= (1ULL << (id & 63));
    } else {
      ret = grow_for(id);
      if (OB_SUCC(ret)) {
        bits_[id >> 6] |= (1ULL << (id & 63));
      } else if (OB_SIZE_OVERFLOW == ret) {
        ret = OB_SUCCESS;
        if (OB_FAIL(add_overflow(id))) {
        }
      }
    }
    return ret;
  }
  uint64_t *bits() const { return bits_; }
  int64_t bits_count() const { return bits_count_; }
  int64_t count() const
  {
    int64_t total = 0;
    for (int64_t i = 0; i < bits_count_ / 64; ++i) {
      total += __builtin_popcountll(bits_[i]);
    }
    if (overflow_inited_) {
      total += static_cast<int64_t>(overflow_.size());
    }
    return total;
  }
private:
  int ensure_bits(const int64_t bits)
  {
    int ret = OB_SUCCESS;
    const int64_t words = (bits + 63) / 64;
    uint64_t *new_bits = static_cast<uint64_t *>(allocator_->alloc(words * sizeof(uint64_t)));
    if (OB_ISNULL(new_bits)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMSET(new_bits, 0, words * sizeof(uint64_t));
      if (OB_NOT_NULL(bits_)) {
        const int64_t old_words = bits_count_ / 64;
        MEMCPY(new_bits, bits_, old_words * sizeof(uint64_t));
      }
      bits_ = new_bits;
      bits_count_ = words * 64;
    }
    return ret;
  }
  int grow_for(const int64_t id)
  {
    int ret = OB_SUCCESS;
    const int64_t need_bits = id + 1;
    const int64_t new_bits = OB_MAX(bits_count_ * 2, need_bits);
    if (new_bits > COUNT_ONLY_BITMAP_MAX_BITS) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      ret = ensure_bits(new_bits);
    }
    return ret;
  }
  int add_overflow(const int64_t id)
  {
    int ret = OB_SUCCESS;
    if (!overflow_inited_) {
      if (OB_FAIL(overflow_.create(1024))) {
      } else {
        overflow_inited_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      ret = overflow_.set_refactored(id);
      if (OB_HASH_EXIST == ret) {
        ret = OB_SUCCESS;
      }
    }
    return ret;
  }
private:
  ObIAllocator *allocator_;
  uint64_t *bits_;
  int64_t bits_count_;
  common::hash::ObHashSet<int64_t> overflow_;
  bool overflow_inited_;
};

} // namespace

int ObTextDaaTIter::compute_count_only_total()
{
  int ret = OB_SUCCESS;
  CountOnlyBitmap bitmap;
  if (OB_FAIL(bitmap.init(iter_allocator_))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      ObTextRetrievalDaaTTokenIter *token_iter =
          static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
      bool done = false;
      while (OB_SUCC(ret) && !done) {
        int64_t batch_count = 0;
        common::ObDatumVector ids;
        ret = token_iter->get_next_batch_ids(batch_count, ids);
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          done = true;
        } else if (OB_FAIL(ret)) {
        } else if (OB_ISNULL(ids.datums_) || batch_count <= 0) {
          done = true;
        } else {
          int64_t bitmap_bits = bitmap.bits_count();
          uint64_t *bitmap_bits_ptr = bitmap.bits();
          for (int64_t j = 0; OB_SUCC(ret) && j < batch_count; ++j) {
            const ObDatum *id = ids.datums_ + j;
            int64_t doc_seq_id = 0;
            if (OB_ISNULL(id) || id->len_ < 16) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("invalid doc id for count only", K(ret), KP(id));
            } else if (FALSE_IT(MEMCPY(&doc_seq_id, id->ptr_ + 8, sizeof(doc_seq_id)))) {
            } else if (OB_LIKELY(doc_seq_id >= 0 && doc_seq_id < bitmap_bits)) {
              bitmap_bits_ptr[doc_seq_id >> 6] |= (1ULL << (doc_seq_id & 63));
            } else if (OB_FAIL(bitmap.add(doc_seq_id))) {
            } else {
              bitmap_bits = bitmap.bits_count();
              bitmap_bits_ptr = bitmap.bits();
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      count_only_total_ = bitmap.count();
    }
  }
  return ret;
}


} // namespace storage
} // namespace oceanbase
