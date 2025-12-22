//! Batch operations for the GroveDB FFI.
//!
//! This module provides functions to build and apply batch operations atomically.
//! Batch operations are more efficient than individual operations when you need to
//! perform multiple inserts, updates, or deletes.

use grovedb::batch::QualifiedGroveDbOp;
use grovedb_merk::tree_type::TreeType;
use grovedb_version::version::GroveVersion;

use crate::element::GroveDbElement;
use crate::error::{set_error, set_ok, GroveDbError, GroveDbErrorCode};
use crate::handles::{tx_arg_from_guard, GroveDbHandle, TransactionHandle};
use crate::panic::catch_panic;
use crate::types::{ByteBuffer, GroveDbPath};

/// Operation type for batch operations.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BatchOpType {
    /// Insert only if the key doesn't exist
    InsertOnly = 0,
    /// Insert or replace if the key exists
    InsertOrReplace = 1,
    /// Replace only if the key exists
    Replace = 2,
    /// Delete an element
    Delete = 3,
    /// Delete a tree (non-sum tree)
    DeleteTree = 4,
    /// Delete a sum tree
    DeleteSumTree = 5,
}

/// A batch operation builder.
///
/// Build up a list of operations, then apply them atomically with `grovedb_apply_batch`.
pub struct BatchBuilder {
    operations: Vec<QualifiedGroveDbOp>,
}

/// Opaque handle to a batch builder.
#[repr(C)]
pub struct BatchHandle {
    _private: [u8; 0],
}

impl BatchHandle {
    /// Create a handle from a BatchBuilder
    pub fn from_builder(builder: Box<BatchBuilder>) -> *mut Self {
        Box::into_raw(builder) as *mut Self
    }

    /// Get a mutable reference to the builder
    ///
    /// # Safety
    /// The pointer must be valid and non-null.
    pub unsafe fn as_builder_mut<'a>(ptr: *mut Self) -> Option<&'a mut BatchBuilder> {
        (ptr as *mut BatchBuilder).as_mut()
    }

    /// Take ownership of the builder
    ///
    /// # Safety
    /// The pointer must be valid, non-null, and this must be the last use.
    pub unsafe fn into_builder(ptr: *mut Self) -> Option<Box<BatchBuilder>> {
        if ptr.is_null() {
            None
        } else {
            Some(Box::from_raw(ptr as *mut BatchBuilder))
        }
    }
}

/// Create a new batch operation builder.
///
/// # Arguments
///
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// A handle to the batch builder, or null on error.
/// The caller must apply or free the batch with `grovedb_apply_batch` or `grovedb_free_batch`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_batch_new(error_out: *mut GroveDbError) -> *mut BatchHandle {
    catch_panic(error_out, std::ptr::null_mut(), || {
        set_ok(error_out);
        let builder = Box::new(BatchBuilder {
            operations: Vec::new(),
        });
        BatchHandle::from_builder(builder)
    })
}

/// Add an insert operation to the batch.
///
/// # Arguments
///
/// * `batch` - Batch handle
/// * `path` - Path to the subtree
/// * `key` - Key within the subtree
/// * `element` - Element to insert (borrowed, caller retains ownership)
/// * `op_type` - Type of insert operation (InsertOnly, InsertOrReplace, Replace)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error. Element is always retained by caller.
#[no_mangle]
pub unsafe extern "C" fn grovedb_batch_add_insert(
    batch: *mut BatchHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    element: *const GroveDbElement,
    op_type: BatchOpType,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        // Validate batch handle
        let builder = match BatchHandle::as_builder_mut(batch) {
            Some(b) => b,
            None => {
                set_error(error_out, GroveDbError::null_pointer("batch"));
                return false;
            }
        };

        // Validate path
        if path.is_null() {
            set_error(error_out, GroveDbError::null_pointer("path"));
            return false;
        }

        // Validate element
        if element.is_null() {
            set_error(error_out, GroveDbError::null_pointer("element"));
            return false;
        }

        // Convert path to owned Vec<Vec<u8>> with validation
        let path_vec = match (*path).to_vec_validated() {
            Ok(v) => v,
            Err(e) => {
                set_error(error_out, e);
                return false;
            }
        };

        // Convert key to owned Vec<u8>
        let key_vec = if key.data.is_null() || key.len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(key.data, key.len).to_vec()
        };

        // Borrow element and convert to grovedb::Element
        let grove_element = match (*element).to_grovedb_element() {
            Ok(e) => e,
            Err(err) => {
                set_error(error_out, err);
                return false;
            }
        };

        // Create the operation based on type
        let op = match op_type {
            BatchOpType::InsertOnly => QualifiedGroveDbOp::insert_only_op(path_vec, key_vec, grove_element),
            BatchOpType::InsertOrReplace => QualifiedGroveDbOp::insert_or_replace_op(path_vec, key_vec, grove_element),
            BatchOpType::Replace => QualifiedGroveDbOp::replace_op(path_vec, key_vec, grove_element),
            _ => {
                set_error(
                    error_out,
                    GroveDbError::new(GroveDbErrorCode::InvalidParameter, "Invalid op_type for insert"),
                );
                return false;
            }
        };

        builder.operations.push(op);
        set_ok(error_out);
        true
    })
}

