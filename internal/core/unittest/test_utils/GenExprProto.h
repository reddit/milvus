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

#pragma once

#include <memory>
#include <type_traits>
#include <vector>

#include "common/Consts.h"
#include "expr/ITypeExpr.h"
#include "exec/expression/Expr.h"
#include "exec/expression/Utils.h"
#include "pb/plan.pb.h"
#include "plan/PlanNode.h"
#include <typeinfo>

namespace milvus::test {

template <typename T>
inline constexpr bool always_false = false;

inline auto
GenColumnInfo(
    int64_t field_id,
    proto::schema::DataType field_type,
    bool auto_id,
    bool is_pk,
    proto::schema::DataType element_type = proto::schema::DataType::None,
    bool nullable = false) {
    auto column_info = new proto::plan::ColumnInfo();
    column_info->set_field_id(field_id);
    column_info->set_data_type(field_type);
    column_info->set_is_autoid(auto_id);
    column_info->set_is_primary_key(is_pk);
    column_info->set_element_type(element_type);
    column_info->set_nullable(nullable);
    return column_info;
}

template <typename T>
auto
GenGenericValue(T value) {
    auto generic = new proto::plan::GenericValue();
    if constexpr (std::is_same_v<T, bool>) {
        generic->set_bool_val(static_cast<bool>(value));
    } else if constexpr (std::is_integral_v<T>) {
        generic->set_int64_val(static_cast<int64_t>(value));
    } else if constexpr (std::is_floating_point_v<T>) {
        generic->set_float_val(static_cast<float>(value));
    } else if constexpr (std::is_same_v<T, std::string>) {
        generic->set_string_val(static_cast<std::string>(value));
    } else if constexpr (std::is_same_v<T, const char*> ||
                         std::is_same_v<std::decay_t<T>, const char*> ||
                         (std::is_array_v<T> &&
                          std::is_same_v<std::remove_extent_t<T>,
                                         const char>)) {
        generic->set_string_val(std::string(value));
    } else {
        static_assert(always_false<T>);
    }
    return generic;
}

template <typename T>
auto
GenUnaryRangeExpr(proto::plan::OpType op, T& value) {
    auto unary_range_expr = new proto::plan::UnaryRangeExpr();
    unary_range_expr->set_op(op);
    auto generic = GenGenericValue(value);
    unary_range_expr->set_allocated_value(generic);
    return unary_range_expr;
}

inline auto
GenNullExpr(NullExprType op) {
    auto null_expr = new proto::plan::NullExpr();
    null_expr->set_op(op);
    return null_expr;
}

inline auto
GenExpr() {
    return std::make_unique<proto::plan::Expr>();
}

inline std::shared_ptr<milvus::plan::PlanNode>
CreateRetrievePlanByExpr(std::shared_ptr<milvus::expr::ITypeExpr> expr) {
    auto init_plannode_id = std::stoi(DEFAULT_PLANNODE_ID);
    milvus::plan::PlanNodePtr plannode;
    std::vector<milvus::plan::PlanNodePtr> sources;

    plannode =
        std::make_shared<plan::FilterBitsNode>(DEFAULT_PLANNODE_ID, expr);
    sources = std::vector<milvus::plan::PlanNodePtr>{plannode};

    plannode = std::make_shared<milvus::plan::MvccNode>(
        std::to_string(init_plannode_id++), sources);
    return plannode;
}

inline std::shared_ptr<milvus::plan::PlanNode>
CreateRetrievePlanForRandomSample(
    float sample_factor,
    std::shared_ptr<milvus::expr::ITypeExpr> expr = nullptr) {
    auto init_plannode_id = std::stoi(DEFAULT_PLANNODE_ID);
    milvus::plan::PlanNodePtr plannode;
    std::vector<milvus::plan::PlanNodePtr> sources;

    if (expr) {
        plannode = std::make_shared<plan::FilterBitsNode>(
            std::to_string(init_plannode_id++), expr, sources);
        sources = std::vector<milvus::plan::PlanNodePtr>{plannode};
    }

    plannode = std::make_shared<plan::RandomSampleNode>(
        DEFAULT_PLANNODE_ID, sample_factor, sources);
    sources = std::vector<milvus::plan::PlanNodePtr>{plannode};

    plannode = std::make_shared<milvus::plan::MvccNode>(
        std::to_string(init_plannode_id++), sources);
    return plannode;
}

inline std::shared_ptr<milvus::plan::PlanNode>
CreateSearchPlanByExpr(std::shared_ptr<milvus::expr::ITypeExpr> expr) {
    auto init_plannode_id = std::stoi(DEFAULT_PLANNODE_ID);
    milvus::plan::PlanNodePtr plannode;
    std::vector<milvus::plan::PlanNodePtr> sources;

    plannode =
        std::make_shared<plan::FilterBitsNode>(DEFAULT_PLANNODE_ID, expr);
    sources = std::vector<milvus::plan::PlanNodePtr>{plannode};

    plannode = std::make_shared<milvus::plan::MvccNode>(
        std::to_string(init_plannode_id++), sources);
    sources = std::vector<milvus::plan::PlanNodePtr>{plannode};

    plannode = std::make_shared<milvus::plan::VectorSearchNode>(
        std::to_string(init_plannode_id++), sources);

    return plannode;
}

inline ColumnVectorPtr
gen_filter_res(milvus::plan::PlanNode* plan_node,
               const milvus::segcore::SegmentInternalInterface* segment,
               uint64_t active_count,
               uint64_t timestamp,
               FixedVector<int32_t>* offsets = nullptr) {
    auto filter_node = dynamic_cast<milvus::plan::FilterBitsNode*>(plan_node);
    assert(filter_node != nullptr);
    std::vector<milvus::expr::TypedExprPtr> filters;
    filters.emplace_back(filter_node->filter());
    auto query_context = std::make_shared<milvus::exec::QueryContext>(
        DEAFULT_QUERY_ID, segment, active_count, timestamp);

    std::unique_ptr<milvus::exec::ExecContext> exec_context =
        std::make_unique<milvus::exec::ExecContext>(query_context.get());
    auto exprs_ =
        std::make_unique<milvus::exec::ExprSet>(filters, exec_context.get());
    std::vector<VectorPtr> results_;
    milvus::exec::EvalCtx eval_ctx(exec_context.get(), exprs_.get());
    eval_ctx.set_offset_input(offsets);
    exprs_->Eval(0, 1, true, eval_ctx, results_);

    if (results_.empty() || results_[0] == nullptr) {
        auto size = offsets == nullptr ? active_count : offsets->size();
        return std::make_shared<milvus::ColumnVector>(
            TargetBitmap(size, false), TargetBitmap(size, true));
    }
    if (typeid(*results_[0]) == typeid(milvus::ColumnVector)) {
        return std::static_pointer_cast<milvus::ColumnVector>(results_[0]);
    }
    if (std::dynamic_pointer_cast<milvus::RowVector>(results_[0])) {
        return milvus::exec::GetColumnVector(results_[0]);
    }
    if (auto constant =
            std::dynamic_pointer_cast<milvus::ConstantVector<bool>>(
                results_[0])) {
        auto size = constant->size();
        auto valid = !constant->IsNull();
        auto value = constant->GetValue() && valid;
        return std::make_shared<milvus::ColumnVector>(
            TargetBitmap(size, value),
            TargetBitmap(size, valid));
    }
    if (auto simple = std::dynamic_pointer_cast<milvus::SimpleVector>(
            results_[0])) {
        auto size = simple->size();
        TargetBitmap bitmap(size, false);
        TargetBitmap valid_bitmap(size, false);
        for (auto i = 0; i < size; ++i) {
            auto valid = simple->ValidAt(i);
            valid_bitmap.set(i, valid);
            if (!valid) {
                continue;
            }
            bool value = false;
            switch (simple->type()) {
                case DataType::NONE:
                    value = *static_cast<int8_t*>(
                                simple->RawValueAt(i, sizeof(int8_t))) != 0;
                    break;
                case DataType::BOOL:
                    value =
                        *static_cast<bool*>(simple->RawValueAt(i, sizeof(bool)));
                    break;
                case DataType::INT8:
                    value = *static_cast<int8_t*>(
                                simple->RawValueAt(i, sizeof(int8_t))) != 0;
                    break;
                default:
                    ThrowInfo(
                        UnexpectedError,
                        "expr result simple vector must be bool-like, got {}",
                        static_cast<int>(simple->type()));
            }
            bitmap.set(i, value);
        }
        return std::make_shared<milvus::ColumnVector>(std::move(bitmap),
                                                      std::move(valid_bitmap));
    }
    auto col_vec = milvus::exec::GetColumnVector(results_[0]);
    return col_vec;
}

}  // namespace milvus::test
