//! Opaque handle types for the GroveDB FFI.
//!
//! These handles hide the internal Rust types from C code, providing type-safe
//! pointers that can only be used through the FFI functions.
//!
//! # Thread Safety
//!
//! The handle table approach ensures that concurrent operations on the same database
//! are safe. However, calling `grovedb_close()` concurrently with other operations
//! on the same handle is undefined behavior (the caller must ensure synchronization).
//!
//! This is the standard FFI contract used by SQLite, RocksDB, and similar libraries.

use grovedb::{GroveDb, OwnedTransaction};
use once_cell::sync::Lazy;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, RwLock};

// =============================================================================
// Global Handle Table
// =============================================================================
//
// The handle table maps integer IDs to database wrappers, protecting all access
// with a RwLock. This eliminates the race condition where a handle pointer could
// be dereferenced while being freed by another thread calling close().

/// Global handle table - maps integer IDs to database wrappers
static HANDLE_TABLE: Lazy<RwLock<HashMap<u64, Arc<GroveDbWrapper>>>> =
    Lazy::new(|| RwLock::new(HashMap::new()));

/// Counter for generating unique handle IDs
static HANDLE_ID_COUNTER: AtomicU64 = AtomicU64::new(1);

/// A counter for generating unique transaction IDs
static TRANSACTION_ID_COUNTER: AtomicU64 = AtomicU64::new(1);

/// Register a new database wrapper and return its handle ID.
///
/// The wrapper is stored in the global table and can be retrieved with `get_handle()`.
pub fn register_handle(wrapper: GroveDbWrapper) -> u64 {
    let id = HANDLE_ID_COUNTER.fetch_add(1, Ordering::Relaxed);
    HANDLE_TABLE
        .write()
        .unwrap()
        .insert(id, Arc::new(wrapper));
    id
}

/// Get a cloned Arc to a database wrapper by ID.
///
/// Returns None if the ID is not found (database was closed).
/// The returned Arc keeps the database alive for the duration of the operation.
pub fn get_handle(id: u64) -> Option<Arc<GroveDbWrapper>> {
    HANDLE_TABLE.read().unwrap().get(&id).cloned()
}

/// Remove and return a database wrapper from the table.
///
/// Used during close() to remove the database from the table.
/// The returned Arc may still have other references (from in-flight operations),
/// so the actual database is only freed when all references are dropped.
pub fn remove_handle(id: u64) -> Option<Arc<GroveDbWrapper>> {
    HANDLE_TABLE.write().unwrap().remove(&id)
}

/// A guard that holds the transaction lock and provides safe access to the transaction.
///
/// The lock is held for the lifetime of this guard, ensuring the transaction reference
/// remains valid for the duration of the database operation.
pub struct TransactionGuard<'a> {
    /// The mutex guard that keeps the transaction map locked
    guard: MutexGuard<'a, HashMap<u64, OwnedTransaction>>,
    /// The transaction ID to look up
    id: u64,
}

impl<'a> TransactionGuard<'a> {
    /// Get the transaction argument with correct lifetime.
    ///
    /// The reference lifetime is tied to the guard, not 'static.
    pub fn tx_arg(&self) -> grovedb::TransactionArg<'static, '_> {
        self.guard.get(&self.id).map(|owned| owned.as_ref())
    }
}

/// Helper to get TransactionArg from Option<TransactionGuard>.
///
/// Returns None if the guard is None, otherwise returns the transaction argument.
pub fn tx_arg_from_guard<'a>(guard: &'a Option<TransactionGuard<'a>>) -> grovedb::TransactionArg<'static, 'a> {
    guard.as_ref().and_then(|g| g.tx_arg())
}

/// Wrapper around GroveDb that also manages transactions.
///
/// This struct owns the GroveDb via Arc and maintains a map of active transactions.
/// Each transaction uses `grovedb::OwnedTransaction` which internally keeps the
/// database alive via its own Arc reference.
pub struct GroveDbWrapper {
    /// The underlying GroveDb instance (Arc for shared ownership with transactions)
    db: Arc<GroveDb>,
    /// Active transactions indexed by ID.
    /// Using `grovedb::OwnedTransaction` which safely manages the transaction lifetime.
    transactions: Mutex<HashMap<u64, OwnedTransaction>>,
    /// Flag indicating the database has been closed.
    /// Once set, all operations should return an error.
    closed: AtomicBool,
}

