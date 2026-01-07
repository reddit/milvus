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