/// Add a delete operation to the batch.
///
/// # Arguments
///
/// * `batch` - Batch handle
/// * `path` - Path to the subtree
/// * `key` - Key to delete
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
#[no_mangle]
pub unsafe extern "C" fn grovedb_batch_add_delete(
    batch: *mut BatchHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        // Validate batch handle
        let builder = match BatchHandle::as_builder_mut(batch) {
            Some(b) => b,
            None => {
                set_error(error_out, GroveDbError::null_pointer("batch"));
                return false;
            }
        };

        // Validate path
        if path.is_null() {
            set_error(error_out, GroveDbError::null_pointer("path"));
            return false;
        }

        // Convert path to owned Vec<Vec<u8>> with validation
        let path_vec = match (*path).to_vec_validated() {
            Ok(v) => v,
            Err(e) => {
                set_error(error_out, e);
                return false;
            }
        };

        // Convert key to owned Vec<u8>
        let key_vec = if key.data.is_null() || key.len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(key.data, key.len).to_vec()
        };

        let op = QualifiedGroveDbOp::delete_op(path_vec, key_vec);
        builder.operations.push(op);
        set_ok(error_out);
        true
    })
}

/// Add a delete tree operation to the batch.
///
/// # Arguments
///
/// * `batch` - Batch handle
/// * `path` - Path to the subtree containing the tree
/// * `key` - Key of the tree to delete
/// * `is_sum_tree` - Whether the tree is a sum tree
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
#[no_mangle]
pub unsafe extern "C" fn grovedb_batch_add_delete_tree(
    batch: *mut BatchHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    is_sum_tree: bool,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        // Validate batch handle
        let builder = match BatchHandle::as_builder_mut(batch) {
            Some(b) => b,
            None => {
                set_error(error_out, GroveDbError::null_pointer("batch"));
                return false;
            }
        };

        // Validate path
        if path.is_null() {
            set_error(error_out, GroveDbError::null_pointer("path"));
            return false;
        }

        // Convert path to owned Vec<Vec<u8>> with validation
        let path_vec = match (*path).to_vec_validated() {
            Ok(v) => v,
            Err(e) => {
                set_error(error_out, e);
                return false;
            }
        };

        // Convert key to owned Vec<u8>
        let key_vec = if key.data.is_null() || key.len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(key.data, key.len).to_vec()
        };

        let tree_type = if is_sum_tree {
            TreeType::SumTree
        } else {
            TreeType::NormalTree
        };

        let op = QualifiedGroveDbOp::delete_tree_op(path_vec, key_vec, tree_type);
        builder.operations.push(op);
        set_ok(error_out);
        true
    })
}

/// Get the number of operations in the batch.
///
/// # Arguments
///
/// * `batch` - Batch handle
///
/// # Returns
///
/// The number of operations, or 0 if batch is null.
#[no_mangle]
pub unsafe extern "C" fn grovedb_batch_count(batch: *const BatchHandle) -> usize {
    if batch.is_null() {
        return 0;
    }
    let builder = match (batch as *const BatchBuilder).as_ref() {
        Some(b) => b,
        None => return 0,
    };
    builder.operations.len()
}