impl GroveDbWrapper {
    /// Create a new wrapper around a GroveDb
    pub fn new(db: GroveDb) -> Self {
        Self {
            db: Arc::new(db),
            transactions: Mutex::new(HashMap::new()),
            closed: AtomicBool::new(false),
        }
    }

    /// Get a reference to the underlying GroveDb
    pub fn db(&self) -> &GroveDb {
        &self.db
    }

    /// Get a clone of the Arc-wrapped GroveDb.
    ///
    /// This is used when creating `OwnedTransaction` instances.
    pub fn db_arc(&self) -> Arc<GroveDb> {
        Arc::clone(&self.db)
    }

    /// Check if the database has been closed.
    ///
    /// Returns true if `mark_closed()` has been called, indicating
    /// that no further operations should be performed on this database.
    pub fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire)
    }

    /// Mark the database as closed.
    ///
    /// After this is called, `is_closed()` will return true.
    /// This should be called before dropping to prevent race conditions.
    pub fn mark_closed(&self) {
        self.closed.store(true, Ordering::Release);
    }

    /// Clear all transactions.
    ///
    /// This should be called during close() to clean up any pending transactions.
    pub fn clear_transactions(&self) {
        self.transactions.lock().unwrap().clear();
    }

    /// Start a new transaction and return its ID.
    ///
    /// Uses `grovedb::OwnedTransaction` which safely handles the transaction
    /// lifetime by keeping an Arc reference to the database.
    pub fn start_transaction(&self) -> u64 {
        let id = TRANSACTION_ID_COUNTER.fetch_add(1, Ordering::Relaxed);

        // Create an OwnedTransaction using the grovedb crate's safe implementation.
        // This handles the lifetime transmute safely by keeping an Arc<GroveDb>.
        let owned_tx = OwnedTransaction::new(Arc::clone(&self.db));

        self.transactions.lock().unwrap().insert(id, owned_tx);
        id
    }

    /// Get a transaction guard that holds the lock for the duration of an operation.
    ///
    /// Returns None if the transaction ID is not found.
    /// The returned guard holds the mutex lock until dropped, ensuring the
    /// transaction reference remains valid.
    pub fn get_transaction_guard(&self, id: u64) -> Option<TransactionGuard<'_>> {
        let guard = self.transactions.lock().unwrap();
        if !guard.contains_key(&id) {
            return None;
        }
        Some(TransactionGuard { guard, id })
    }

    /// Commit a transaction by ID
    pub fn commit_transaction(&self, id: u64) -> Result<(), grovedb::Error> {
        let owned = self
            .transactions
            .lock()
            .unwrap()
            .remove(&id)
            .ok_or_else(|| grovedb::Error::InternalError("transaction not found".into()))?;

        // Use OwnedTransaction's commit method which extracts the result
        let cost_result = owned.commit();
        cost_result.unwrap()
    }

    /// Rollback a transaction by ID.
    ///
    /// After rollback, the transaction is removed from the HashMap and cannot
    /// be reused. The transaction handle should be freed with `grovedb_free_transaction()`.
    pub fn rollback_transaction(&self, id: u64) -> Result<(), grovedb::Error> {
        let mut guard = self.transactions.lock().unwrap();
        let owned = guard
            .get(&id)
            .ok_or_else(|| grovedb::Error::InternalError("transaction not found".into()))?;

        // Rollback the transaction using OwnedTransaction's method
        owned.rollback()?;

        // Remove from HashMap after successful rollback.
        // Transaction cannot be reused after rollback.
        guard.remove(&id);

        Ok(())
    }

    /// Abort (drop) a transaction by ID without commit or rollback
    pub fn abort_transaction(&self, id: u64) -> bool {
        self.transactions.lock().unwrap().remove(&id).is_some()
    }

    /// Check if a transaction exists
    pub fn has_transaction(&self, id: u64) -> bool {
        self.transactions.lock().unwrap().contains_key(&id)
    }
}

impl Drop for GroveDbWrapper {
    fn drop(&mut self) {
        // Mark as closed first to prevent any concurrent operations from starting.
        // This is critical for safety: we set this BEFORE clearing transactions
        // so that any racing operations will see the closed flag and abort.
        self.mark_closed();

        // Clear all transactions before dropping.
        // OwnedTransaction's drop will handle cleanup properly.
        self.transactions.lock().unwrap().clear();
    }
}

