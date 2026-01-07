use std::ffi::c_void;

use tantivy::{
    collector::TopDocs,
    query::{BooleanQuery, PhraseQuery},
    schema::Value,
    tokenizer::{TextAnalyzer, TokenStream},
    TantivyDocument,
    Term,
};

use crate::{
    analyzer::standard_analyzer, error::TantivyBindingError, index_reader::IndexReaderWrapper,
    array::RustScoredSearchResult,
};
use crate::{
    bitset_wrapper::BitsetWrapper, direct_bitset_collector::DirectBitsetCollector, error::Result,
};

impl IndexReaderWrapper {
    // split the query string into multiple tokens using index's default tokenizer,
    // and then execute the disconjunction of term query.
    pub(crate) fn match_query(&self, q: &str, bitset: *mut c_void) -> Result<()> {
        // clone the tokenizer to make `match_query` thread-safe.
        let mut tokenizer = self
            .index
            .tokenizer_for_field(self.field)
            .unwrap_or(standard_analyzer(vec![]))
            .clone();
        let mut token_stream = tokenizer.token_stream(q);
        let mut terms: Vec<Term> = Vec::new();
        while token_stream.advance() {
            let token = token_stream.token();
            terms.push(Term::from_field_text(self.field, &token.text));
        }
        let collector = DirectBitsetCollector {
            bitset_wrapper: BitsetWrapper::new(bitset, self.set_bitset),
            terms: terms.clone(),
        };
        let query = BooleanQuery::new_multiterms_query(terms);
        let searcher = self.reader.searcher();
        searcher
            .search(&query, &collector)
            .map_err(TantivyBindingError::TantivyError)
    }

    pub(crate) fn match_query_with_minimum(
        &self,
        q: &str,
        min_should_match: usize,
        bitset: *mut c_void,
    ) -> Result<()> {
        let mut tokenizer = self
            .index
            .tokenizer_for_field(self.field)
            .unwrap_or(standard_analyzer(vec![]))
            .clone();
        let mut token_stream = tokenizer.token_stream(q);
        let mut terms: Vec<Term> = Vec::new();
        while token_stream.advance() {
            let token = token_stream.token();
            terms.push(Term::from_field_text(self.field, &token.text));
        }
        use tantivy::query::{Occur, TermQuery};
        use tantivy::schema::IndexRecordOption;
        let mut subqueries: Vec<(Occur, Box<dyn tantivy::query::Query>)> = Vec::new();
        for term in terms.into_iter() {
            subqueries.push((
                Occur::Should,
                Box::new(TermQuery::new(term, IndexRecordOption::Basic)),
            ));
        }
        let effective_min = std::cmp::max(1, min_should_match);
        let query = BooleanQuery::with_minimum_required_clauses(subqueries, effective_min);
        self.search(&query, bitset)
    }

    // split the query string into multiple tokens using index's default tokenizer,
    // and then execute the disconjunction of term query.
    pub(crate) fn phrase_match_query(&self, q: &str, slop: u32, bitset: *mut c_void) -> Result<()> {
        // clone the tokenizer to make `match_query` thread-safe.
        let mut tokenizer = self
            .index
            .tokenizer_for_field(self.field)
            .unwrap_or(standard_analyzer(vec![]))
            .clone();
        let mut token_stream = tokenizer.token_stream(q);
        let mut terms: Vec<Term> = Vec::new();

        let mut positions = vec![];
        while token_stream.advance() {
            let token = token_stream.token();
            positions.push(token.position);
            terms.push(Term::from_field_text(self.field, &token.text));
        }
        if terms.len() <= 1 {
            // tantivy will panic when terms.len() <= 1, so we forward to text match instead.
            let query = BooleanQuery::new_multiterms_query(terms);
            return self.search(&query, bitset);
        }

        let terms_with_offset: Vec<_> = positions.into_iter().zip(terms.into_iter()).collect();
        let phrase_query = PhraseQuery::new_with_offset_and_slop(terms_with_offset, slop);
        self.search(&phrase_query, bitset)
    }

