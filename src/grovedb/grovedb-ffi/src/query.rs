//! Query operations for the GroveDB FFI.
//!
//! This module provides functions for building and executing queries
//! in GroveDB using PathQuery.

use grovedb::query_result_type::{QueryResultElement as GdbQueryResultElement, QueryResultType};
use grovedb::{PathQuery, Query, QueryItem, SizedQuery};
use grovedb_version::version::GroveVersion;
use libc::size_t;

use crate::element::GroveDbElement;
use crate::error::{set_error, set_ok, GroveDbError, GroveDbErrorCode};
use crate::handles::{tx_arg_from_guard, GroveDbHandle, PathQueryHandle, TransactionHandle};
use crate::panic::{catch_panic, catch_panic_ptr};
use crate::types::{ByteBuffer, GroveDbPath, GroveDbPathOwned, OwnedByteBuffer};

/// Query results returned from query operations.
///
/// Free with `grovedb_free_query_results()`.
#[repr(C)]
pub struct QueryResults {
    /// Array of result elements
    pub elements: *mut QueryResultElement,
    /// Number of elements in the array
    pub count: size_t,
    /// Capacity of the elements array (for proper deallocation)
    pub capacity: size_t,
    /// Number of elements skipped (due to offset)
    pub skipped: u16,
}

impl QueryResults {
    /// Create empty results
    pub fn empty() -> Self {
        Self {
            elements: std::ptr::null_mut(),
            count: 0,
            capacity: 0,
            skipped: 0,
        }
    }

    /// Create from GroveDB QueryResultElements
    pub fn from_query_result_elements(
        query_results: grovedb::query_result_type::QueryResultElements,
        skipped: u16,
    ) -> Self {
        if query_results.is_empty() {
            return Self {
                elements: std::ptr::null_mut(),
                count: 0,
                capacity: 0,
                skipped,
            };
        }

        let results: Vec<QueryResultElement> = query_results
            .elements
            .into_iter()
            .map(|result_item| match result_item {
                GdbQueryResultElement::ElementResultItem(elem) => QueryResultElement {
                    path: GroveDbPathOwned::null(),
                    key: OwnedByteBuffer::null(),
                    element: Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem))),
                    has_element: true,
                },
                GdbQueryResultElement::KeyElementPairResultItem((key, elem)) => QueryResultElement {
                    path: GroveDbPathOwned::null(),
                    key: OwnedByteBuffer::from_vec(key),
                    element: Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem))),
                    has_element: true,
                },
                GdbQueryResultElement::PathKeyElementTrioResultItem((path, key, elem)) => {
                    QueryResultElement {
                        path: GroveDbPathOwned::from_vec(path),
                        key: OwnedByteBuffer::from_vec(key),
                        element: Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem))),
                        has_element: true,
                    }
                }
            })
            .collect();

        let count = results.len();
        let mut results = std::mem::ManuallyDrop::new(results);
        Self {
            elements: results.as_mut_ptr(),
            count,
            capacity: results.capacity(),
            skipped,
        }
    }

    /// Create from a vector of (path, key, optional element) tuples (for proof verification)
    pub fn from_path_key_optional_elements(
        items: Vec<(Vec<Vec<u8>>, Vec<u8>, Option<grovedb::Element>)>,
    ) -> Self {
        if items.is_empty() {
            return Self::empty();
        }

        let results: Vec<QueryResultElement> = items
            .into_iter()
            .map(|(path, key, elem)| {
                let has_element = elem.is_some();
                QueryResultElement {
                    path: GroveDbPathOwned::from_vec(path),
                    key: OwnedByteBuffer::from_vec(key),
                    element: elem
                        .map(|e| Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&e))))
                        .unwrap_or(std::ptr::null_mut()),
                    has_element,
                }
            })
            .collect();

        let count = results.len();
        let mut results = std::mem::ManuallyDrop::new(results);
        Self {
            elements: results.as_mut_ptr(),
            count,
            capacity: results.capacity(),
            skipped: 0,
        }
    }

    /// Free the query results
    ///
    /// # Safety
    /// Must only be called once.
    pub unsafe fn free(&mut self) {
        if !self.elements.is_null() {
            let results = Vec::from_raw_parts(self.elements, self.count, self.capacity);
            for mut result in results {
                result.free();
            }
            self.elements = std::ptr::null_mut();
            self.count = 0;
            self.capacity = 0;
        }
    }
}

