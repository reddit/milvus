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

#include "index/TextMatchIndex.h"

namespace milvus::index {

class TextMatchIndexBM25Test : public ::testing::Test {
 protected:
    void SetUp() override {
        // Create a temporary directory for the index
        temp_dir_ = std::filesystem::temp_directory_path() / 
                    ("text_match_bm25_test_" + std::to_string(std::rand()));
        std::filesystem::create_directories(temp_dir_);
        
        // Create a unique ID for the index
        unique_id_ = "test_field_" + std::to_string(std::rand());
    }

    void TearDown() override {
        // Clean up temporary directory
        std::filesystem::remove_all(temp_dir_);
    }

    std::unique_ptr<TextMatchIndex> CreateIndex() {
        return std::make_unique<TextMatchIndex>(
            50000,  // commit_interval_in_ms
            unique_id_.c_str(),
            "milvus_tokenizer",
            "{}");
    }

    std::unique_ptr<TextMatchIndex> CreateIndexWithPath() {
        return std::make_unique<TextMatchIndex>(
            temp_dir_.string(),
            unique_id_.c_str(),
            50000,  // commit_interval_in_ms
            "milvus_tokenizer",
            "{}");
    }

    void AddDocuments(TextMatchIndex* index,
                      const std::vector<std::string>& docs,
                      bool is_nullable = false) {
        index->AddTextsGrowing(docs.size(), docs.data(), nullptr, 0);
        index->Commit();
        index->Reload();
    }

    std::filesystem::path temp_dir_;
    std::string unique_id_;
};

// Test basic BM25 search functionality
TEST_F(TextMatchIndexBM25Test, BasicBM25Search) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {
        "machine learning deep learning",
        "machine learning algorithms",
        "deep neural networks",
        "something completely different",
        "machine learning is great"
    };
    AddDocuments(index.get(), docs);

    // Perform BM25 search
    auto [seg_offsets, scores] = index->BM25SearchQuery("machine learning", 10);

    // Should find matching documents
    ASSERT_GT(seg_offsets.size(), 0) << "Should return some results";
    ASSERT_EQ(seg_offsets.size(), scores.size()) << "Offsets and scores should match";

    // Scores should be positive
    for (const auto& score : scores) {
        EXPECT_GT(score, 0.0f) << "BM25 scores should be positive";
    }

    // Scores should be in descending order
    for (size_t i = 1; i < scores.size(); ++i) {
        EXPECT_GE(scores[i-1], scores[i]) << "Scores should be in descending order";
    }

    // Check that documents with both "machine" and "learning" are found
    bool found_ml_doc = false;
    for (const auto& offset : seg_offsets) {
        if (offset == 0 || offset == 1 || offset == 4) {
            found_ml_doc = true;
            break;
        }
    }
    EXPECT_TRUE(found_ml_doc) << "Should find documents with 'machine learning'";
}

// Test BM25 search respects topk limit
TEST_F(TextMatchIndexBM25Test, BM25SearchTopK) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back("common words document " + std::to_string(i));
    }
    AddDocuments(index.get(), docs);

    // Test topk = 5
    auto [seg_offsets5, scores5] = index->BM25SearchQuery("common words", 5);
    EXPECT_LE(seg_offsets5.size(), 5) << "Should respect topk=5 limit";

    // Test topk = 50
    auto [seg_offsets50, scores50] = index->BM25SearchQuery("common words", 50);
    EXPECT_LE(seg_offsets50.size(), 50) << "Should respect topk=50 limit";
    EXPECT_GE(seg_offsets50.size(), seg_offsets5.size()) 
        << "Larger topk should return at least as many results";
}

// Test BM25 search with empty query
TEST_F(TextMatchIndexBM25Test, BM25SearchEmptyQuery) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {"hello world", "test document"};
    AddDocuments(index.get(), docs);

    auto [seg_offsets, scores] = index->BM25SearchQuery("", 10);
    EXPECT_EQ(seg_offsets.size(), 0) << "Empty query should return no results";
}

// Test BM25 search with no matches
TEST_F(TextMatchIndexBM25Test, BM25SearchNoMatches) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {"hello world", "test document"};
    AddDocuments(index.get(), docs);

    auto [seg_offsets, scores] = index->BM25SearchQuery("xyz123nonexistent", 10);
    EXPECT_EQ(seg_offsets.size(), 0) << "Non-matching query should return no results";
}

// Test BM25 search with filter
TEST_F(TextMatchIndexBM25Test, BM25SearchWithFilter) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {
        "hello world",      // doc 0
        "hello universe",   // doc 1 - will be filtered
        "goodbye world",    // doc 2
        "hello earth",      // doc 3 - will be filtered
        "hello mars"        // doc 4
    };
    AddDocuments(index.get(), docs);

    // Create filter bitset - exclude docs 1 and 3
    // Bitset: bit 1 and bit 3 set = exclude those docs
    std::vector<uint8_t> filter_bitset(1);
    filter_bitset[0] = 0b00001010;  // bits 1 and 3 set
    
    auto [seg_offsets, scores] = index->BM25SearchQueryWithFilter(
        "hello", 10, filter_bitset.data(), 5);

    // Should NOT contain filtered docs (1 and 3)
    for (const auto& offset : seg_offsets) {
        EXPECT_NE(offset, 1) << "Doc 1 should be filtered out";
        EXPECT_NE(offset, 3) << "Doc 3 should be filtered out";
    }

    // Should contain unfiltered docs that match "hello"
    bool has_doc0 = std::find(seg_offsets.begin(), seg_offsets.end(), 0) != seg_offsets.end();
    bool has_doc4 = std::find(seg_offsets.begin(), seg_offsets.end(), 4) != seg_offsets.end();
    EXPECT_TRUE(has_doc0 || has_doc4) << "Should contain unfiltered matching docs";
}

