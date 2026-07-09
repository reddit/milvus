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

#pragma once

#include <roaring/roaring.h>

#include <algorithm>
#include <limits>
#include <memory>

#include "common/EasyAssert.h"
#include "common/Vector.h"

namespace milvus {

class RoaringBitmapVector final : public BaseVector {
 public:
    RoaringBitmapVector(TargetBitmap&& bitset, TargetBitmap&& valid_bitset)
        : BaseVector(DataType::INT8, bitset.size()),
          bitmap_(roaring_bitmap_create()),
          valid_values_all_valid_(false),
          valid_values_(std::move(valid_bitset)) {
        AssertInfo(bitmap_ != nullptr, "failed to create roaring bitmap");
        AssertInfo(valid_values_.size() == length_,
                   "valid bitmap size {} does not match bitmap size {}",
                   valid_values_.size(),
                   length_);

        for (auto offset = bitset.find_first(true); offset.has_value();
             offset = bitset.find_next(offset.value(), true)) {
            AssertInfo(offset.value() <= std::numeric_limits<uint32_t>::max(),
                       "roaring bitmap only supports uint32 ids, got {}",
                       offset.value());
            roaring_bitmap_add(bitmap_.get(),
                               static_cast<uint32_t>(offset.value()));
        }
    }

    explicit RoaringBitmapVector(size_t size, bool valid = true)
        : BaseVector(DataType::INT8, size),
          bitmap_(roaring_bitmap_create()),
          valid_values_all_valid_(valid),
          valid_values_(valid ? TargetBitmap{} : TargetBitmap(size, false)) {
        AssertInfo(bitmap_ != nullptr, "failed to create roaring bitmap");
    }

    RoaringBitmapVector(size_t size, TargetBitmap&& valid_values)
        : BaseVector(DataType::INT8, size),
          bitmap_(roaring_bitmap_create()),
          valid_values_all_valid_(false),
          valid_values_(std::move(valid_values)) {
        AssertInfo(bitmap_ != nullptr, "failed to create roaring bitmap");
        AssertInfo(valid_values_.size() == length_,
                   "valid bitmap size {} does not match bitmap size {}",
                   valid_values_.size(),
                   length_);
    }

    ColumnVectorPtr
    ToColumnVector() const {
        auto bitset = ToTargetBitmap();
        auto valid_values = ValidValuesBitmap().clone();
        return std::make_shared<ColumnVector>(std::move(bitset),
                                              std::move(valid_values));
    }

    TargetBitmap
    ToTargetBitmap() const {
        TargetBitmap bitset(length_, false);
        IterateTrueBits([&](uint32_t offset) {
            if (offset < length_) {
                bitset[offset] = true;
            }
            return true;
        });
        return bitset;
    }

    size_t
    size() const {
        return length_;
    }

    void
    Add(size_t offset) {
        AssertInfo(offset <= std::numeric_limits<uint32_t>::max(),
                   "roaring bitmap only supports uint32 ids, got {}",
                   offset);
        roaring_bitmap_add(bitmap_.get(), static_cast<uint32_t>(offset));
    }

    void
    AddMany(const uint32_t* offsets, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (offsets[i] < length_) {
                roaring_bitmap_add(bitmap_.get(), offsets[i]);
            }
        }
    }

    void
    AddRange(size_t begin, size_t end) {
        if (begin >= end) {
            return;
        }
        AssertInfo(end - 1 <= std::numeric_limits<uint32_t>::max(),
                   "roaring bitmap only supports uint32 ids, got {}",
                   end - 1);
        roaring_bitmap_add_range(bitmap_.get(), begin, end);
    }

    void
    Flip() {
        if (length_ == 0) {
            return;
        }
        roaring_bitmap_flip_inplace(bitmap_.get(), 0, length_);
    }

    void
    SetAll() {
        AddRange(0, length_);
    }

    void
    Or(const TargetBitmap& bitset, size_t limit) {
        auto or_size = std::min({bitset.size(), length_, limit});
        for (auto offset = bitset.find_first(true);
             offset.has_value() && offset.value() < or_size;
             offset = bitset.find_next(offset.value(), true)) {
            Add(offset.value());
        }
    }

    void
    OrShifted(const RoaringBitmapVector& bitset, size_t offset) {
        bitset.IterateTrueBits([&](uint32_t value) {
            Add(offset + value);
            return true;
        });
    }

    std::shared_ptr<RoaringBitmapVector>
    Slice(size_t offset, size_t size) const {
        AssertInfo(offset + size <= length_,
                   "roaring bitmap slice [{}, {}) exceeds size {}",
                   offset,
                   offset + size,
                   length_);
        auto sliced = std::make_shared<RoaringBitmapVector>(size, false);
        if (valid_values_all_valid_) {
            sliced->valid_values_all_valid_ = true;
            sliced->valid_values_.clear();
        } else {
            sliced->valid_values_.clear();
            sliced->valid_values_.append(valid_values_, offset, size);
        }
        IterateTrueBits([&](uint32_t value) {
            if (value >= offset && value < offset + size) {
                sliced->Add(value - offset);
            }
            return value < offset + size;
        });
        return sliced;
    }

    size_t
    count() const {
        return roaring_bitmap_get_cardinality(bitmap_.get());
    }

    const roaring_bitmap_t*
    bitmap() const {
        return bitmap_.get();
    }

    const TargetBitmap&
    valid_values() const {
        return ValidValuesBitmap();
    }

    bool
    valid_values_all_valid() const {
        return valid_values_all_valid_;
    }

    void
    AppendValidValuesTo(TargetBitmap& dst, size_t offset, size_t size) const {
        AssertInfo(offset + size <= length_,
                   "valid bitmap slice [{}, {}) exceeds size {}",
                   offset,
                   offset + size,
                   length_);
        if (valid_values_all_valid_) {
            dst.append(TargetBitmap(size, true));
        } else {
            dst.append(valid_values_, offset, size);
        }
    }

    void
    set_valid_values(TargetBitmap&& valid_values) {
        AssertInfo(valid_values.size() == length_,
                   "valid bitmap size {} does not match bitmap size {}",
                   valid_values.size(),
                   length_);
        valid_values_all_valid_ = false;
        valid_values_ = std::move(valid_values);
    }

 private:
    struct RoaringDeleter {
        void
        operator()(roaring_bitmap_t* ptr) const {
            roaring_bitmap_free(ptr);
        }
    };

    template <typename Func>
    void
    IterateTrueBits(Func&& func) const {
        struct Context {
            Func* func;
        } context{&func};

        roaring_iterate(
            bitmap_.get(),
            [](uint32_t value, void* param) -> bool {
                auto* context = static_cast<Context*>(param);
                return (*context->func)(value);
            },
            &context);
    }

    const TargetBitmap&
    ValidValuesBitmap() const {
        if (valid_values_all_valid_) {
            valid_values_ = TargetBitmap(length_, true);
            valid_values_all_valid_ = false;
        }
        return valid_values_;
    }

    std::unique_ptr<roaring_bitmap_t, RoaringDeleter> bitmap_;
    mutable bool valid_values_all_valid_{true};
    mutable TargetBitmap valid_values_;
};

using RoaringBitmapVectorPtr = std::shared_ptr<RoaringBitmapVector>;

}  // namespace milvus