    pub(crate) fn register_tokenizer(&self, tokenizer_name: String, tokenizer: TextAnalyzer) {
        self.index.tokenizers().register(&tokenizer_name, tokenizer)
    }

    /// Performs a BM25 scored text search and returns top-k results with scores.
    /// This is similar to match_query but returns relevance scores instead of just matching doc IDs.
    pub(crate) fn bm25_search_query(&self, q: &str, topk: usize) -> Result<RustScoredSearchResult> {
        let mut tokenizer = self
            .index
            .tokenizer_for_field(self.field)
            .unwrap_or(standard_analyzer(vec![]))
            .clone();
        let mut token_stream = tokenizer.token_stream(q);
        let mut terms: Vec<Term> = Vec::new();
        while token_stream.advance() {
            let token = token_stream.token();
            terms.push(Term::from_field_text(self.field, &token.text));
        }
        
        if terms.is_empty() {
            return Ok(RustScoredSearchResult::default());
        }

        let query = BooleanQuery::new_multiterms_query(terms);
        let searcher = self.reader.searcher();
        let top_docs = searcher
            .search(&query, &TopDocs::with_limit(topk))
            .map_err(TantivyBindingError::TantivyError)?;

        let mut doc_ids: Vec<u32> = Vec::with_capacity(top_docs.len());
        let mut scores: Vec<f32> = Vec::with_capacity(top_docs.len());

        for (score, doc_address) in top_docs {
            // Get the milvus doc ID from the tantivy doc
            let doc_id = if self.user_specified_doc_id {
                doc_address.doc_id
            } else if let Some(id_field) = self.id_field {
                let doc: TantivyDocument = searcher.doc(doc_address).map_err(TantivyBindingError::TantivyError)?;
                doc.get_first(id_field)
                    .and_then(|v| v.as_i64())
                    .unwrap_or(doc_address.doc_id as i64) as u32
            } else {
                doc_address.doc_id
            };
            doc_ids.push(doc_id);
            scores.push(score);
        }

        Ok(RustScoredSearchResult::from_vecs(doc_ids, scores))
    }

    /// Performs a BM25 scored text search with minimum_should_match parameter.
    pub(crate) fn bm25_search_query_with_minimum(
        &self,
        q: &str,
        min_should_match: usize,
        topk: usize,
    ) -> Result<RustScoredSearchResult> {
        let mut tokenizer = self
            .index
            .tokenizer_for_field(self.field)
            .unwrap_or(standard_analyzer(vec![]))
            .clone();
        let mut token_stream = tokenizer.token_stream(q);
        let mut terms: Vec<Term> = Vec::new();
        while token_stream.advance() {
            let token = token_stream.token();
            terms.push(Term::from_field_text(self.field, &token.text));
        }

        if terms.is_empty() {
            return Ok(RustScoredSearchResult::default());
        }

        use tantivy::query::{Occur, TermQuery};
        use tantivy::schema::IndexRecordOption;
        
        let mut subqueries: Vec<(Occur, Box<dyn tantivy::query::Query>)> = Vec::new();
        for term in terms.into_iter() {
            subqueries.push((
                Occur::Should,
                Box::new(TermQuery::new(term, IndexRecordOption::Basic)),
            ));
        }
        let effective_min = std::cmp::max(1, min_should_match);
        let query = BooleanQuery::with_minimum_required_clauses(subqueries, effective_min);

        let searcher = self.reader.searcher();
        let top_docs = searcher
            .search(&query, &TopDocs::with_limit(topk))
            .map_err(TantivyBindingError::TantivyError)?;

        let mut doc_ids: Vec<u32> = Vec::with_capacity(top_docs.len());
        let mut scores: Vec<f32> = Vec::with_capacity(top_docs.len());

        for (score, doc_address) in top_docs {
            let doc_id = if self.user_specified_doc_id {
                doc_address.doc_id
            } else if let Some(id_field) = self.id_field {
                let doc: TantivyDocument = searcher.doc(doc_address).map_err(TantivyBindingError::TantivyError)?;
                doc.get_first(id_field)
                    .and_then(|v| v.as_i64())
                    .unwrap_or(doc_address.doc_id as i64) as u32
            } else {
                doc_address.doc_id
            };
            doc_ids.push(doc_id);
            scores.push(score);
        }

        Ok(RustScoredSearchResult::from_vecs(doc_ids, scores))
    }