// Test BM25 search with filter that excludes all matches
TEST_F(TextMatchIndexBM25Test, BM25SearchFilterExcludesAll) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {
        "hello world",      // doc 0
        "hello universe",   // doc 1
    };
    AddDocuments(index.get(), docs);

    // Filter out both docs
    std::vector<uint8_t> filter_bitset(1);
    filter_bitset[0] = 0b00000011;  // bits 0 and 1 set
    
    auto [seg_offsets, scores] = index->BM25SearchQueryWithFilter(
        "hello", 10, filter_bitset.data(), 2);

    EXPECT_EQ(seg_offsets.size(), 0) << "All matching docs filtered, should return empty";
}

// Test BM25 search with minimum_should_match
TEST_F(TextMatchIndexBM25Test, BM25SearchWithMinimum) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {
        "a b",       // doc 0 - has 2 terms
        "a c",       // doc 1 - has 1 of query terms
        "b c",       // doc 2 - has 1 of query terms
        "c",         // doc 3 - has 0 of query terms
        "a b c"      // doc 4 - has 3 terms
    };
    AddDocuments(index.get(), docs);

    // Search with min_should_match = 2
    auto [seg_offsets, scores] = index->BM25SearchQueryWithMinimum("a b c", 2, 10);

    // Should find docs with at least 2 matching terms
    ASSERT_GT(seg_offsets.size(), 0) << "Should find matching docs";
    
    // Doc 3 (only "c") should not be included
    bool has_doc3 = std::find(seg_offsets.begin(), seg_offsets.end(), 3) != seg_offsets.end();
    EXPECT_FALSE(has_doc3) << "Doc 3 with only 1 term should not match min=2";

    // Doc 4 (has all 3) should be included
    bool has_doc4 = std::find(seg_offsets.begin(), seg_offsets.end(), 4) != seg_offsets.end();
    EXPECT_TRUE(has_doc4) << "Doc 4 with 3 terms should match min=2";
}

// Test BM25 phrase search
TEST_F(TextMatchIndexBM25Test, BM25PhraseSearch) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {
        "machine learning is great",   // doc 0 - exact phrase
        "learning machine works",      // doc 1 - reversed
        "machine and learning",        // doc 2 - separated
        "deep learning models"         // doc 3 - no "machine"
    };
    AddDocuments(index.get(), docs);

    // Exact phrase match (slop=0)
    auto [seg_offsets0, scores0] = index->BM25PhraseSearchQuery("machine learning", 0, 10);
    
    // Only doc 0 should match exact phrase
    bool has_doc0 = std::find(seg_offsets0.begin(), seg_offsets0.end(), 0) != seg_offsets0.end();
    EXPECT_TRUE(has_doc0) << "Should find exact phrase match";

    // With slop=1, might match more docs
    auto [seg_offsets1, scores1] = index->BM25PhraseSearchQuery("machine learning", 1, 10);
    EXPECT_GE(seg_offsets1.size(), seg_offsets0.size()) 
        << "Higher slop should find at least as many matches";
}

// Test BM25 relevance ranking
TEST_F(TextMatchIndexBM25Test, BM25RelevanceRanking) {
    auto index = CreateIndex();
    
    std::vector<std::string> docs = {
        "machine learning machine learning machine learning",  // doc 0 - high TF
        "machine learning",                                    // doc 1 - low TF
        "something else entirely"                              // doc 2 - no match
    };
    AddDocuments(index.get(), docs);

    auto [seg_offsets, scores] = index->BM25SearchQuery("machine learning", 10);

    ASSERT_GE(seg_offsets.size(), 2) << "Should find both matching docs";
    
    // Doc 0 should have higher score than doc 1 (more term occurrences)
    // Find positions in results
    auto it0 = std::find(seg_offsets.begin(), seg_offsets.end(), 0);
    auto it1 = std::find(seg_offsets.begin(), seg_offsets.end(), 1);
    
    if (it0 != seg_offsets.end() && it1 != seg_offsets.end()) {
        size_t pos0 = std::distance(seg_offsets.begin(), it0);
        size_t pos1 = std::distance(seg_offsets.begin(), it1);
        
        float score0 = scores[pos0];
        float score1 = scores[pos1];
        
        // Higher TF should give higher score (but BM25 has saturation)
        EXPECT_GE(score0, score1) 
            << "Doc with more occurrences should have >= score";
    }
}

}  // namespace milvus::index
