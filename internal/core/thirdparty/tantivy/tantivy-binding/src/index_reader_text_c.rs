use std::ffi::CStr;

use libc::{c_char, c_void};

use crate::{
    analyzer::create_analyzer, array::{RustResult, RustScoredSearchResult}, cstr_to_str, index_reader::IndexReaderWrapper,
    log::init_log,
};

#[no_mangle]
pub extern "C" fn tantivy_match_query(
    ptr: *mut c_void,
    query: *const c_char,
    min_should_match: usize,
    bitset: *mut c_void,
) -> RustResult {
    let real = ptr as *mut IndexReaderWrapper;
    let query = cstr_to_str!(query);
    if min_should_match > 1 {
        unsafe {
            (*real)
                .match_query_with_minimum(query, min_should_match, bitset)
                .into()
        }
    } else {
        unsafe { (*real).match_query(query, bitset).into() }
    }
}

#[no_mangle]
pub extern "C" fn tantivy_phrase_match_query(
    ptr: *mut c_void,
    query: *const c_char,
    slop: u32,
    bitset: *mut c_void,
) -> RustResult {
    let real = ptr as *mut IndexReaderWrapper;
    let query = cstr_to_str!(query);
    unsafe { (*real).phrase_match_query(query, slop, bitset).into() }
}

#[no_mangle]
pub extern "C" fn tantivy_register_tokenizer(
    ptr: *mut c_void,
    tokenizer_name: *const c_char,
    analyzer_params: *const c_char,
) -> RustResult {
    init_log();
    let real = ptr as *mut IndexReaderWrapper;
    let tokenizer_name = cstr_to_str!(tokenizer_name);
    let params = cstr_to_str!(analyzer_params);
    let analyzer = create_analyzer(params);
    match analyzer {
        Ok(text_analyzer) => unsafe {
            (*real).register_tokenizer(String::from(tokenizer_name), text_analyzer);
            Ok(()).into()
        },
        Err(err) => RustResult::from_error(err.to_string()),
    }
}

/// Performs a BM25 scored text search and returns top-k results with scores.
/// Returns a RustScoredSearchResult containing doc_ids and scores arrays.
#[no_mangle]
pub extern "C" fn tantivy_bm25_search_query(
    ptr: *mut c_void,
    query: *const c_char,
    topk: usize,
    result: *mut RustScoredSearchResult,
) -> RustResult {
    let real = ptr as *mut IndexReaderWrapper;
    let query = cstr_to_str!(query);
    unsafe {
        match (*real).bm25_search_query(query, topk) {
            Ok(scored_result) => {
                *result = scored_result;
                Ok(()).into()
            }
            Err(e) => RustResult::from_error(e.to_string()),
        }
    }
}

/// Performs a BM25 scored text search with minimum_should_match parameter.
#[no_mangle]
pub extern "C" fn tantivy_bm25_search_query_with_minimum(
    ptr: *mut c_void,
    query: *const c_char,
    min_should_match: usize,
    topk: usize,
    result: *mut RustScoredSearchResult,
) -> RustResult {
    let real = ptr as *mut IndexReaderWrapper;
    let query = cstr_to_str!(query);
    unsafe {
        match (*real).bm25_search_query_with_minimum(query, min_should_match, topk) {
            Ok(scored_result) => {
                *result = scored_result;
                Ok(()).into()
            }
            Err(e) => RustResult::from_error(e.to_string()),
        }
    }
}

/// Performs a BM25 scored phrase search and returns top-k results with scores.
#[no_mangle]
pub extern "C" fn tantivy_bm25_phrase_search_query(
    ptr: *mut c_void,
    query: *const c_char,
    slop: u32,
    topk: usize,
    result: *mut RustScoredSearchResult,
) -> RustResult {
    let real = ptr as *mut IndexReaderWrapper;
    let query = cstr_to_str!(query);
    unsafe {
        match (*real).bm25_phrase_search_query(query, slop, topk) {
            Ok(scored_result) => {
                *result = scored_result;
                Ok(()).into()
            }
            Err(e) => RustResult::from_error(e.to_string()),
        }
    }
}

/// Performs a BM25 scored text search with a filter bitset.
/// The filter bitset contains 1 for documents to EXCLUDE.
/// This is more efficient than searching and then filtering post-hoc.
#[no_mangle]
pub extern "C" fn tantivy_bm25_search_query_with_filter(
    ptr: *mut c_void,
    query: *const c_char,
    topk: usize,
    filter_bitset: *const u8,
    filter_bitset_len: usize,
    result: *mut RustScoredSearchResult,
) -> RustResult {
    let real = ptr as *mut IndexReaderWrapper;
    let query = cstr_to_str!(query);
    
    // Convert filter bitset pointer to slice
    let filter_slice = if filter_bitset.is_null() || filter_bitset_len == 0 {
        &[] as &[u8]
    } else {
        let byte_len = (filter_bitset_len + 7) / 8;
        unsafe { std::slice::from_raw_parts(filter_bitset, byte_len) }
    };

    unsafe {
        match (*real).bm25_search_query_with_filter(query, topk, filter_slice, filter_bitset_len) {
            Ok(scored_result) => {
                *result = scored_result;
                Ok(()).into()
            }
            Err(e) => RustResult::from_error(e.to_string()),
        }
    }
}

/// BM25 aggregation type for multi-field search.
/// 0 = WeightedSum (default), 1 = Max
#[repr(C)]
pub enum BM25AggregationType {
    WeightedSum = 0,
    Max = 1,
}