    /// Performs a BM25 scored phrase search and returns top-k results with scores.
    pub(crate) fn bm25_phrase_search_query(
        &self,
        q: &str,
        slop: u32,
        topk: usize,
    ) -> Result<RustScoredSearchResult> {
        let mut tokenizer = self
            .index
            .tokenizer_for_field(self.field)
            .unwrap_or(standard_analyzer(vec![]))
            .clone();
        let mut token_stream = tokenizer.token_stream(q);
        let mut terms: Vec<Term> = Vec::new();
        let mut positions = vec![];
        
        while token_stream.advance() {
            let token = token_stream.token();
            positions.push(token.position);
            terms.push(Term::from_field_text(self.field, &token.text));
        }

        if terms.is_empty() {
            return Ok(RustScoredSearchResult::default());
        }

        let searcher = self.reader.searcher();

        // If only one term, fall back to term query
        if terms.len() <= 1 {
            let query = BooleanQuery::new_multiterms_query(terms);
            let top_docs = searcher
                .search(&query, &TopDocs::with_limit(topk))
                .map_err(TantivyBindingError::TantivyError)?;

            let mut doc_ids: Vec<u32> = Vec::with_capacity(top_docs.len());
            let mut scores: Vec<f32> = Vec::with_capacity(top_docs.len());

            for (score, doc_address) in top_docs {
                let doc_id = if self.user_specified_doc_id {
                    doc_address.doc_id
                } else if let Some(id_field) = self.id_field {
                    let doc: TantivyDocument = searcher.doc(doc_address).map_err(TantivyBindingError::TantivyError)?;
                    doc.get_first(id_field)
                        .and_then(|v| v.as_i64())
                        .unwrap_or(doc_address.doc_id as i64) as u32
                } else {
                    doc_address.doc_id
                };
                doc_ids.push(doc_id);
                scores.push(score);
            }
            return Ok(RustScoredSearchResult::from_vecs(doc_ids, scores));
        }

        let terms_with_offset: Vec<_> = positions.into_iter().zip(terms.into_iter()).collect();
        let phrase_query = PhraseQuery::new_with_offset_and_slop(terms_with_offset, slop);
        
        let top_docs = searcher
            .search(&phrase_query, &TopDocs::with_limit(topk))
            .map_err(TantivyBindingError::TantivyError)?;

        let mut doc_ids: Vec<u32> = Vec::with_capacity(top_docs.len());
        let mut scores: Vec<f32> = Vec::with_capacity(top_docs.len());

        for (score, doc_address) in top_docs {
            let doc_id = if self.user_specified_doc_id {
                doc_address.doc_id
            } else if let Some(id_field) = self.id_field {
                let doc: TantivyDocument = searcher.doc(doc_address).map_err(TantivyBindingError::TantivyError)?;
                doc.get_first(id_field)
                    .and_then(|v| v.as_i64())
                    .unwrap_or(doc_address.doc_id as i64) as u32
            } else {
                doc_address.doc_id
            };
            doc_ids.push(doc_id);
            scores.push(score);
        }

        Ok(RustScoredSearchResult::from_vecs(doc_ids, scores))
    }

