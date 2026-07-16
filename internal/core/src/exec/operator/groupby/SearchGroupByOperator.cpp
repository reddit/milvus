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
#include "SearchGroupByOperator.h"
#include <variant>

#include "common/Consts.h"
#include "query/Utils.h"
#include "common/JsonUtils.h"

namespace milvus {
namespace exec {

using GroupByDataGetter =
    std::variant<std::shared_ptr<DataGetter<bool>>,
                 std::shared_ptr<DataGetter<int8_t>>,
                 std::shared_ptr<DataGetter<int16_t>>,
                 std::shared_ptr<DataGetter<int32_t>>,
                 std::shared_ptr<DataGetter<int64_t>>,
                 std::shared_ptr<DataGetter<std::string>>>;

static GroupByDataGetter
GetGroupByDataGetter(milvus::OpContext* op_ctx,
                     const SearchInfo& search_info,
                     const segcore::SegmentInternalInterface& segment) {
    auto group_by_field_id = search_info.group_by_field_id_.value();
    auto data_type = segment.GetFieldDataType(group_by_field_id);
    switch (data_type) {
        case DataType::BOOL:
            return GetDataGetter<bool>(op_ctx, segment, group_by_field_id);
        case DataType::INT8:
            return GetDataGetter<int8_t>(op_ctx, segment, group_by_field_id);
        case DataType::INT16:
            return GetDataGetter<int16_t>(op_ctx, segment, group_by_field_id);
        case DataType::INT32:
            return GetDataGetter<int32_t>(op_ctx, segment, group_by_field_id);
        case DataType::INT64:
        case DataType::TIMESTAMPTZ:
            return GetDataGetter<int64_t>(op_ctx, segment, group_by_field_id);
        case DataType::VARCHAR:
            return GetDataGetter<std::string>(
                op_ctx, segment, group_by_field_id);
        case DataType::JSON: {
            AssertInfo(search_info.json_path_.has_value(),
                       "json_path is required for json field when doing "
                       "search_group_by");
            auto get_json_data = [&](auto type_tag) -> GroupByDataGetter {
                using T = decltype(type_tag);
                return GetDataGetter<T, milvus::Json>(op_ctx,
                                                      segment,
                                                      group_by_field_id,
                                                      search_info.json_path_,
                                                      search_info.json_type_,
                                                      search_info.strict_cast_);
            };
            auto json_type = search_info.json_type_.value_or(DataType::VARCHAR);
            switch (json_type) {
                case DataType::BOOL:
                    return get_json_data(bool{});
                case DataType::INT8:
                    return get_json_data(int8_t{});
                case DataType::INT16:
                    return get_json_data(int16_t{});
                case DataType::INT32:
                    return get_json_data(int32_t{});
                case DataType::INT64:
                    return get_json_data(int64_t{});
                case DataType::VARCHAR:
                    return get_json_data(std::string{});
                default:
                    ThrowInfo(
                        Unsupported,
                        fmt::format("unsupported data type {} for group by "
                                    "operator",
                                    json_type));
            }
        }
        default:
            ThrowInfo(
                Unsupported,
                fmt::format("unsupported data type {} for group by operator",
                            data_type));
    }
    return {};
}

void
SearchGroupBy(milvus::OpContext* op_ctx,
              const std::vector<std::shared_ptr<VectorIterator>>& iterators,
              const SearchInfo& search_info,
              std::vector<GroupByValueType>& group_by_values,
              const segcore::SegmentInternalInterface& segment,
              std::vector<int64_t>& seg_offsets,
              std::vector<float>& distances,
              std::vector<size_t>& topk_per_nq_prefix_sum) {
    int max_total_size =
        search_info.topk_ * search_info.group_size_ * iterators.size();
    seg_offsets.reserve(max_total_size);
    distances.reserve(max_total_size);
    group_by_values.reserve(max_total_size);
    topk_per_nq_prefix_sum.reserve(iterators.size() + 1);

    std::visit(
        [&](const auto& data_getter) {
            GroupIteratorsByType(iterators,
                                 search_info.topk_,
                                 search_info.group_size_,
                                 search_info.strict_group_size_,
                                 data_getter,
                                 group_by_values,
                                 seg_offsets,
                                 distances,
                                 search_info.metric_type_,
                                 topk_per_nq_prefix_sum);
        },
        GetGroupByDataGetter(op_ctx, search_info, segment));
}

template <typename T>
static void
PopulateGroupByValuesByType(const std::shared_ptr<DataGetter<T>>& data_getter,
                            std::vector<GroupByValueType>& group_by_values,
                            const std::vector<int64_t>& seg_offsets) {
    group_by_values.reserve(seg_offsets.size());
    for (const auto offset : seg_offsets) {
        if (offset == INVALID_SEG_OFFSET) {
            group_by_values.emplace_back(std::nullopt);
            continue;
        }
        group_by_values.emplace_back(data_getter->Get(offset));
    }
}

void
PopulateGroupByValues(milvus::OpContext* op_ctx,
                      const SearchInfo& search_info,
                      std::vector<GroupByValueType>& group_by_values,
                      const segcore::SegmentInternalInterface& segment,
                      const std::vector<int64_t>& seg_offsets) {
    std::visit(
        [&](const auto& data_getter) {
            PopulateGroupByValuesByType(
                data_getter, group_by_values, seg_offsets);
        },
        GetGroupByDataGetter(op_ctx, search_info, segment));
}

template <typename T>
void
GroupIteratorsByType(
    const std::vector<std::shared_ptr<VectorIterator>>& iterators,
    int64_t topK,
    int64_t group_size,
    bool strict_group_size,
    const std::shared_ptr<DataGetter<T>>& data_getter,
    std::vector<GroupByValueType>& group_by_values,
    std::vector<int64_t>& seg_offsets,
    std::vector<float>& distances,
    const knowhere::MetricType& metrics_type,
    std::vector<size_t>& topk_per_nq_prefix_sum) {
    topk_per_nq_prefix_sum.push_back(0);
    for (auto& iterator : iterators) {
        GroupIteratorResult<T>(iterator,
                               topK,
                               group_size,
                               strict_group_size,
                               data_getter,
                               group_by_values,
                               seg_offsets,
                               distances,
                               metrics_type);
        topk_per_nq_prefix_sum.push_back(seg_offsets.size());
    }
}

template <typename T>
void
GroupIteratorResult(const std::shared_ptr<VectorIterator>& iterator,
                    int64_t topK,
                    int64_t group_size,
                    bool strict_group_size,
                    const std::shared_ptr<DataGetter<T>>& data_getter,
                    std::vector<GroupByValueType>& group_by_values,
                    std::vector<int64_t>& offsets,
                    std::vector<float>& distances,
                    const knowhere::MetricType& metrics_type) {
    //1.
    GroupByMap<T> groupMap(topK, group_size, strict_group_size);

    //2. do iteration until fill the whole map or run out of all data
    //note it may enumerate all data inside a segment and can block following
    //query and search possibly
    std::vector<std::tuple<int64_t, float, std::optional<T>>> res;
    while (iterator->HasNext() && !groupMap.IsGroupResEnough()) {
        auto offset_dis_pair = iterator->Next();
        AssertInfo(
            offset_dis_pair.has_value(),
            "Wrong state! iterator cannot return valid result whereas it still"
            "tells hasNext, terminate groupBy operation");
        auto offset = offset_dis_pair.value().first;
        auto dis = offset_dis_pair.value().second;
        std::optional<T> row_data = data_getter->Get(offset);
        if (groupMap.Push(row_data)) {
            res.emplace_back(offset, dis, row_data);
        }
    }

    //3. sorted based on distances and metrics
    auto customComparator = [&](const auto& lhs, const auto& rhs) {
        return milvus::query::dis_closer(
            std::get<1>(lhs), std::get<1>(rhs), metrics_type);
    };
    std::sort(res.begin(), res.end(), customComparator);

    //4. save groupBy results
    for (auto iter = res.begin(); iter != res.end(); ++iter) {
        offsets.emplace_back(std::get<0>(*iter));
        distances.emplace_back(std::get<1>(*iter));
        group_by_values.emplace_back(std::move(std::get<2>(*iter)));
    }
}

}  // namespace exec
}  // namespace milvus
