// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#include "GroupReduce.h"
#include <unordered_map>

#include "common/Consts.h"
#include "fmt/format.h"
#include "log/Log.h"
#include "segcore/SegmentInterface.h"
#include "segcore/ReduceUtils.h"

namespace milvus::segcore {

void
GroupReduceHelper::FillOtherData(
    int result_count,
    int64_t nq_begin,
    int64_t nq_end,
    std::unique_ptr<milvus::proto::schema::SearchResultData>& search_res_data) {
    std::vector<GroupByValueType> group_by_values;
    group_by_values.resize(result_count);
    for (auto qi = nq_begin; qi < nq_end; qi++) {
        for (auto search_result : search_results_) {
            AssertInfo(search_result != nullptr,
                       "null search result when reorganize");
            if (search_result->result_offsets_.size() == 0) {
                continue;
            }

            auto topk_start = search_result->topk_per_nq_prefix_sum_[qi];
            auto topk_end = search_result->topk_per_nq_prefix_sum_[qi + 1];
            for (auto ki = topk_start; ki < topk_end; ki++) {
                auto loc = search_result->result_offsets_[ki];
                group_by_values[loc] =
                    search_result->group_by_values_.value()[ki];
            }
        }
    }
    AssembleGroupByValues(search_res_data, group_by_values, plan_);
}

void
GroupReduceHelper::RefreshSingleSearchResult(SearchResult* search_result,
                                             int seg_res_idx,
                                             std::vector<int64_t>& real_topks) {
    AssertInfo(search_result->group_by_values_.has_value(),
               "no group by values for search result, group reducer should not "
               "be called, wrong code");
    AssertInfo(search_result->primary_keys_.size() ==
                   search_result->group_by_values_.value().size(),
               "Wrong size for group_by_values size before refresh:{}, "
               "not equal to "
               "primary_keys_.size:{}",
               search_result->group_by_values_.value().size(),
               search_result->primary_keys_.size());
    CheckElementIndicesSize(search_result,
                            search_result->primary_keys_.size(),
                            "group refresh search result");

    uint32_t size = 0;
    for (int j = 0; j < total_nq_; j++) {
        size += final_search_records_[seg_res_idx][j].size();
    }
    std::vector<milvus::PkType> primary_keys(size);
    std::vector<float> distances(size);
    std::vector<int64_t> seg_offsets(size);
    std::vector<GroupByValueType> group_by_values(size);
    std::vector<int32_t> element_indices;
    if (search_result->element_level_) {
        element_indices.resize(size);
    }

    uint32_t index = 0;
    for (int j = 0; j < total_nq_; j++) {
        for (auto offset : final_search_records_[seg_res_idx][j]) {
            primary_keys[index] =
                std::move(search_result->primary_keys_[offset]);
            distances[index] = search_result->distances_[offset];
            seg_offsets[index] = search_result->seg_offsets_[offset];
            group_by_values[index] =
                std::move(search_result->group_by_values_.value()[offset]);
            if (search_result->element_level_) {
                element_indices[index] =
                    search_result->element_indices_[offset];
            }
            index++;
            real_topks[j]++;
        }
    }
    search_result->primary_keys_.swap(primary_keys);
    search_result->distances_.swap(distances);
    search_result->seg_offsets_.swap(seg_offsets);
    search_result->group_by_values_.value().swap(group_by_values);
    if (search_result->element_level_) {
        search_result->element_indices_.swap(element_indices);
    }
    AssertInfo(search_result->primary_keys_.size() ==
                   search_result->group_by_values_.value().size(),
               "Wrong size for group_by_values size after refresh:{}, "
               "not equal to "
               "primary_keys_.size:{}",
               search_result->group_by_values_.value().size(),
               search_result->primary_keys_.size());
}

void
GroupReduceHelper::FilterInvalidSearchResult(SearchResult* search_result) {
    auto nq = search_result->total_nq_;
    auto topK = search_result->unity_topK_;
    auto has_compact_topk_prefix =
        search_result->topk_per_nq_prefix_sum_.size() == nq + 1 &&
        search_result->topk_per_nq_prefix_sum_.front() == 0 &&
        search_result->topk_per_nq_prefix_sum_.back() ==
            search_result->seg_offsets_.size();
    if (topK > 0 && has_compact_topk_prefix &&
        search_result->seg_offsets_.size() != nq * topK) {
        AssertInfo(search_result->distances_.size() ==
                       search_result->seg_offsets_.size(),
                   "wrong distances size, size = {}, expected size = {}",
                   search_result->distances_.size(),
                   search_result->seg_offsets_.size());
        AssertInfo(search_result->group_by_values_.has_value(),
                   "no group by values for search result, group reducer should "
                   "not be called, wrong code");
        AssertInfo(search_result->group_by_values_.value().size() ==
                       search_result->seg_offsets_.size(),
                   "Wrong size for group_by_values size before filter:{}, "
                   "not equal to seg_offsets size:{}",
                   search_result->group_by_values_.value().size(),
                   search_result->seg_offsets_.size());
        CheckElementIndicesSize(search_result,
                                search_result->seg_offsets_.size(),
                                "group filter compact result");

        for (auto i = 0; i < nq; ++i) {
            AssertInfo(search_result->topk_per_nq_prefix_sum_[i] <=
                           search_result->topk_per_nq_prefix_sum_[i + 1],
                       "incorrect topk_per_nq_prefix_sum_ in compact group "
                       "search result");
        }

        for (auto offset : search_result->seg_offsets_) {
            if (offset == INVALID_SEG_OFFSET) {
                continue;
            }
            auto segment =
                static_cast<SegmentInterface*>(search_result->segment_);
            int segment_row_count = segment->get_row_count();
            AssertInfo(0 <= offset && offset < segment_row_count,
                       fmt::format("invalid offset {}, segment {} with "
                                   "rows num {}, data or index corruption",
                                   offset,
                                   segment->get_segment_id(),
                                   segment_row_count));
        }
        return;
    }

    AssertInfo(search_result->seg_offsets_.size() == nq * topK,
               "wrong seg offsets size, size = {}, expected size = {}",
               search_result->seg_offsets_.size(),
               nq * topK);
    AssertInfo(search_result->distances_.size() == nq * topK,
               "wrong distances size, size = {}, expected size = {}",
               search_result->distances_.size(),
               nq * topK);
    AssertInfo(search_result->group_by_values_.has_value(),
               "no group by values for search result, group reducer should not "
               "be called, wrong code");
    AssertInfo(search_result->group_by_values_.value().size() ==
                   search_result->seg_offsets_.size(),
               "Wrong size for group_by_values size before filter:{}, "
               "not equal to seg_offsets size:{}",
               search_result->group_by_values_.value().size(),
               search_result->seg_offsets_.size());
    CheckElementIndicesSize(search_result,
                            search_result->seg_offsets_.size(),
                            "group filter invalid result");

    auto& group_by_values = search_result->group_by_values_.value();
    CompactSearchResult(search_result, &group_by_values);
}

int64_t
GroupReduceHelper::ReduceSearchResultForOneNQ(int64_t qi,
                                              int64_t topk,
                                              int64_t& offset) {
    std::priority_queue<SearchResultPair*,
                        std::vector<SearchResultPair*>,
                        SearchResultPairComparator>
        heap;
    pk_set_.clear();
    element_result_set_.clear();
    pairs_.clear();

    pairs_.reserve(num_segments_);
    for (int i = 0; i < num_segments_; i++) {
        auto search_result = search_results_[i];
        auto offset_beg = search_result->topk_per_nq_prefix_sum_[qi];
        auto offset_end = search_result->topk_per_nq_prefix_sum_[qi + 1];
        if (offset_beg == offset_end) {
            continue;
        }
        auto primary_key = search_result->primary_keys_[offset_beg];
        auto distance = search_result->distances_[offset_beg];
        pairs_.emplace_back(
            primary_key, distance, search_result, i, offset_beg, offset_end);
        heap.push(&pairs_.back());
    }

    if (heap.empty()) {
        return 0;
    }

    int64_t dup_cnt = 0;
    auto start = offset;
    auto group_size = int64_t(1);
    for (auto search_result : search_results_) {
        if (search_result->group_size_.has_value()) {
            group_size = search_result->group_size_.value();
            break;
        }
    }
    auto result_limit = topk * group_size;
    std::unordered_map<GroupByValueType, int64_t> group_counts;
    while (offset - start < result_limit && !heap.empty()) {
        auto pilot = heap.top();
        heap.pop();

        auto index = pilot->segment_index_;
        auto pk = pilot->primary_key_;
        if (pk == INVALID_PK) {
            break;
        }

        auto& search_result = *pilot->search_result_;
        auto& group_by_values = search_result.group_by_values_.value();
        AssertInfo(pilot->offset_ >= 0 && static_cast<size_t>(pilot->offset_) <
                                              group_by_values.size(),
                   "invalid group by value offset {}, group_by_values size {}",
                   pilot->offset_,
                   group_by_values.size());
        auto& group_by_value = group_by_values[pilot->offset_];

        auto group_iter = group_counts.find(group_by_value);
        auto is_new_group = group_iter == group_counts.end();
        auto group_is_full = !is_new_group && group_iter->second >= group_size;
        auto group_capacity_is_full =
            is_new_group && static_cast<int64_t>(group_counts.size()) >= topk;

        if (!group_is_full && !group_capacity_is_full) {
            if (TryAcceptSearchResult(*pilot)) {
                auto& group_count = group_counts[group_by_value];
                group_count++;
                pilot->search_result_->result_offsets_.push_back(offset++);
                final_search_records_[index][qi].push_back(pilot->offset_);
            } else {
                dup_cnt++;
            }
        }

        pilot->advance();
        if (pilot->primary_key_ != INVALID_PK) {
            heap.push(pilot);
        }
    }
    return dup_cnt;
}

}  // namespace milvus::segcore