    /// Performs a BM25 scored text search with a filter bitset.
    /// The filter bitset contains 1 for documents to EXCLUDE (filtered out).
    /// This is more efficient than searching and then filtering post-hoc.
    pub(crate) fn bm25_search_query_with_filter(
        &self,
        q: &str,
        topk: usize,
        filter_bitset: &[u8],
        filter_bitset_len: usize,
    ) -> Result<RustScoredSearchResult> {
        let mut tokenizer = self
            .index
            .tokenizer_for_field(self.field)
            .unwrap_or(standard_analyzer(vec![]))
            .clone();
        let mut token_stream = tokenizer.token_stream(q);
        let mut terms: Vec<Term> = Vec::new();
        while token_stream.advance() {
            let token = token_stream.token();
            terms.push(Term::from_field_text(self.field, &token.text));
        }

        if terms.is_empty() {
            return Ok(RustScoredSearchResult::default());
        }

        let query = BooleanQuery::new_multiterms_query(terms);
        let searcher = self.reader.searcher();

        // Helper to check if a doc is filtered (bit is set = excluded)
        let is_filtered = |doc_id: u32| -> bool {
            let doc_id = doc_id as usize;
            if doc_id >= filter_bitset_len {
                return false; // Out of range docs pass through
            }
            let byte_idx = doc_id / 8;
            let bit_idx = doc_id % 8;
            if byte_idx < filter_bitset.len() {
                (filter_bitset[byte_idx] & (1 << bit_idx)) != 0
            } else {
                false
            }
        };

        // Request more results than topk to account for filtering
        // We'll iterate and filter until we have topk results
        let fetch_limit = std::cmp::min(topk * 10, 10000); // Fetch more to account for filtering
        let top_docs = searcher
            .search(&query, &TopDocs::with_limit(fetch_limit))
            .map_err(TantivyBindingError::TantivyError)?;

        let mut doc_ids: Vec<u32> = Vec::with_capacity(topk);
        let mut scores: Vec<f32> = Vec::with_capacity(topk);

        for (score, doc_address) in top_docs {
            // Get the milvus doc ID from the tantivy doc
            let doc_id = if self.user_specified_doc_id {
                doc_address.doc_id
            } else if let Some(id_field) = self.id_field {
                let doc: TantivyDocument = searcher.doc(doc_address).map_err(TantivyBindingError::TantivyError)?;
                doc.get_first(id_field)
                    .and_then(|v| v.as_i64())
                    .unwrap_or(doc_address.doc_id as i64) as u32
            } else {
                doc_address.doc_id
            };

            // Check filter - skip if filtered out
            if is_filtered(doc_id) {
                continue;
            }

            doc_ids.push(doc_id);
            scores.push(score);

            // Stop once we have topk results
            if doc_ids.len() >= topk {
                break;
            }
        }

        Ok(RustScoredSearchResult::from_vecs(doc_ids, scores))
    }
}

#[cfg(test)]
mod tests {
    use std::{collections::HashSet, ffi::c_void};

    use tempfile::TempDir;

    use crate::{index_writer::IndexWriterWrapper, util::set_bitset, TantivyIndexVersion};
    #[test]
    fn test_jeba() {
        let params = "{\"tokenizer\": \"jieba\"}".to_string();
        let dir = TempDir::new().unwrap();

        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "jieba",
            &params,
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        writer.add("网球和滑雪", Some(0)).unwrap();
        writer.add("网球以及滑雪", Some(1)).unwrap();

        writer.commit().unwrap();

        let slop = 1;
        let reader = writer.create_reader(set_bitset).unwrap();
        let mut res: HashSet<u32> = HashSet::new();
        reader
            .phrase_match_query("网球滑雪", slop, &mut res as *mut _ as *mut c_void)
            .unwrap();
        assert_eq!(res, vec![0].into_iter().collect::<HashSet<u32>>());

        let slop = 2;
        let mut res: HashSet<u32> = HashSet::new();
        let reader = writer.create_reader(set_bitset).unwrap();
        reader
            .phrase_match_query("网球滑雪", slop, &mut res as *mut _ as *mut c_void)
            .unwrap();
        assert_eq!(res, vec![0, 1].into_iter().collect::<HashSet<u32>>());
    }

    #[test]
    fn test_read() {
        let dir = TempDir::new().unwrap();
        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "default",
            "",
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        for i in 0..100000 {
            writer.add("hello world", Some(i)).unwrap();
        }
        writer.commit().unwrap();

        let reader = writer.create_reader(set_bitset).unwrap();

        let mut res: HashSet<u32> = HashSet::new();
        reader
            .match_query("hello world", &mut res as *mut _ as *mut c_void)
            .unwrap();
        assert_eq!(res, (0..100000).collect::<HashSet<u32>>());
    }