impl From<BM25AggregationType> for crate::index_reader_text::BM25AggregationType {
    fn from(c_type: BM25AggregationType) -> Self {
        match c_type {
            BM25AggregationType::WeightedSum => crate::index_reader_text::BM25AggregationType::WeightedSum,
            BM25AggregationType::Max => crate::index_reader_text::BM25AggregationType::Max,
        }
    }
}

/// Performs multi-field BM25 search by querying multiple text indexes and aggregating results.
/// This enables native multi-field text search without requiring HybridSearch + Reranker.
///
/// # Arguments
/// * `readers` - Array of IndexReaderWrapper pointers for each field
/// * `num_readers` - Number of readers
/// * `query` - Search query text
/// * `topk` - Number of top results per field (before aggregation)
/// * `weights` - Weight for each field (array of floats, length must match num_readers)
/// * `aggregation` - How to combine scores (0 = WeightedSum, 1 = Max)
/// * `result` - Output result
#[no_mangle]
pub extern "C" fn tantivy_bm25_multi_field_search(
    readers: *const *mut c_void,
    num_readers: usize,
    query: *const c_char,
    topk: usize,
    weights: *const f32,
    aggregation: BM25AggregationType,
    result: *mut RustScoredSearchResult,
) -> RustResult {
    if readers.is_null() || num_readers == 0 || weights.is_null() {
        return RustResult::from_error("Invalid arguments: readers or weights is null".to_string());
    }

    let query = cstr_to_str!(query);

    // Convert weights to slice
    let weights_slice = unsafe { std::slice::from_raw_parts(weights, num_readers) };

    // Query each field and collect results
    let mut field_results: Vec<(Vec<u32>, Vec<f32>)> = Vec::with_capacity(num_readers);

    for i in 0..num_readers {
        let reader_ptr = unsafe { *readers.add(i) };
        if reader_ptr.is_null() {
            continue;
        }

        let reader = reader_ptr as *mut IndexReaderWrapper;
        match unsafe { (*reader).bm25_search_query(query, topk) } {
            Ok(scored_result) => {
                // Convert RustScoredSearchResult to Vec
                let doc_ids: Vec<u32> = unsafe {
                    std::slice::from_raw_parts(scored_result.doc_ids, scored_result.len).to_vec()
                };
                let scores: Vec<f32> = unsafe {
                    std::slice::from_raw_parts(scored_result.scores, scored_result.len).to_vec()
                };
                // Free the intermediate result
                crate::array::free_rust_scored_search_result(scored_result);
                field_results.push((doc_ids, scores));
            }
            Err(e) => {
                return RustResult::from_error(format!(
                    "Failed to query field {}: {}",
                    i,
                    e.to_string()
                ));
            }
        }
    }

    // Aggregate results
    let aggregated = crate::index_reader_text::aggregate_multi_field_bm25_results(
        field_results,
        weights_slice,
        aggregation.into(),
        topk,
    );

    unsafe {
        *result = aggregated;
    }

    Ok(()).into()
}

/// Performs multi-field BM25 search with a filter bitset.
#[no_mangle]
pub extern "C" fn tantivy_bm25_multi_field_search_with_filter(
    readers: *const *mut c_void,
    num_readers: usize,
    query: *const c_char,
    topk: usize,
    weights: *const f32,
    aggregation: BM25AggregationType,
    filter_bitset: *const u8,
    filter_bitset_len: usize,
    result: *mut RustScoredSearchResult,
) -> RustResult {
    if readers.is_null() || num_readers == 0 || weights.is_null() {
        return RustResult::from_error("Invalid arguments: readers or weights is null".to_string());
    }

    let query = cstr_to_str!(query);
    let weights_slice = unsafe { std::slice::from_raw_parts(weights, num_readers) };

    // Convert filter bitset
    let filter_slice = if filter_bitset.is_null() || filter_bitset_len == 0 {
        &[] as &[u8]
    } else {
        let byte_len = (filter_bitset_len + 7) / 8;
        unsafe { std::slice::from_raw_parts(filter_bitset, byte_len) }
    };

    // Query each field with filter and collect results
    let mut field_results: Vec<(Vec<u32>, Vec<f32>)> = Vec::with_capacity(num_readers);

    for i in 0..num_readers {
        let reader_ptr = unsafe { *readers.add(i) };
        if reader_ptr.is_null() {
            continue;
        }

        let reader = reader_ptr as *mut IndexReaderWrapper;
        let search_result = if filter_slice.is_empty() {
            unsafe { (*reader).bm25_search_query(query, topk) }
        } else {
            unsafe { (*reader).bm25_search_query_with_filter(query, topk, filter_slice, filter_bitset_len) }
        };

        match search_result {
            Ok(scored_result) => {
                let doc_ids: Vec<u32> = unsafe {
                    std::slice::from_raw_parts(scored_result.doc_ids, scored_result.len).to_vec()
                };
                let scores: Vec<f32> = unsafe {
                    std::slice::from_raw_parts(scored_result.scores, scored_result.len).to_vec()
                };
                crate::array::free_rust_scored_search_result(scored_result);
                field_results.push((doc_ids, scores));
            }
            Err(e) => {
                return RustResult::from_error(format!(
                    "Failed to query field {}: {}",
                    i,
                    e.to_string()
                ));
            }
        }
    }

    let aggregated = crate::index_reader_text::aggregate_multi_field_bm25_results(
        field_results,
        weights_slice,
        aggregation.into(),
        topk,
    );

    unsafe {
        *result = aggregated;
    }

    Ok(()).into()
}
