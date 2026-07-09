// Licensed to the LF AI & Data foundation under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "VectorSearchNode.h"
#include "common/ArrayOffsets.h"
#include "common/BitsetView.h"
#include "common/Tracer.h"
#include "common/Vector.h"
#include "fmt/format.h"

#include "monitor/Monitor.h"
namespace milvus {
namespace exec {
namespace {

RoaringBitmapVectorPtr
GetRoaringBitmapVector(const VectorPtr& input) {
    if (auto roaring = std::dynamic_pointer_cast<RoaringBitmapVector>(input)) {
        return roaring;
    }

    if (auto row = std::dynamic_pointer_cast<RowVector>(input)) {
        auto child = row->child(0);
        if (auto roaring =
                std::dynamic_pointer_cast<RoaringBitmapVector>(child)) {
            return roaring;
        }
    }

    return nullptr;
}

}  // namespace

static milvus::SearchResult
empty_search_result(int64_t num_queries, bool element_level) {
    milvus::SearchResult final_result;
    final_result.total_nq_ = num_queries;
    final_result.unity_topK_ = 0;  // no result
    final_result.total_data_cnt_ = 0;
    final_result.element_level_ = element_level;
    return final_result;
}

PhyVectorSearchNode::PhyVectorSearchNode(
    int32_t operator_id,
    DriverContext* driverctx,
    const std::shared_ptr<const plan::VectorSearchNode>& search_node)
    : Operator(driverctx,
               search_node->output_type(),
               operator_id,
               search_node->id(),
               "PhyVectorSearchNode") {
    ExecContext* exec_context = operator_context_->get_exec_context();
    query_context_ = exec_context->get_query_context();
    segment_ = query_context_->get_segment();
    query_timestamp_ = query_context_->get_query_timestamp();
    active_count_ = query_context_->get_active_count();
    placeholder_group_ = query_context_->get_placeholder_group();
    search_info_ = query_context_->get_search_info();
}

void
PhyVectorSearchNode::AddInput(RowVectorPtr& input) {
    input_ = std::move(input);
}

RowVectorPtr
PhyVectorSearchNode::GetOutput() {
    milvus::exec::checkCancellation(query_context_);

    if (is_finished_ || !no_more_input_) {
        return nullptr;
    }

    tracer::AutoSpan span(
        "PhyVectorSearchNode::Execute", tracer::GetRootSpan(), true);

    DeferLambda([&]() { is_finished_ = true; });
    if (input_ == nullptr) {
        return nullptr;
    }

    span.GetSpan()->SetAttribute("search_type", search_info_.metric_type_);
    span.GetSpan()->SetAttribute("topk", search_info_.topk_);

    std::chrono::high_resolution_clock::time_point vector_start =
        std::chrono::high_resolution_clock::now();

    auto& ph = placeholder_group_->at(0);
    auto src_data = ph.get_blob();
    auto src_offsets = ph.get_offsets();
    auto num_queries = ph.num_of_queries_;
    int64_t data_cnt = active_count_;
    std::shared_ptr<const IArrayOffsets> array_offsets = nullptr;
    if (ph.element_level_) {
        array_offsets = segment_->GetArrayOffsets(search_info_.field_id_);
        AssertInfo(array_offsets != nullptr, "Array offsets not available");
        query_context_->set_array_offsets(array_offsets);
        search_info_.array_offsets_ = array_offsets;

        if (!query_context_->bitset_is_element_level()) {
            auto row_roaring = GetRoaringBitmapVector(input_);
            if (row_roaring == nullptr) {
                row_roaring = RoaringBitmapVector::FromColumnVector(
                    GetColumnVector(input_));
            }

            auto row_count = row_roaring->size();
            auto element_range = array_offsets->ElementIDRangeOfRow(row_count);
            auto element_start = element_range.first;
            auto element_end = element_range.second;
            data_cnt = element_end - element_start;
            query_context_->set_active_element_count(data_cnt);

            auto element_roaring =
                std::make_shared<RoaringBitmapVector>(data_cnt, true);
            auto excluded_row_count = row_roaring->count();
            auto selected_row_count = row_count - excluded_row_count;
            auto add_element_range = [&](uint32_t row_id) {
                auto [start, end] = array_offsets->ElementIDRangeOfRow(row_id);
                element_roaring->AddRange(start - element_start,
                                          end - element_start);
            };
            auto remove_element_range = [&](uint32_t row_id) {
                auto [start, end] = array_offsets->ElementIDRangeOfRow(row_id);
                element_roaring->RemoveRange(start - element_start,
                                             end - element_start);
            };

            if (selected_row_count <= excluded_row_count) {
                element_roaring->SetAll();
                for (uint32_t i = 0; i < selected_row_count; ++i) {
                    remove_element_range(row_roaring->SelectFalse(i));
                }
            } else {
                for (uint32_t i = 0; i < excluded_row_count; ++i) {
                    add_element_range(row_roaring->SelectTrue(i));
                }
            }

            AssertInfo(row_roaring->valid_values_all_valid(),
                       "element-level roaring bitset conversion does not "
                       "support invalid row bitsets");
            std::vector<VectorPtr> col_res;
            col_res.push_back(std::move(element_roaring));
            input_ = std::make_shared<RowVector>(col_res);
        }
    }

    auto roaring_input = GetRoaringBitmapVector(input_);
    ColumnVectorPtr col_input = nullptr;
    if (roaring_input == nullptr) {
        col_input = GetColumnVector(input_);
    }

    // Prepare BitsetView for search.
    // Fast path: all_rows_visible + non-element-level -> empty BitsetView
    //            (IDSelectorAll in Knowhere, skips per-vector bit test).
    // Normal path: build BitsetView from the bitmap produced upstream.
    milvus::BitsetView search_view;
    std::unique_ptr<milvus::FrozenRoaringBitsetView> frozen_search_view;

    if (query_context_->get_all_rows_visible() && !ph.element_level_) {
        // search_view stays default-constructed (empty)
    } else {
        if (roaring_input != nullptr) {
            if (roaring_input->count() == roaring_input->size()) {
                query_context_->set_search_result(std::move(
                    empty_search_result(num_queries, ph.element_level_)));
                return input_;
            }
            if (roaring_input->count() > 0) {
                frozen_search_view =
                    std::make_unique<milvus::FrozenRoaringBitsetView>(
                        *roaring_input);
                search_view = frozen_search_view->view();
            }
            data_cnt = roaring_input->size();
        } else {
            TargetBitmapView view(col_input->GetRawData(), col_input->size());

            if (view.all()) {
                query_context_->set_search_result(std::move(
                    empty_search_result(num_queries, ph.element_level_)));
                return input_;
            }

            if (!view.none()) {
                frozen_search_view =
                    std::make_unique<milvus::FrozenRoaringBitsetView>(view);
                search_view = frozen_search_view->view();
            }
            data_cnt = col_input->size();
        }
    }

    // Single search + metrics path
    milvus::SearchResult search_result;
    auto op_context = query_context_->get_op_context();
    segment_->vector_search(search_info_,
                            src_data,
                            src_offsets,
                            num_queries,
                            query_timestamp_,
                            search_view,
                            op_context,
                            search_result);

    search_result.total_data_cnt_ = data_cnt;
    search_result.element_level_ = ph.element_level_;

    span.GetSpan()->SetAttribute(
        "result_count", static_cast<int>(search_result.seg_offsets_.size()));
    query_context_->set_search_result(std::move(search_result));

    std::chrono::high_resolution_clock::time_point vector_end =
        std::chrono::high_resolution_clock::now();
    double vector_cost =
        std::chrono::duration<double, std::micro>(vector_end - vector_start)
            .count();
    milvus::monitor::internal_core_search_latency_vector.Observe(vector_cost /
                                                                 1000);
    // vector search stores result in query_context;
    // this node returns the bitset for downstream operators
    return input_;
}

bool
PhyVectorSearchNode::IsFinished() {
    return is_finished_;
}

}  // namespace exec
}  // namespace milvus
