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

#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include "index/TextMatchIndex.h"
#include "common/Utils.h"

namespace milvus::index {

// Test that the BM25 API methods exist and have correct signatures
// Full integration tests would require more complex setup with Tantivy index

TEST(TextMatchIndexBM25Test, APIExists) {
    // This test verifies that the BM25 search API methods are declared
    // in TextMatchIndex and have the correct return types.
    
    // Verify method signatures exist at compile time
    using BM25SearchQueryType = std::pair<std::vector<int64_t>, std::vector<float>> 
        (TextMatchIndex::*)(const std::string&, int64_t);
    using BM25SearchQueryWithMinimumType = std::pair<std::vector<int64_t>, std::vector<float>> 
        (TextMatchIndex::*)(const std::string&, uint32_t, int64_t);
    using BM25PhraseSearchQueryType = std::pair<std::vector<int64_t>, std::vector<float>> 
        (TextMatchIndex::*)(const std::string&, uint32_t, int64_t);
    using BM25SearchQueryWithFilterType = std::pair<std::vector<int64_t>, std::vector<float>> 
        (TextMatchIndex::*)(const std::string&, int64_t, const uint8_t*, size_t);
    
    // These lines verify the method signatures match - they won't compile if wrong
    BM25SearchQueryType bm25_search = &TextMatchIndex::BM25SearchQuery;
    BM25SearchQueryWithMinimumType bm25_search_min = &TextMatchIndex::BM25SearchQueryWithMinimum;
    BM25PhraseSearchQueryType bm25_phrase = &TextMatchIndex::BM25PhraseSearchQuery;
    BM25SearchQueryWithFilterType bm25_filter = &TextMatchIndex::BM25SearchQueryWithFilter;
    
    // Suppress unused variable warnings
    (void)bm25_search;
    (void)bm25_search_min;
    (void)bm25_phrase;
    (void)bm25_filter;
    
    SUCCEED();
}

// Test the common metric type constant
TEST(TextMatchIndexBM25Test, MetricTypeConstant) {
    // Verify TEXT_BM25_METRIC constant exists and has expected value
    EXPECT_STREQ(TEXT_BM25_METRIC, "TEXT_BM25");
}

// Test IsTextSearchMetricType utility function
TEST(TextMatchIndexBM25Test, IsTextSearchMetricType) {
    // The utility function should correctly identify TEXT_BM25 metric type
    EXPECT_TRUE(IsTextSearchMetricType("TEXT_BM25"));
    EXPECT_FALSE(IsTextSearchMetricType("BM25"));
    EXPECT_FALSE(IsTextSearchMetricType("L2"));
    EXPECT_FALSE(IsTextSearchMetricType("IP"));
    EXPECT_FALSE(IsTextSearchMetricType("COSINE"));
    EXPECT_FALSE(IsTextSearchMetricType(""));
}

}  // namespace milvus::index