/// A single query result element.
#[repr(C)]
pub struct QueryResultElement {
    /// The path to the element (may be null for element-only results)
    pub path: GroveDbPathOwned,
    /// The key of the element (may be null for element-only results)
    pub key: OwnedByteBuffer,
    /// The element (may be null if not present)
    pub element: *mut GroveDbElement,
    /// Whether the element is present
    pub has_element: bool,
}

impl QueryResultElement {
    /// Free this result element
    ///
    /// # Safety
    /// Must only be called once.
    pub unsafe fn free(&mut self) {
        self.path.free();
        self.key.free();
        if !self.element.is_null() {
            let mut elem = Box::from_raw(self.element);
            elem.free();
            self.element = std::ptr::null_mut();
        }
    }
}

/// Internal wrapper for building PathQuery
pub(crate) struct PathQueryBuilder {
    path: Vec<Vec<u8>>,
    query_items: Vec<QueryItem>,
    limit: Option<u16>,
    offset: Option<u16>,
}

impl PathQueryBuilder {
    pub(crate) fn new(path: Vec<Vec<u8>>) -> Self {
        Self {
            path,
            query_items: Vec::new(),
            limit: None,
            offset: None,
        }
    }

    fn add_key(&mut self, key: Vec<u8>) {
        self.query_items.push(QueryItem::Key(key));
    }

    fn add_range(&mut self, start: Vec<u8>, end: Vec<u8>) {
        self.query_items.push(QueryItem::Range(start..end));
    }

    fn add_range_inclusive(&mut self, start: Vec<u8>, end: Vec<u8>) {
        self.query_items.push(QueryItem::RangeInclusive(start..=end));
    }

    fn add_range_full(&mut self) {
        self.query_items.push(QueryItem::RangeFull(..));
    }

    fn add_range_from(&mut self, start: Vec<u8>) {
        self.query_items.push(QueryItem::RangeFrom(start..));
    }

    #[allow(dead_code)]
    fn add_range_to(&mut self, end: Vec<u8>) {
        self.query_items.push(QueryItem::RangeTo(..end));
    }

    #[allow(dead_code)]
    fn add_range_to_inclusive(&mut self, end: Vec<u8>) {
        self.query_items.push(QueryItem::RangeToInclusive(..=end));
    }

    fn add_range_after(&mut self, start: Vec<u8>) {
        self.query_items.push(QueryItem::RangeAfter(start..));
    }

    #[allow(dead_code)]
    fn add_range_after_to(&mut self, start: Vec<u8>, end: Vec<u8>) {
        self.query_items.push(QueryItem::RangeAfterTo(start..end));
    }

    #[allow(dead_code)]
    fn add_range_after_to_inclusive(&mut self, start: Vec<u8>, end: Vec<u8>) {
        self.query_items.push(QueryItem::RangeAfterToInclusive(start..=end));
    }

    fn set_limit(&mut self, limit: u16) {
        self.limit = Some(limit);
    }

    fn set_offset(&mut self, offset: u16) {
        self.offset = Some(offset);
    }

    pub(crate) fn build(self) -> PathQuery {
        let mut query = Query::new_with_direction(true);
        query.items = self.query_items;
        let sized_query = SizedQuery::new(query, self.limit, self.offset);
        PathQuery {
            path: self.path,
            query: sized_query,
        }
    }
}

// ============================================================================
// FFI Functions
// ============================================================================

/// Create a new PathQuery builder for the given path.
///
/// # Arguments
///
/// * `path` - The path to query (may be null for root)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// A PathQuery handle, or null on error.
/// Use `grovedb_path_query_*` functions to configure, then execute or free.
///
/// # Safety
///
/// * `path` segments must remain valid during the call
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_new(
    path: *const GroveDbPath,
    error_out: *mut GroveDbError,
) -> *mut PathQueryHandle {
    catch_panic_ptr(error_out, || {
        let path_vec = if path.is_null() {
            Vec::new()
        } else {
            match (*path).to_vec_validated() {
                Ok(v) => v,
                Err(e) => {
                    set_error(error_out, e);
                    return std::ptr::null_mut();
                }
            }
        };

        let builder = Box::new(PathQueryBuilder::new(path_vec));
        set_ok(error_out);

        // Store as opaque handle
        Box::into_raw(builder) as *mut PathQueryHandle
    })
}

