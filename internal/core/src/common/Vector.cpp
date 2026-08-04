#include "common/Vector.h"

namespace milvus {

ColumnVector::~ColumnVector() {
    values_.reset();
    valid_values_.reset();
}

}  // namespace milvus
