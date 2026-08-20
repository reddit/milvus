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

#include "LogicalBinaryExpr.h"

namespace milvus {
namespace exec {

void
PhyLogicalBinaryExpr::Eval(EvalCtx& context, VectorPtr& result) {
    tracer::AutoSpan span("PhyLogicalBinaryExpr::Eval", tracer::GetRootSpan());

    AssertInfo(
        inputs_.size() == 2,
        "logical binary expr must have 2 inputs, but {} inputs are provided",
        inputs_.size());
    VectorPtr left;
    inputs_[0]->Eval(context, left);
    VectorPtr right;
    inputs_[1]->Eval(context, right);
    auto lflat = GetBitmapVector(left);
    auto rflat = GetBitmapVector(right);
    if (expr_->op_type_ == expr::LogicalBinaryExpr::OpType::And) {
        lflat->And(*rflat);
    } else if (expr_->op_type_ == expr::LogicalBinaryExpr::OpType::Or) {
        lflat->Or(*rflat);
    } else {
        ThrowInfo(OpTypeInvalid,
                  "unsupported logical operator: {}",
                  expr_->GetOpTypeString());
    }
    result = std::move(lflat);
}

}  //namespace exec
}  // namespace milvus