/// Add a single key to the query.
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `key` - The key to query
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * `query` must be a valid PathQuery handle
/// * `key` data must be valid during the call
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_add_key(
    query: *mut PathQueryHandle,
    key: ByteBuffer,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return false;
        }

        let key_vec = key.to_vec();
        let builder = &mut *(query as *mut PathQueryBuilder);
        builder.add_key(key_vec);

        set_ok(error_out);
        true
    })
}

/// Add a range query [start, end).
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `start` - Range start (inclusive)
/// * `end` - Range end (exclusive)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_add_range(
    query: *mut PathQueryHandle,
    start: ByteBuffer,
    end: ByteBuffer,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return false;
        }

        let builder = &mut *(query as *mut PathQueryBuilder);
        builder.add_range(start.to_vec(), end.to_vec());

        set_ok(error_out);
        true
    })
}

/// Add a range query [start, end].
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `start` - Range start (inclusive)
/// * `end` - Range end (inclusive)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_add_range_inclusive(
    query: *mut PathQueryHandle,
    start: ByteBuffer,
    end: ByteBuffer,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return false;
        }

        let builder = &mut *(query as *mut PathQueryBuilder);
        builder.add_range_inclusive(start.to_vec(), end.to_vec());

        set_ok(error_out);
        true
    })
}

/// Add a full range query (all keys).
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_add_range_full(
    query: *mut PathQueryHandle,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return false;
        }

        let builder = &mut *(query as *mut PathQueryBuilder);
        builder.add_range_full();

        set_ok(error_out);
        true
    })
}

/// Add a range from query [start, ..].
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `start` - Range start (inclusive)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_add_range_from(
    query: *mut PathQueryHandle,
    start: ByteBuffer,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return false;
        }

        let builder = &mut *(query as *mut PathQueryBuilder);
        builder.add_range_from(start.to_vec());

        set_ok(error_out);
        true
    })
}

/// Add a range after query (start, ..].
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `start` - Range start (exclusive)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_add_range_after(
    query: *mut PathQueryHandle,
    start: ByteBuffer,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return false;
        }

        let builder = &mut *(query as *mut PathQueryBuilder);
        builder.add_range_after(start.to_vec());

        set_ok(error_out);
        true
    })
}

/// Set the result limit for the query.
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `limit` - Maximum number of results to return
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_set_limit(query: *mut PathQueryHandle, limit: u16) {
    if query.is_null() {
        return;
    }
    let builder = &mut *(query as *mut PathQueryBuilder);
    builder.set_limit(limit);
}

/// Set the result offset for the query.
///
/// # Arguments
///
/// * `query` - PathQuery handle
/// * `offset` - Number of results to skip
#[no_mangle]
pub unsafe extern "C" fn grovedb_path_query_set_offset(query: *mut PathQueryHandle, offset: u16) {
    if query.is_null() {
        return;
    }
    let builder = &mut *(query as *mut PathQueryBuilder);
    builder.set_offset(offset);
}

