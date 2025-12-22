//! Database lifecycle and core operations for the GroveDB FFI.
//!
//! This module provides functions for opening, closing, and managing GroveDB
//! instances, as well as core CRUD operations.

use grovedb::GroveDb;
use grovedb_version::version::GroveVersion;
use libc::c_char;
use std::ffi::CStr;

use crate::element::GroveDbElement;
use crate::error::{set_error, set_ok, GroveDbError, GroveDbErrorCode};
use crate::handles::{
    register_handle, remove_handle, tx_arg_from_guard, GroveDbHandle, GroveDbWrapper,
    TransactionHandle,
};
use crate::panic::{catch_panic, catch_panic_ptr};
use crate::types::{ByteBuffer, DeleteOptions, GroveDbPath, InsertOptions};

/// Open or create a GroveDB at the specified path.
///
/// # Arguments
///
/// * `path` - Null-terminated string path to the database directory
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// A handle to the opened database, or null on error.
/// The caller must close the database with `grovedb_close()` when done.
///
/// # Safety
///
/// * `path` must be a valid null-terminated UTF-8 string
#[no_mangle]
pub unsafe extern "C" fn grovedb_open(
    path: *const c_char,
    error_out: *mut GroveDbError,
) -> *mut GroveDbHandle {
    catch_panic_ptr(error_out, || {
        // Validate path
        if path.is_null() {
            set_error(error_out, GroveDbError::null_pointer("path"));
            return std::ptr::null_mut();
        }

        // Convert to Rust string
        let path_str = match CStr::from_ptr(path).to_str() {
            Ok(s) => s,
            Err(_) => {
                set_error(
                    error_out,
                    GroveDbError::new(GroveDbErrorCode::InvalidInput, "path is not valid UTF-8"),
                );
                return std::ptr::null_mut();
            }
        };

        // Open the database
        match GroveDb::open(path_str) {
            Ok(db) => {
                set_ok(error_out);
                let wrapper = GroveDbWrapper::new(db);
                let id = register_handle(wrapper);
                GroveDbHandle::from_id(id)
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Close a GroveDB handle and free all associated resources.
///
/// This will also abort any uncommitted transactions.
///
/// # Thread Safety
///
/// Calling this function concurrently with other operations on the same handle
/// is undefined behavior. The caller must ensure proper synchronization.
///
/// # Arguments
///
/// * `handle` - Handle to close (may be null, which is a no-op)
///
/// # Safety
///
/// * `handle` must have been returned by `grovedb_open()`
/// * `handle` must not be used after this call
/// * `handle` must not be used concurrently with this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_close(handle: *mut GroveDbHandle) {
    if handle.is_null() {
        return;
    }

    // Get the ID from the handle
    let id = match GroveDbHandle::get_id(handle) {
        Some(id) => id,
        None => return,
    };

    // Free the handle struct (just the Box containing the ID)
    GroveDbHandle::free_handle(handle);

    // Remove from handle table and clean up
    if let Some(wrapper) = remove_handle(id) {
        // Mark as closed to reject any new operations
        wrapper.mark_closed();
        // Clear all pending transactions
        wrapper.clear_transactions();
        // Drop the Arc - database is freed when all references are dropped
        // (in-flight operations may still hold Arc references)
    }
}

/// Flush all pending writes to disk.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * `handle` must be a valid database handle
#[no_mangle]
pub unsafe extern "C" fn grovedb_flush(
    handle: *const GroveDbHandle,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return false;
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return false;
        }

        match wrapper.db().flush() {
            Ok(()) => {
                set_ok(error_out);
                true
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                false
            }
        }
    })
}

/// Get the root hash of the database.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `transaction` - Optional transaction handle (may be null for no transaction)
/// * `hash_out` - Pointer to 32-byte buffer to receive the hash
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `hash_out` must point to at least 32 bytes of writable memory
#[no_mangle]
pub unsafe extern "C" fn grovedb_root_hash(
    handle: *const GroveDbHandle,
    transaction: *const TransactionHandle,
    hash_out: *mut [u8; 32],
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return false;
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return false;
        }

        if hash_out.is_null() {
            set_error(error_out, GroveDbError::null_pointer("hash_out"));
            return false;
        }

        let version = GroveVersion::latest();

        // Get transaction guard if provided
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
                    return false;
                }
            }
        };

        // Extract result from CostContext (cost is discarded at FFI boundary)
        let grovedb_costs::CostContext { value: result, .. } =
            wrapper.db().root_hash(tx_arg_from_guard(&tx_guard), version);
        match result {
            Ok(hash) => {
                (*hash_out).copy_from_slice(&hash);
                set_ok(error_out);
                true
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                false
            }
        }
    })
}

