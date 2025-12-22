//! Owned transaction type for FFI and long-lived transaction handles.
//!
//! This module provides [`OwnedTransaction`], a transaction wrapper that owns
//! a reference to its database via `Arc`, allowing the transaction to be safely
//! held across FFI boundaries or in contexts where Rust's lifetime tracking
//! cannot be applied.
//!
//! # Safety
//!
//! The safety of this module relies on the fact that RocksDB's `Transaction`
//! internally holds a pointer to the database. As long as the database is kept
//! alive (via the `Arc<GroveDb>`), the transaction's internal pointer remains
//! valid. This is a well-established pattern used by the rocksdb crate itself.
//!
//! # Example
//!
//! ```
//! use grovedb::GroveDb;
//! use grovedb::owned_transaction::OwnedTransaction;
//! use std::sync::Arc;
//! use tempfile::TempDir;
//!
//! let tmp_dir = TempDir::new().unwrap();
//! let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());
//!
//! // Create an owned transaction
//! let tx = OwnedTransaction::new(Arc::clone(&db));
//!
//! // Use it for operations...
//! // tx.as_ref() returns &Transaction for use with GroveDb methods
//!
//! // Commit or rollback
//! tx.commit().unwrap().unwrap();
//! ```

use std::sync::Arc;

use grovedb_costs::CostResult;

use crate::error::Error;
use crate::{GroveDb, Transaction};

/// A transaction that owns a reference to its database.
///
/// This type wraps a GroveDB transaction along with an `Arc<GroveDb>` that
/// keeps the database alive for the lifetime of the transaction. This allows
/// the transaction to be safely used across FFI boundaries where Rust's
/// lifetime system cannot track the relationship.
///
/// # Thread Safety
///
/// `OwnedTransaction` is `Send` but not `Sync`, matching the underlying
/// RocksDB transaction semantics. A transaction can be moved between threads
/// but should not be accessed concurrently from multiple threads.
///
/// # Memory Safety
///
/// The `Arc<GroveDb>` ensures the database cannot be dropped while the
/// transaction is alive. The internal lifetime transmute is safe because:
///
/// 1. The transaction only holds a pointer to RocksDB internal state
/// 2. The `Arc<GroveDb>` keeps that state alive
/// 3. The transaction is dropped before or with the Arc reference
pub struct OwnedTransaction {
    /// Reference to the database - keeps it alive for the transaction's lifetime.
    /// This MUST be declared before `tx` to ensure correct drop order.
    db: Arc<GroveDb>,

    /// The underlying transaction with a transmuted 'static lifetime.
    ///
    /// # Safety
    ///
    /// This is safe because:
    /// - The `db` field keeps the GroveDb alive
    /// - GroveDb keeps the RocksDbStorage alive
    /// - The transaction only references internal RocksDB state
    /// - Drop order ensures `tx` is dropped before `db`
    tx: Transaction<'static>,
}

// SAFETY: OwnedTransaction can be sent between threads because:
// 1. Arc<GroveDb> is Send + Sync
// 2. Transaction is Send (but not Sync)
// The !Sync bound is inherited from Transaction
unsafe impl Send for OwnedTransaction {}

impl OwnedTransaction {
    /// Create a new owned transaction from an Arc-wrapped GroveDb.
    ///
    /// # Arguments
    ///
    /// * `db` - An `Arc<GroveDb>` that will be kept alive for the transaction's lifetime
    ///
    /// # Example
    ///
    /// ```
    /// use grovedb::GroveDb;
    /// use grovedb::owned_transaction::OwnedTransaction;
    /// use std::sync::Arc;
    /// use tempfile::TempDir;
    ///
    /// let tmp_dir = TempDir::new().unwrap();
    /// let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());
    /// let tx = OwnedTransaction::new(Arc::clone(&db));
    /// ```
    pub fn new(db: Arc<GroveDb>) -> Self {
        let tx = db.start_transaction();

        // SAFETY: We're extending the lifetime of the transaction from 'db to 'static.
        // This is safe because:
        //
        // 1. The transaction internally holds a pointer to RocksDB's internal state
        // 2. The Arc<GroveDb> we store ensures the GroveDb (and thus RocksDbStorage)
        //    cannot be dropped while this OwnedTransaction exists
        // 3. Rust's drop order guarantees `tx` is dropped before `db` since `db`
        //    is declared first in the struct
        // 4. The rocksdb crate's Transaction only uses the database pointer for
        //    read/write operations - it doesn't capture any stack references
        //
        // This pattern is also used in grovedb's state_sync_session.rs for the
        // same reason.
        let tx_static: Transaction<'static> = unsafe { std::mem::transmute(tx) };

        Self { db, tx: tx_static }
    }