/// Apply a batch of operations atomically.
///
/// This function consumes and frees the batch handle.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `batch` - Batch handle (consumed, do not use after this call)
/// * `transaction` - Optional transaction handle
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// `true` on success, `false` on error.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `batch` must be a valid batch handle and will be freed
#[no_mangle]
pub unsafe extern "C" fn grovedb_apply_batch(
    handle: *mut GroveDbHandle,
    batch: *mut BatchHandle,
    transaction: *const TransactionHandle,
    error_out: *mut GroveDbError,
) -> bool {
    catch_panic(error_out, false, || {
        // Validate database handle
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

        // Take ownership of batch
        let builder = match BatchHandle::into_builder(batch) {
            Some(b) => b,
            None => {
                set_error(error_out, GroveDbError::null_pointer("batch"));
                return false;
            }
        };

        // Get transaction guard
        let tx_guard = if transaction.is_null() {
            None
        } else {
            let tx_id = TransactionHandle::get_id(transaction);
            if let Some(id) = tx_id {
                match wrapper.get_transaction_guard(id) {
                    Some(guard) => Some(guard),
                    None => {
                        set_error(
                            error_out,
                            GroveDbError::new(GroveDbErrorCode::InvalidParameter, "Transaction not found"),
                        );
                        return false;
                    }
                }
            } else {
                None
            }
        };

        // Apply the batch (extract result from CostContext)
        let grove_version = GroveVersion::latest();
        let grovedb_costs::CostContext { value: result, .. } =
            wrapper.db().apply_batch(builder.operations, None, tx_arg_from_guard(&tx_guard), grove_version);

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

/// Free a batch handle without applying it.
///
/// Use this to discard a batch that was started but shouldn't be applied.
///
/// # Arguments
///
/// * `batch` - Batch handle (may be null, which is a no-op)
///
/// # Safety
///
/// * `batch` must have been returned by `grovedb_batch_new()`
/// * `batch` must not be used after this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_batch(batch: *mut BatchHandle) {
    if !batch.is_null() {
        drop(BatchHandle::into_builder(batch));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;
    use std::ffi::CString;
    use crate::{grovedb_open, grovedb_close, grovedb_element_new_tree, grovedb_free_element};
    use crate::types::ByteBuffer;

    fn byte_buffer(data: &[u8]) -> ByteBuffer {
        ByteBuffer {
            data: data.as_ptr(),
            len: data.len(),
        }
    }

    #[test]
    fn test_batch_lifecycle() {
        unsafe {
            let mut error = GroveDbError::default();

            // Create batch
            let batch = grovedb_batch_new(&mut error);
            assert!(!batch.is_null());
            assert!(error.is_ok());

            // Check count
            assert_eq!(grovedb_batch_count(batch), 0);

            // Free batch
            grovedb_free_batch(batch);
        }
    }

    #[test]
    fn test_batch_operations() {
        unsafe {
            let temp_dir = TempDir::new().expect("Failed to create temp dir");
            let path = temp_dir.path().to_str().unwrap();
            let c_path = CString::new(path).unwrap();

            let mut error = GroveDbError::default();

            // Open database
            let db = grovedb_open(c_path.as_ptr(), &mut error);
            assert!(!db.is_null());

            // Create batch
            let batch = grovedb_batch_new(&mut error);
            assert!(!batch.is_null());

            // Add tree insert
            let root_path = GroveDbPath {
                segments: std::ptr::null(),
                count: 0,
            };
            let tree = grovedb_element_new_tree(ByteBuffer { data: std::ptr::null(), len: 0 });
            assert!(!tree.is_null(), "Failed to create tree element");

            let ok = grovedb_batch_add_insert(
                batch,
                &root_path,
                byte_buffer(b"test_tree"),
                tree,
                BatchOpType::InsertOnly,
                &mut error,
            );
            assert!(ok, "Failed to add insert: {:?}", error);

            // Free the tree element - with borrowing semantics, caller must free it
            grovedb_free_element(tree);

            // Check count
            assert_eq!(grovedb_batch_count(batch), 1);

            // Apply batch
            let ok = grovedb_apply_batch(db, batch, std::ptr::null(), &mut error);
            assert!(ok, "Failed to apply batch: {:?}", error);

            // Clean up
            grovedb_close(db);
        }
    }
}
