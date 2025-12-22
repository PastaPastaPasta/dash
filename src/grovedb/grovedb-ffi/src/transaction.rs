//! Transaction management for the GroveDB FFI.
//!
//! This module provides functions for starting, committing, and rolling back
//! transactions in GroveDB.

use crate::error::{set_error, set_ok, GroveDbError, GroveDbErrorCode};
use crate::handles::{GroveDbHandle, TransactionHandle};
use crate::panic::{catch_panic, catch_panic_ptr};

/// Start a new transaction.
///
/// All operations using this transaction will be isolated until commit or rollback.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// A transaction handle, or null on error.
/// Use `grovedb_commit_transaction()` or `grovedb_rollback_transaction()` to finish.
/// The handle is automatically freed when committed.
/// For rollback, you can continue using the transaction or free it with
/// `grovedb_free_transaction()`.
///
/// # Safety
///
/// * `handle` must be a valid database handle
#[no_mangle]
pub unsafe extern "C" fn grovedb_start_transaction(
    handle: *const GroveDbHandle,
    error_out: *mut GroveDbError,
) -> *mut TransactionHandle {
    catch_panic_ptr(error_out, || {
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

        let tx_id = wrapper.start_transaction();
        set_ok(error_out);
        TransactionHandle::boxed(tx_id)
    })
}

/// Commit a transaction.
///
/// All changes made within the transaction are persisted to the database.
/// The transaction handle is consumed and should not be used after this call.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `transaction` - Transaction handle to commit (consumed on success)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `transaction` must be a valid transaction handle from this database
/// * `transaction` must not be used after successful commit
#[no_mangle]
pub unsafe extern "C" fn grovedb_commit_transaction(
    handle: *const GroveDbHandle,
    transaction: *mut TransactionHandle,
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

        let tx_id = match TransactionHandle::get_id(transaction) {
            Some(id) => id,
            None => {
                set_error(error_out, GroveDbError::null_pointer("transaction"));
                return false;
            }
        };

        match wrapper.commit_transaction(tx_id) {
            Ok(()) => {
                // Free the handle since the transaction is consumed
                TransactionHandle::free(transaction);
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

/// Rollback a transaction.
///
/// All changes made within the transaction are discarded.
/// After rollback, the transaction is invalidated and cannot be reused.
/// Free the transaction handle with `grovedb_free_transaction()`.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `transaction` - Transaction handle to rollback
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `transaction` must be a valid transaction handle from this database
/// * `transaction` cannot be used for operations after rollback
#[no_mangle]
pub unsafe extern "C" fn grovedb_rollback_transaction(
    handle: *const GroveDbHandle,
    transaction: *const TransactionHandle,
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

        let tx_id = match TransactionHandle::get_id(transaction) {
            Some(id) => id,
            None => {
                set_error(error_out, GroveDbError::null_pointer("transaction"));
                return false;
            }
        };

        match wrapper.rollback_transaction(tx_id) {
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

/// Abort a transaction without commit or rollback.
///
/// This simply drops the transaction, discarding any uncommitted changes.
/// Use this when you want to cancel a transaction without going through
/// the normal rollback process.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `transaction` - Transaction handle to abort (consumed)
///
/// # Returns
///
/// `true` if the transaction was found and aborted, `false` if not found.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `transaction` must not be used after this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_abort_transaction(
    handle: *const GroveDbHandle,
    transaction: *mut TransactionHandle,
) -> bool {
    if handle.is_null() || transaction.is_null() {
        return false;
    }

    let wrapper = match GroveDbHandle::get_wrapper(handle) {
        Some(w) => w,
        None => return false,
    };

    // If database is closed, we still free the handle but return false
    if wrapper.is_closed() {
        TransactionHandle::free(transaction);
        return false;
    }

    let tx_id = match TransactionHandle::get_id(transaction) {
        Some(id) => id,
        None => return false,
    };

    let result = wrapper.abort_transaction(tx_id);

    // Always free the handle
    TransactionHandle::free(transaction);

    result
}

/// Free a transaction handle.
///
/// Use this to free a transaction handle that was rolled back and is no longer needed,
/// or to clean up after an error. Do not use on committed transactions (they are
/// automatically freed).
///
/// # Arguments
///
/// * `transaction` - Transaction handle to free (may be null)
///
/// # Safety
///
/// * `transaction` must have been returned by `grovedb_start_transaction()`
/// * `transaction` must not be used after this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_transaction(transaction: *mut TransactionHandle) {
    TransactionHandle::free(transaction);
}

/// Check if a transaction is active.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `transaction` - Transaction handle to check
///
/// # Returns
///
/// `true` if the transaction is active, `false` otherwise.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `transaction` must be a valid transaction handle
#[no_mangle]
pub unsafe extern "C" fn grovedb_is_transaction_active(
    handle: *const GroveDbHandle,
    transaction: *const TransactionHandle,
) -> bool {
    if handle.is_null() || transaction.is_null() {
        return false;
    }

    let wrapper = match GroveDbHandle::get_wrapper(handle) {
        Some(w) => w,
        None => return false,
    };

    // If database is closed, no transactions can be active
    if wrapper.is_closed() {
        return false;
    }

    let tx_id = match TransactionHandle::get_id(transaction) {
        Some(id) => id,
        None => return false,
    };

    wrapper.has_transaction(tx_id)
}