/// Execute a query on the database.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `query` - PathQuery handle (consumed - do not use after this call)
/// * `transaction` - Optional transaction handle (may be null)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// Query results, or null on error. Caller must free with `grovedb_free_query_results()`.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `query` is consumed by this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_query(
    handle: *const GroveDbHandle,
    query: *mut PathQueryHandle,
    transaction: *const TransactionHandle,
    error_out: *mut GroveDbError,
) -> *mut QueryResults {
    catch_panic_ptr(error_out, || {
        // Validate handle
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return std::ptr::null_mut();
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return std::ptr::null_mut();
        }

        // Validate query
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return std::ptr::null_mut();
        }

        // Take ownership and build the PathQuery
        let builder = Box::from_raw(query as *mut PathQueryBuilder);
        let path_query = builder.build();

        // Get transaction guard
        let tx_guard = if transaction.is_null() {
            None
        } else {
            let tx_id = TransactionHandle::get_id(transaction).unwrap_or(0);
            match wrapper.get_transaction_guard(tx_id) {
                Some(guard) => Some(guard),
                None => {
                    set_error(
                        error_out,
                        GroveDbError::new(GroveDbErrorCode::InvalidParameter, "Transaction not found"),
                    );
                    return std::ptr::null_mut();
                }
            }
        };

        let version = GroveVersion::latest();

        // Execute query (extract result from CostContext)
        let grovedb_costs::CostContext { value: result, .. } = wrapper
            .db()
            .query(
                &path_query,
                true,  // allow_cache
                true,  // decrease_limit_on_range_with_no_sub_elements
                true,  // error_if_intermediate_path_tree_not_present
                QueryResultType::QueryPathKeyElementTrioResultType,
                tx_arg_from_guard(&tx_guard),
                version,
            );
        match result {
            Ok((result_elements, skipped)) => {
                set_ok(error_out);
                let query_results = QueryResults::from_query_result_elements(result_elements, skipped);
                Box::into_raw(Box::new(query_results))
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Create a simple single-key query and execute it.
///
/// This is a convenience function that combines path_query_new, add_key, and query.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `path` - The path to query
/// * `key` - The key to query
/// * `transaction` - Optional transaction handle (may be null)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// Query results, or null on error. Caller must free with `grovedb_free_query_results()`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_query_key(
    handle: *const GroveDbHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    transaction: *const TransactionHandle,
    error_out: *mut GroveDbError,
) -> *mut QueryResults {
    catch_panic_ptr(error_out, || {
        // Create query
        let path_vec = if path.is_null() {
            Vec::new()
        } else {
            match (*path).to_vec_validated() {
                Ok(v) => v,
                Err(e) => {
                    set_error(error_out, e);
                    return std::ptr::null_mut();
                }
            }
        };

        let path_query = PathQuery::new_single_key(path_vec, key.to_vec());

        // Get wrapper
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return std::ptr::null_mut();
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return std::ptr::null_mut();
        }

        // Get transaction guard
        let tx_guard = if transaction.is_null() {
            None
        } else {
            let tx_id = TransactionHandle::get_id(transaction).unwrap_or(0);
            match wrapper.get_transaction_guard(tx_id) {
                Some(guard) => Some(guard),
                None => {
                    set_error(
                        error_out,
                        GroveDbError::new(GroveDbErrorCode::InvalidParameter, "Transaction not found"),
                    );
                    return std::ptr::null_mut();
                }
            }
        };

        let version = GroveVersion::latest();

        // Execute query (extract result from CostContext)
        let grovedb_costs::CostContext { value: result, .. } = wrapper
            .db()
            .query(
                &path_query,
                true,
                true,
                true,
                QueryResultType::QueryPathKeyElementTrioResultType,
                tx_arg_from_guard(&tx_guard),
                version,
            );
        match result {
            Ok((result_elements, skipped)) => {
                set_ok(error_out);
                let query_results = QueryResults::from_query_result_elements(result_elements, skipped);
                Box::into_raw(Box::new(query_results))
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Free a PathQuery handle without executing it.
///
/// # Arguments
///
/// * `query` - PathQuery handle to free (may be null)
///
/// # Safety
///
/// * `query` must have been returned by `grovedb_path_query_new()`
/// * `query` must not be used after this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_path_query(query: *mut PathQueryHandle) {
    if !query.is_null() {
        drop(Box::from_raw(query as *mut PathQueryBuilder));
    }
}

/// Free query results.
///
/// # Arguments
///
/// * `results` - Query results to free (may be null)
///
/// # Safety
///
/// * `results` must have been returned by a query function
/// * `results` must not be used after this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_query_results(results: *mut QueryResults) {
    if !results.is_null() {
        let mut r = Box::from_raw(results);
        r.free();
    }
}

/// Get the number of results in a query result set.
///
/// # Arguments
///
/// * `results` - Query results
///
/// # Returns
///
/// Number of results, or 0 if results is null.
#[no_mangle]
pub unsafe extern "C" fn grovedb_query_results_count(results: *const QueryResults) -> size_t {
    if results.is_null() {
        0
    } else {
        (*results).count
    }
}

/// Get a result element at the specified index.
///
/// # Arguments
///
/// * `results` - Query results
/// * `index` - Index of the result to get
///
/// # Returns
///
/// Pointer to the result element, or null if out of bounds.
/// The returned pointer is valid until the results are freed.
///
/// # Safety
///
/// * `results` must be valid
#[no_mangle]
pub unsafe extern "C" fn grovedb_query_results_get(
    results: *const QueryResults,
    index: size_t,
) -> *const QueryResultElement {
    if results.is_null() || index >= (*results).count {
        std::ptr::null()
    } else {
        (*results).elements.add(index)
    }
}