    /// Get a reference to the underlying transaction.
    ///
    /// The returned reference has the lifetime of the `OwnedTransaction`,
    /// ensuring it cannot outlive the transaction.
    ///
    /// # Example
    ///
    /// ```
    /// use grovedb::{Element, GroveDb};
    /// use grovedb::owned_transaction::OwnedTransaction;
    /// use grovedb_version::version::GroveVersion;
    /// use std::sync::Arc;
    /// use tempfile::TempDir;
    ///
    /// let grove_version = GroveVersion::latest();
    /// let tmp_dir = TempDir::new().unwrap();
    /// let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());
    /// let tx = OwnedTransaction::new(Arc::clone(&db));
    ///
    /// // Use as_ref() to get a transaction reference for GroveDb operations
    /// db.insert(
    ///     &[],
    ///     b"key",
    ///     Element::empty_tree(),
    ///     None,
    ///     Some(tx.as_ref()),
    ///     grove_version,
    /// ).unwrap().unwrap();
    /// ```
    #[inline]
    pub fn as_ref(&self) -> &Transaction<'static> {
        &self.tx
    }

    /// Get a reference to the database.
    ///
    /// This can be useful when you need to perform operations on the database
    /// while holding the transaction.
    #[inline]
    pub fn db(&self) -> &GroveDb {
        &self.db
    }

    /// Get a clone of the Arc-wrapped database.
    ///
    /// This can be useful when you need to share the database reference.
    #[inline]
    pub fn db_arc(&self) -> Arc<GroveDb> {
        Arc::clone(&self.db)
    }

    /// Commit the transaction.
    ///
    /// This consumes the `OwnedTransaction`, ensuring it cannot be used after
    /// commit. The database Arc reference is released after the commit completes.
    ///
    /// # Returns
    ///
    /// A `CostResult` containing either `()` on success or an `Error` on failure.
    ///
    /// # Example
    ///
    /// ```
    /// use grovedb::GroveDb;
    /// use grovedb::owned_transaction::OwnedTransaction;
    /// use std::sync::Arc;
    /// use tempfile::TempDir;
    ///
    /// let tmp_dir = TempDir::new().unwrap();
    /// let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());
    /// let tx = OwnedTransaction::new(Arc::clone(&db));
    ///
    /// // Perform operations...
    ///
    /// // Commit consumes the transaction
    /// tx.commit().unwrap().unwrap();
    /// ```
    pub fn commit(self) -> CostResult<(), Error> {
        self.db.commit_transaction(self.tx)
    }

    /// Rollback the transaction.
    ///
    /// Unlike `commit`, this does not consume the transaction because RocksDB
    /// allows continued use of a transaction after rollback. However, in most
    /// FFI use cases, you'll want to drop the transaction after rollback.
    ///
    /// # Returns
    ///
    /// `Ok(())` on success, or an `Error` on failure.
    ///
    /// # Example
    ///
    /// ```
    /// use grovedb::GroveDb;
    /// use grovedb::owned_transaction::OwnedTransaction;
    /// use std::sync::Arc;
    /// use tempfile::TempDir;
    ///
    /// let tmp_dir = TempDir::new().unwrap();
    /// let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());
    /// let tx = OwnedTransaction::new(Arc::clone(&db));
    ///
    /// // Perform operations...
    ///
    /// // Rollback the transaction
    /// tx.rollback().unwrap();
    /// // Transaction can still be used or dropped
    /// ```
    pub fn rollback(&self) -> Result<(), Error> {
        self.db.rollback_transaction(&self.tx)
    }

    /// Consume the transaction without committing or rolling back.
    ///
    /// This is equivalent to dropping the transaction, which in RocksDB
    /// semantics means the transaction is implicitly rolled back.
    ///
    /// This method is provided for explicit intent in FFI scenarios.
    pub fn abort(self) {
        // Just drop self - the transaction will be implicitly rolled back
        drop(self);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Element;
    use grovedb_version::version::GroveVersion;
    use tempfile::TempDir;

    #[test]
    fn test_owned_transaction_basic() {
        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().unwrap();
        let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());

        let tx = OwnedTransaction::new(Arc::clone(&db));

        let root_path: &[&[u8]] = &[];

        // Insert within transaction
        db.insert(
            root_path,
            b"test_key",
            Element::empty_tree(),
            None,
            Some(tx.as_ref()),
            grove_version,
        )
        .unwrap()
        .unwrap();

        // Commit
        tx.commit().unwrap().unwrap();

        // Verify the insert persisted
        let result = db
            .get(root_path, b"test_key", None, grove_version)
            .unwrap()
            .unwrap();
        assert_eq!(result, Element::empty_tree());
    }

    #[test]
    fn test_owned_transaction_rollback() {
        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().unwrap();
        let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());

        let tx = OwnedTransaction::new(Arc::clone(&db));

        let root_path: &[&[u8]] = &[];

        // Insert within transaction
        db.insert(
            root_path,
            b"rollback_key",
            Element::empty_tree(),
            None,
            Some(tx.as_ref()),
            grove_version,
        )
        .unwrap()
        .unwrap();

        // Rollback
        tx.rollback().unwrap();
        drop(tx);

        // Verify the insert was rolled back
        let result = db.get(root_path, b"rollback_key", None, grove_version).unwrap();
        assert!(result.is_err());
    }

    #[test]
    fn test_owned_transaction_abort() {
        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().unwrap();
        let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());

        let tx = OwnedTransaction::new(Arc::clone(&db));

        let root_path: &[&[u8]] = &[];

        // Insert within transaction
        db.insert(
            root_path,
            b"abort_key",
            Element::empty_tree(),
            None,
            Some(tx.as_ref()),
            grove_version,
        )
        .unwrap()
        .unwrap();

        // Abort (implicit rollback)
        tx.abort();

        // Verify the insert was rolled back
        let result = db.get(root_path, b"abort_key", None, grove_version).unwrap();
        assert!(result.is_err());
    }

    #[test]
    fn test_owned_transaction_db_access() {
        let tmp_dir = TempDir::new().unwrap();
        let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());

        let tx = OwnedTransaction::new(Arc::clone(&db));

        // Verify we can access the db through the transaction
        assert!(Arc::ptr_eq(&tx.db_arc(), &db));

        tx.abort();
    }

    #[test]
    fn test_multiple_transactions_sequential() {
        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().unwrap();
        let db = Arc::new(GroveDb::open(tmp_dir.path()).unwrap());

        let root_path: &[&[u8]] = &[];

        // First transaction
        let tx1 = OwnedTransaction::new(Arc::clone(&db));
        db.insert(
            root_path,
            b"key1",
            Element::empty_tree(),
            None,
            Some(tx1.as_ref()),
            grove_version,
        )
        .unwrap()
        .unwrap();
        tx1.commit().unwrap().unwrap();

        // Second transaction
        let tx2 = OwnedTransaction::new(Arc::clone(&db));
        db.insert(
            root_path,
            b"key2",
            Element::empty_tree(),
            None,
            Some(tx2.as_ref()),
            grove_version,
        )
        .unwrap()
        .unwrap();
        tx2.commit().unwrap().unwrap();

        // Both should exist
        assert!(db.get(root_path, b"key1", None, grove_version).unwrap().is_ok());
        assert!(db.get(root_path, b"key2", None, grove_version).unwrap().is_ok());
    }
}