/// Insert an element at the specified path and key.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `path` - Path to the subtree
/// * `key` - Key within the subtree
/// * `element` - Element to insert
/// * `options` - Insert options (may be null for defaults)
/// * `transaction` - Optional transaction handle (may be null)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * All pointers must be valid or null where allowed
/// * `path` segments and `key` data must remain valid during the call
#[no_mangle]
pub unsafe extern "C" fn grovedb_insert(
    handle: *const GroveDbHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    element: *const GroveDbElement,
    options: *const InsertOptions,
    transaction: *const TransactionHandle,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        // Validate handle
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return false;
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return false;
        }

        // Validate element
        if element.is_null() {
            set_error(error_out, GroveDbError::null_pointer("element"));
            return false;
        }

        // Convert path
        let path_vec = if path.is_null() {
            Vec::new()
        } else {
            match (*path).to_vec_validated() {
                Ok(v) => v,
                Err(e) => {
                    set_error(error_out, e);
                    return false;
                }
            }
        };
        let path_slices: Vec<&[u8]> = path_vec.iter().map(|v| v.as_slice()).collect();

        // Convert key
        let key_slice = match key.as_slice() {
            Some(k) => k,
            None => {
                set_error(error_out, GroveDbError::null_pointer("key"));
                return false;
            }
        };

        // Convert element
        let elem = match (*element).to_grovedb_element() {
            Ok(e) => e,
            Err(err) => {
                set_error(error_out, err);
                return false;
            }
        };

        // Convert options
        let opts = InsertOptions::from_ptr(options);

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
                    return false;
                }
            }
        };

        let version = GroveVersion::latest();

        // Perform insert (extract result from CostContext)
        let grovedb_costs::CostContext { value: result, .. } = wrapper
            .db()
            .insert(path_slices.as_slice(), key_slice, elem, opts, tx_arg_from_guard(&tx_guard), version);
        match result {
            Ok(()) => {
                set_ok(error_out);
                true
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                false
            }
        }
    })
}

/// Get an element at the specified path and key.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `path` - Path to the subtree
/// * `key` - Key within the subtree
/// * `transaction` - Optional transaction handle (may be null)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// The element if found, or null on error. Caller must free with `grovedb_free_element()`.
///
/// # Safety
///
/// * All pointers must be valid or null where allowed
#[no_mangle]
pub unsafe extern "C" fn grovedb_get(
    handle: *const GroveDbHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    transaction: *const TransactionHandle,
    error_out: *mut GroveDbError,
) -> *mut GroveDbElement {
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

        // Convert path
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
        let path_slices: Vec<&[u8]> = path_vec.iter().map(|v| v.as_slice()).collect();

        // Convert key
        let key_slice = match key.as_slice() {
            Some(k) => k,
            None => {
                set_error(error_out, GroveDbError::null_pointer("key"));
                return std::ptr::null_mut();
            }
        };

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

        // Perform get (extract result from CostContext)
        let grovedb_costs::CostContext { value: result, .. } = wrapper
            .db()
            .get(path_slices.as_slice(), key_slice, tx_arg_from_guard(&tx_guard), version);
        match result {
            Ok(elem) => {
                set_ok(error_out);
                Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Delete an element at the specified path and key.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `path` - Path to the subtree
/// * `key` - Key within the subtree
/// * `options` - Delete options (may be null for defaults)
/// * `transaction` - Optional transaction handle (may be null)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * All pointers must be valid or null where allowed
#[no_mangle]
pub unsafe extern "C" fn grovedb_delete(
    handle: *const GroveDbHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    options: *const DeleteOptions,
    transaction: *const TransactionHandle,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        // Validate handle
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return false;
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return false;
        }

        // Convert path
        let path_vec = if path.is_null() {
            Vec::new()
        } else {
            match (*path).to_vec_validated() {
                Ok(v) => v,
                Err(e) => {
                    set_error(error_out, e);
                    return false;
                }
            }
        };
        let path_slices: Vec<&[u8]> = path_vec.iter().map(|v| v.as_slice()).collect();

        // Convert key
        let key_slice = match key.as_slice() {
            Some(k) => k,
            None => {
                set_error(error_out, GroveDbError::null_pointer("key"));
                return false;
            }
        };

        // Convert options
        let opts = DeleteOptions::from_ptr(options);

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
                    return false;
                }
            }
        };

        let version = GroveVersion::latest();

        // Perform delete (extract result from CostContext)
        let grovedb_costs::CostContext { value: result, .. } = wrapper
            .db()
            .delete(path_slices.as_slice(), key_slice, opts, tx_arg_from_guard(&tx_guard), version);
        match result {
            Ok(()) => {
                set_ok(error_out);
                true
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                false
            }
        }
    })
}