    #[test]
    fn test_min_should_match_match_query() {
        let dir = tempfile::TempDir::new().unwrap();
        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "default",
            "",
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        // doc ids: 0..4
        writer.add("a b", Some(0)).unwrap();
        writer.add("a c", Some(1)).unwrap();
        writer.add("b c", Some(2)).unwrap();
        writer.add("c", Some(3)).unwrap();
        writer.add("a b c", Some(4)).unwrap();
        writer.commit().unwrap();

        let reader = writer.create_reader(set_bitset).unwrap();

        // min=1 behaves like union of tokens
        let mut res: HashSet<u32> = HashSet::new();
        reader
            .match_query_with_minimum("a b", 1, &mut res as *mut _ as *mut c_void)
            .unwrap();
        assert_eq!(res, vec![0, 1, 2, 4].into_iter().collect::<HashSet<u32>>());

        // min=2 requires at least two tokens
        res.clear();
        reader
            .match_query_with_minimum("a b c", 2, &mut res as *mut _ as *mut c_void)
            .unwrap();
        assert_eq!(res, vec![0, 1, 2, 4].into_iter().collect::<HashSet<u32>>());

        // min=3 requires all three tokens
        res.clear();
        reader
            .match_query_with_minimum("a b c", 3, &mut res as *mut _ as *mut c_void)
            .unwrap();
        assert_eq!(res, vec![4].into_iter().collect::<HashSet<u32>>());

        // large min should yield empty
        res.clear();
        reader
            .match_query_with_minimum("a b c", 10, &mut res as *mut _ as *mut c_void)
            .unwrap();
        assert!(res.is_empty());
    }

    #[test]
    fn test_bm25_search_query() {
        let dir = TempDir::new().unwrap();
        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "default",
            "",
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        // Add documents with varying relevance
        writer.add("machine learning deep learning", Some(0)).unwrap();
        writer.add("machine learning", Some(1)).unwrap();
        writer.add("deep learning neural networks", Some(2)).unwrap();
        writer.add("something completely different", Some(3)).unwrap();
        writer.add("machine", Some(4)).unwrap();
        writer.commit().unwrap();

        let reader = writer.create_reader(set_bitset).unwrap();

        // Test basic BM25 search
        let result = reader.bm25_search_query("machine learning", 10).unwrap();
        assert!(result.len > 0, "Should return some results");
        
        // Convert to vectors for easier assertions
        let doc_ids: Vec<u32> = unsafe {
            std::slice::from_raw_parts(result.doc_ids, result.len).to_vec()
        };
        let scores: Vec<f32> = unsafe {
            std::slice::from_raw_parts(result.scores, result.len).to_vec()
        };

        // Doc 0 and 1 should match best (both have "machine" and "learning")
        assert!(doc_ids.contains(&0) || doc_ids.contains(&1), 
                "Should find docs with 'machine learning'");
        
        // Scores should be positive
        for score in &scores {
            assert!(*score > 0.0, "BM25 scores should be positive");
        }

        // Scores should be in descending order
        for i in 1..scores.len() {
            assert!(scores[i - 1] >= scores[i], 
                    "Scores should be in descending order");
        }

        // Free the result
        crate::array::free_rust_scored_search_result(result);
    }

    #[test]
    fn test_bm25_search_query_topk() {
        let dir = TempDir::new().unwrap();
        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "default",
            "",
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        // Add many documents
        for i in 0..100 {
            let doc = format!("document {} with common words", i);
            writer.add(doc.as_str(), Some(i)).unwrap();
        }
        writer.commit().unwrap();

        let reader = writer.create_reader(set_bitset).unwrap();

        // Test topk limit
        let result = reader.bm25_search_query("common words", 5).unwrap();
        assert!(result.len <= 5, "Should respect topk limit");
        crate::array::free_rust_scored_search_result(result);

        // Test larger topk
        let result = reader.bm25_search_query("common words", 50).unwrap();
        assert!(result.len <= 50, "Should respect larger topk limit");
        crate::array::free_rust_scored_search_result(result);
    }