// =============================================================================
// FFI Handle Types
// =============================================================================

/// Opaque handle to a GroveDb instance.
///
/// This handle stores an ID that maps to the actual database wrapper in the
/// global handle table. This design eliminates race conditions between
/// operations and close by ensuring all access goes through the mutex-protected
/// table.
///
/// Obtain with `grovedb_open()`, free with `grovedb_close()`.
#[repr(C)]
pub struct GroveDbHandle {
    /// The handle ID that maps to an entry in HANDLE_TABLE
    id: u64,
}

impl GroveDbHandle {
    /// Create a handle with the given ID and return a pointer to it.
    ///
    /// The handle is boxed and the caller is responsible for freeing it.
    pub fn from_id(id: u64) -> *mut Self {
        Box::into_raw(Box::new(Self { id }))
    }

    /// Get the handle ID from a pointer.
    ///
    /// # Safety
    /// The pointer must be valid and non-null.
    pub unsafe fn get_id(ptr: *const Self) -> Option<u64> {
        if ptr.is_null() {
            None
        } else {
            Some((*ptr).id)
        }
    }

    /// Get a cloned Arc to the wrapper via the handle table.
    ///
    /// This is the safe way to access the wrapper - the table lookup is
    /// mutex-protected, and the returned Arc keeps the wrapper alive for
    /// the duration of the operation.
    ///
    /// Returns None if:
    /// - The pointer is null
    /// - The database was closed (ID not in table)
    ///
    /// # Safety
    /// The pointer must be valid and point to a GroveDbHandle created by `from_id()`.
    pub unsafe fn get_wrapper(ptr: *const Self) -> Option<Arc<GroveDbWrapper>> {
        let id = Self::get_id(ptr)?;
        get_handle(id)
    }

    /// Free the handle struct without affecting the database.
    ///
    /// This only frees the Box containing the handle ID, not the database itself.
    /// Use `remove_handle()` to also remove from the table.
    ///
    /// # Safety
    /// The pointer must have been created by `from_id()`.
    pub unsafe fn free_handle(ptr: *mut Self) {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Opaque handle representing a transaction ID.
///
/// This is actually just a wrapper around a u64 transaction ID.
/// Obtain with `grovedb_start_transaction()`, use `grovedb_commit_transaction()`
/// or `grovedb_rollback_transaction()` to finish.
#[repr(C)]
pub struct TransactionHandle {
    /// The transaction ID (not actually opaque, but presented as such to C)
    pub id: u64,
}

impl TransactionHandle {
    /// Create a new handle with the given ID
    pub fn new(id: u64) -> Self {
        Self { id }
    }

    /// Get the transaction ID from a pointer
    ///
    /// # Safety
    /// The pointer must be valid and non-null.
    pub unsafe fn get_id(ptr: *const Self) -> Option<u64> {
        ptr.as_ref().map(|h| h.id)
    }

    /// Create a boxed handle
    pub fn boxed(id: u64) -> *mut Self {
        Box::into_raw(Box::new(Self::new(id)))
    }

    /// Free a boxed handle
    ///
    /// # Safety
    /// The pointer must have been created by `boxed()`.
    pub unsafe fn free(ptr: *mut Self) {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    }
}

/// Opaque handle to a PathQuery builder.
///
/// Note: This handle actually stores a `PathQueryBuilder` internally, not a `PathQuery`.
/// The builder is converted to a `PathQuery` when the query is executed.
///
/// Obtain with `grovedb_path_query_new()`, free with `grovedb_free_path_query()`.
#[repr(C)]
pub struct PathQueryHandle {
    _private: [u8; 0],
}

// Note: PathQueryHandle methods were intentionally removed.
// The handle stores a PathQueryBuilder, but previous methods incorrectly
// assumed it stored a PathQuery. All FFI code uses direct pointer casts
// to PathQueryBuilder instead. See query.rs for usage.

#[cfg(test)]
mod tests {
    use super::*;

    // Note: Most tests require a real GroveDb, which needs a temp directory
    // These tests just verify the handle mechanics work

    #[test]
    fn test_transaction_handle() {
        let handle = TransactionHandle::new(42);
        assert_eq!(handle.id, 42);

        let ptr = TransactionHandle::boxed(123);
        unsafe {
            assert_eq!(TransactionHandle::get_id(ptr), Some(123));
            TransactionHandle::free(ptr);
        }
    }
}