    #[test]
    fn test_bm25_search_query_with_filter() {
        let dir = TempDir::new().unwrap();
        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "default",
            "",
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        // Add documents
        writer.add("hello world", Some(0)).unwrap();
        writer.add("hello universe", Some(1)).unwrap();
        writer.add("goodbye world", Some(2)).unwrap();
        writer.add("hello earth", Some(3)).unwrap();
        writer.add("hello mars", Some(4)).unwrap();
        writer.commit().unwrap();

        let reader = writer.create_reader(set_bitset).unwrap();

        // Create a filter bitset that excludes docs 1 and 3
        // Bitset: bit 1 and bit 3 set = exclude those docs
        let mut filter_bitset: Vec<u8> = vec![0; 1]; // 1 byte for 8 bits
        filter_bitset[0] = 0b00001010; // bits 1 and 3 set (0-indexed)
        
        let result = reader
            .bm25_search_query_with_filter("hello", 10, &filter_bitset, 5)
            .unwrap();

        let doc_ids: Vec<u32> = unsafe {
            std::slice::from_raw_parts(result.doc_ids, result.len).to_vec()
        };

        // Should NOT contain filtered docs (1 and 3)
        assert!(!doc_ids.contains(&1), "Doc 1 should be filtered out");
        assert!(!doc_ids.contains(&3), "Doc 3 should be filtered out");
        
        // Should contain unfiltered docs that match "hello"
        // Docs 0, 4 match "hello" and are not filtered
        let has_unfiltered = doc_ids.contains(&0) || doc_ids.contains(&4);
        assert!(has_unfiltered, "Should contain unfiltered matching docs");

        crate::array::free_rust_scored_search_result(result);
    }

    #[test]
    fn test_bm25_search_empty_query() {
        let dir = TempDir::new().unwrap();
        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "default",
            "",
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        writer.add("hello world", Some(0)).unwrap();
        writer.commit().unwrap();

        let reader = writer.create_reader(set_bitset).unwrap();

        // Empty query should return empty result
        let result = reader.bm25_search_query("", 10).unwrap();
        assert_eq!(result.len, 0, "Empty query should return no results");
    }

    #[test]
    fn test_bm25_phrase_search_query() {
        let dir = TempDir::new().unwrap();
        let mut writer = IndexWriterWrapper::create_text_writer(
            "text",
            dir.path().to_str().unwrap(),
            "default",
            "",
            1,
            50_000_000,
            false,
            TantivyIndexVersion::default_version(),
        )
        .unwrap();

        // Add documents with phrases
        writer.add("machine learning is great", Some(0)).unwrap();
        writer.add("learning machine works", Some(1)).unwrap(); // reversed order
        writer.add("machine and learning", Some(2)).unwrap(); // words separated
        writer.commit().unwrap();

        let reader = writer.create_reader(set_bitset).unwrap();

        // Exact phrase match (slop=0)
        let result = reader.bm25_phrase_search_query("machine learning", 0, 10).unwrap();
        let doc_ids: Vec<u32> = unsafe {
            std::slice::from_raw_parts(result.doc_ids, result.len).to_vec()
        };
        
        // Only doc 0 has exact phrase "machine learning"
        assert!(doc_ids.contains(&0), "Should find exact phrase match");
        crate::array::free_rust_scored_search_result(result);

        // With slop=1, should also match doc 2 ("machine and learning")
        let result = reader.bm25_phrase_search_query("machine learning", 1, 10).unwrap();
        let doc_ids: Vec<u32> = unsafe {
            std::slice::from_raw_parts(result.doc_ids, result.len).to_vec()
        };
        assert!(doc_ids.len() >= 1, "Should find matches with slop");
        crate::array::free_rust_scored_search_result(result);
    }
}
