//! Integration tests for the GroveDB FFI layer.
//!
//! These tests verify that the FFI functions work correctly end-to-end,
//! including proper memory management and error handling.

#![allow(non_camel_case_types)]

use std::ffi::CString;
use std::ptr;
use tempfile::TempDir;

// Import the FFI types and functions
use grovedb_ffi::handles::{GroveDbHandle, PathQueryHandle, TransactionHandle};
use grovedb_ffi::*;

// Type aliases to match C naming convention for clarity
type grovedb_t = GroveDbHandle;
#[allow(dead_code)]
type grovedb_transaction_t = TransactionHandle;
#[allow(dead_code)]
type grovedb_path_query_t = PathQueryHandle;

/// Helper to create a test database in a temporary directory
fn open_test_db() -> (TempDir, *mut grovedb_t) {
    let temp_dir = TempDir::new().expect("Failed to create temp dir");
    let path = temp_dir.path().to_str().unwrap();
    let c_path = CString::new(path).unwrap();

    let mut error = GroveDbError::default();
    let db = unsafe { grovedb_open(c_path.as_ptr(), &mut error) };

    assert!(!db.is_null(), "Failed to open database: {:?}", error);
    assert!(error.is_ok(), "Error opening database");

    (temp_dir, db)
}

/// Helper to create a ByteBuffer from a slice
fn byte_buffer(data: &[u8]) -> ByteBuffer {
    ByteBuffer {
        data: data.as_ptr(),
        len: data.len(),
    }
}

/// Helper to create an empty ByteBuffer
fn empty_buffer() -> ByteBuffer {
    ByteBuffer {
        data: ptr::null(),
        len: 0,
    }
}

/// Helper to create a path from segments
fn make_path(segments: &[&[u8]]) -> (Vec<ByteBuffer>, GroveDbPath) {
    let buffers: Vec<ByteBuffer> = segments.iter().map(|s| byte_buffer(s)).collect();
    let path = GroveDbPath {
        segments: buffers.as_ptr(),
        count: buffers.len(),
    };
    (buffers, path)
}

#[test]
fn test_database_lifecycle() {
    let temp_dir = TempDir::new().expect("Failed to create temp dir");
    let path = temp_dir.path().to_str().unwrap();
    let c_path = CString::new(path).unwrap();

    let mut error = GroveDbError::default();

    // Open database
    let db = unsafe { grovedb_open(c_path.as_ptr(), &mut error) };
    assert!(!db.is_null());
    assert!(error.is_ok());

    // Flush
    let flush_ok = unsafe { grovedb_flush(db, &mut error) };
    assert!(flush_ok);
    assert!(error.is_ok());

    // Close database
    unsafe { grovedb_close(db) };

    // Reopen database
    let db2 = unsafe { grovedb_open(c_path.as_ptr(), &mut error) };
    assert!(!db2.is_null());
    assert!(error.is_ok());

    unsafe { grovedb_close(db2) };
}

#[test]
fn test_root_hash() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Get root hash (should be all zeros for empty db)
    let mut hash = [0u8; 32];
    let ok = unsafe { grovedb_root_hash(db, ptr::null(), &mut hash, &mut error) };
    assert!(ok);
    assert!(error.is_ok());

    unsafe { grovedb_close(db) };
}

#[test]
fn test_insert_and_get_item() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree at root
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    assert!(!tree.is_null());

    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };
    let tree_key = byte_buffer(b"users");

    let ok = unsafe {
        grovedb_insert(db, &root_path, tree_key, tree, ptr::null(), ptr::null(), &mut error)
    };
    assert!(ok, "Failed to insert tree: {:?}", error);
    unsafe { grovedb_free_element(tree) };

    // Insert an item in the tree
    let item_data = b"Alice Smith";
    let item = unsafe { grovedb_element_new_item(byte_buffer(item_data), empty_buffer()) };
    assert!(!item.is_null());

    let (path_bufs, users_path) = make_path(&[b"users"]);
    let alice_key = byte_buffer(b"alice");

    let ok = unsafe {
        grovedb_insert(db, &users_path, alice_key, item, ptr::null(), ptr::null(), &mut error)
    };
    assert!(ok, "Failed to insert item: {:?}", error);
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Get the item back
    let (path_bufs, users_path) = make_path(&[b"users"]);
    let alice_key = byte_buffer(b"alice");

    let result = unsafe { grovedb_get(db, &users_path, alice_key, ptr::null(), &mut error) };
    drop(path_bufs);

    assert!(!result.is_null(), "Get returned null: {:?}", error);
    assert!(error.is_ok());

    // Verify the item data
    unsafe {
        let elem = &*result;
        assert_eq!(elem.element_type, GroveDbElementType::Item);
        assert_eq!(elem.value.len, item_data.len());

        let returned_data = std::slice::from_raw_parts(elem.value.data, elem.value.len);
        assert_eq!(returned_data, item_data);

        grovedb_free_element(result);
    }

    unsafe { grovedb_close(db) };
}

#[test]
fn test_insert_and_get_sum_item() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a sum tree at root
    let sum_tree = unsafe { grovedb_element_new_sum_tree(empty_buffer()) };
    assert!(!sum_tree.is_null());

    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    let ok = unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"scores"),
            sum_tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    assert!(ok, "Failed to insert sum tree: {:?}", error);
    unsafe { grovedb_free_element(sum_tree) };

    // Insert sum items
    let (path_bufs, scores_path) = make_path(&[b"scores"]);

    let sum_item = unsafe { grovedb_element_new_sum_item(100, empty_buffer()) };
    assert!(!sum_item.is_null());

    let ok = unsafe {
        grovedb_insert(
            db,
            &scores_path,
            byte_buffer(b"alice"),
            sum_item,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    assert!(ok, "Failed to insert sum item: {:?}", error);
    drop(path_bufs);
    unsafe { grovedb_free_element(sum_item) };

    // Get the sum item back
    let (path_bufs, scores_path) = make_path(&[b"scores"]);
    let result = unsafe { grovedb_get(db, &scores_path, byte_buffer(b"alice"), ptr::null(), &mut error) };
    drop(path_bufs);

    assert!(!result.is_null(), "Get returned null: {:?}", error);

    unsafe {
        let elem = &*result;
        assert_eq!(elem.element_type, GroveDbElementType::SumItem);
        assert_eq!(elem.sum_value, 100);
        grovedb_free_element(result);
    }

    unsafe { grovedb_close(db) };
}

#[test]
fn test_delete() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"data"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Insert an item
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"value"), empty_buffer()) };

    unsafe {
        grovedb_insert(
            db,
            &data_path,
            byte_buffer(b"key"),
            item,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Verify it exists
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let result = unsafe { grovedb_get(db, &data_path, byte_buffer(b"key"), ptr::null(), &mut error) };
    assert!(!result.is_null());
    unsafe { grovedb_free_element(result) };
    drop(path_bufs);

    // Delete it
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let ok = unsafe {
        grovedb_delete(
            db,
            &data_path,
            byte_buffer(b"key"),
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    assert!(ok, "Delete failed: {:?}", error);
    drop(path_bufs);

    // Verify it's gone
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let result = unsafe { grovedb_get(db, &data_path, byte_buffer(b"key"), ptr::null(), &mut error) };
    drop(path_bufs);

    // Should either be null or have PathKeyNotFound error
    if !result.is_null() {
        unsafe { grovedb_free_element(result) };
    }
    // Error code should be PathKeyNotFound
    assert!(
        error.code == GroveDbErrorCode::PathKeyNotFound || result.is_null(),
        "Expected PathKeyNotFound error"
    );

    unsafe { grovedb_close(db) };
}

#[test]
fn test_transaction_commit() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree first (outside transaction)
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"data"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Start transaction
    let tx = unsafe { grovedb_start_transaction(db, &mut error) };
    assert!(!tx.is_null(), "Failed to start transaction: {:?}", error);

    // Insert within transaction
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"tx_value"), empty_buffer()) };

    let ok = unsafe {
        grovedb_insert(
            db,
            &data_path,
            byte_buffer(b"tx_key"),
            item,
            ptr::null(),
            tx,
            &mut error,
        )
    };
    assert!(ok, "Insert in transaction failed: {:?}", error);
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Commit transaction
    let ok = unsafe { grovedb_commit_transaction(db, tx, &mut error) };
    assert!(ok, "Commit failed: {:?}", error);

    // Verify data persisted
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let result =
        unsafe { grovedb_get(db, &data_path, byte_buffer(b"tx_key"), ptr::null(), &mut error) };
    drop(path_bufs);

    assert!(!result.is_null(), "Data not persisted after commit");
    unsafe { grovedb_free_element(result) };

    unsafe { grovedb_close(db) };
}

#[test]
fn test_transaction_rollback() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree first
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"data"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Start transaction
    let tx = unsafe { grovedb_start_transaction(db, &mut error) };
    assert!(!tx.is_null());

    // Insert within transaction
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"rollback_value"), empty_buffer()) };

    unsafe {
        grovedb_insert(
            db,
            &data_path,
            byte_buffer(b"rollback_key"),
            item,
            ptr::null(),
            tx,
            &mut error,
        )
    };
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Rollback transaction
    let ok = unsafe { grovedb_rollback_transaction(db, tx, &mut error) };
    assert!(ok, "Rollback failed: {:?}", error);

    // Free the transaction handle after rollback
    unsafe { grovedb_free_transaction(tx) };

    // Verify data was not persisted
    let (path_bufs, data_path) = make_path(&[b"data"]);
    error = GroveDbError::default();
    let result = unsafe {
        grovedb_get(
            db,
            &data_path,
            byte_buffer(b"rollback_key"),
            ptr::null(),
            &mut error,
        )
    };
    drop(path_bufs);

    // Should not exist
    if !result.is_null() {
        unsafe { grovedb_free_element(result) };
        panic!("Data should not exist after rollback");
    }

    unsafe { grovedb_close(db) };
}

#[test]
fn test_query_range_full() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"items"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Insert some items
    let (path_bufs, items_path) = make_path(&[b"items"]);
    for i in 0..5 {
        let key = format!("key{}", i);
        let value = format!("value{}", i);
        let item = unsafe { grovedb_element_new_item(byte_buffer(value.as_bytes()), empty_buffer()) };

        unsafe {
            grovedb_insert(
                db,
                &items_path,
                byte_buffer(key.as_bytes()),
                item,
                ptr::null(),
                ptr::null(),
                &mut error,
            )
        };
        unsafe { grovedb_free_element(item) };
    }
    drop(path_bufs);

    // Create a query for all items
    let (path_bufs, items_path) = make_path(&[b"items"]);
    let query = unsafe { grovedb_path_query_new(&items_path, &mut error) };
    drop(path_bufs);
    assert!(!query.is_null(), "Failed to create query: {:?}", error);

    // Add full range
    let ok = unsafe { grovedb_path_query_add_range_full(query, &mut error) };
    assert!(ok);

    // Execute query
    let results = unsafe { grovedb_query(db, query, ptr::null(), &mut error) };
    assert!(!results.is_null(), "Query failed: {:?}", error);

    // Check results
    let count = unsafe { grovedb_query_results_count(results) };
    assert_eq!(count, 5, "Expected 5 results, got {}", count);

    unsafe {
        grovedb_free_query_results(results);
        grovedb_close(db);
    }
}

#[test]
fn test_error_on_invalid_path() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Try to get from a non-existent path
    let (path_bufs, bad_path) = make_path(&[b"nonexistent", b"path"]);
    let result = unsafe { grovedb_get(db, &bad_path, byte_buffer(b"key"), ptr::null(), &mut error) };
    drop(path_bufs);

    // Should return null and set error
    assert!(result.is_null());
    assert!(!error.is_ok());
    assert!(
        error.code == GroveDbErrorCode::PathNotFound
            || error.code == GroveDbErrorCode::PathParentLayerNotFound,
        "Expected PathNotFound error, got {:?}",
        error.code
    );

    // Clean up error message
    unsafe {
        if !error.message.is_null() {
            grovedb_free_string(error.message);
        }
        grovedb_close(db);
    }
}

#[test]
fn test_element_types() {
    // Test creating different element types
    unsafe {
        // Item
        let item = grovedb_element_new_item(byte_buffer(b"data"), empty_buffer());
        assert!(!item.is_null());
        assert_eq!(grovedb_element_get_type(item), GroveDbElementType::Item);
        grovedb_free_element(item);

        // Tree
        let tree = grovedb_element_new_tree(empty_buffer());
        assert!(!tree.is_null());
        assert_eq!(grovedb_element_get_type(tree), GroveDbElementType::Tree);
        grovedb_free_element(tree);

        // SumItem
        let sum_item = grovedb_element_new_sum_item(42, empty_buffer());
        assert!(!sum_item.is_null());
        assert_eq!(
            grovedb_element_get_type(sum_item),
            GroveDbElementType::SumItem
        );
        grovedb_free_element(sum_item);

        // SumTree
        let sum_tree = grovedb_element_new_sum_tree(empty_buffer());
        assert!(!sum_tree.is_null());
        assert_eq!(
            grovedb_element_get_type(sum_tree),
            GroveDbElementType::SumTree
        );
        grovedb_free_element(sum_tree);

        // BigSumTree
        let big_sum_tree = grovedb_element_new_big_sum_tree(empty_buffer());
        assert!(!big_sum_tree.is_null());
        assert_eq!(
            grovedb_element_get_type(big_sum_tree),
            GroveDbElementType::BigSumTree
        );
        grovedb_free_element(big_sum_tree);

        // CountTree
        let count_tree = grovedb_element_new_count_tree(empty_buffer());
        assert!(!count_tree.is_null());
        assert_eq!(
            grovedb_element_get_type(count_tree),
            GroveDbElementType::CountTree
        );
        grovedb_free_element(count_tree);

        // CountSumTree
        let count_sum_tree = grovedb_element_new_count_sum_tree(empty_buffer());
        assert!(!count_sum_tree.is_null());
        assert_eq!(
            grovedb_element_get_type(count_sum_tree),
            GroveDbElementType::CountSumTree
        );
        grovedb_free_element(count_sum_tree);
    }
}

#[test]
fn test_null_error_out() {
    let temp_dir = TempDir::new().expect("Failed to create temp dir");
    let path = temp_dir.path().to_str().unwrap();
    let c_path = CString::new(path).unwrap();

    // Open with null error_out should not crash
    let db = unsafe { grovedb_open(c_path.as_ptr(), ptr::null_mut()) };
    assert!(!db.is_null());

    // Other operations with null error_out
    let mut hash = [0u8; 32];
    unsafe {
        grovedb_root_hash(db, ptr::null(), &mut hash, ptr::null_mut());
        grovedb_flush(db, ptr::null_mut());
        grovedb_close(db);
    }
}

#[test]
fn test_prove_and_verify_key() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"data"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Insert an item
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"test_value"), empty_buffer()) };

    unsafe {
        grovedb_insert(
            db,
            &data_path,
            byte_buffer(b"key1"),
            item,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Generate proof
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let proof = unsafe { grovedb_prove_query_key(db, &data_path, byte_buffer(b"key1"), &mut error) };
    drop(path_bufs);

    assert!(!proof.data.is_null(), "Proof generation failed: {:?}", error);
    assert!(proof.len > 0, "Proof should not be empty");

    // Verify proof
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let mut root_hash = [0u8; 32];
    let results = unsafe {
        grovedb_verify_query_key(
            ByteBuffer {
                data: proof.data,
                len: proof.len,
            },
            &data_path,
            byte_buffer(b"key1"),
            &mut root_hash,
            &mut error,
        )
    };
    drop(path_bufs);

    assert!(!results.is_null(), "Verification failed: {:?}", error);

    // Check we got results
    let count = unsafe { grovedb_query_results_count(results) };
    assert_eq!(count, 1, "Expected 1 result, got {}", count);

    unsafe {
        grovedb_free_query_results(results);
        grovedb_free_byte_buffer(&mut OwnedByteBuffer {
            data: proof.data as *mut u8,
            len: proof.len,
            capacity: proof.capacity,
        });
        grovedb_close(db);
    }
}

#[test]
fn test_proof_root_hash_matches_db() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"data"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Insert an item
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"value"), empty_buffer()) };

    unsafe {
        grovedb_insert(
            db,
            &data_path,
            byte_buffer(b"key"),
            item,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Get database root hash
    let mut db_hash = [0u8; 32];
    let ok = unsafe { grovedb_root_hash(db, ptr::null(), &mut db_hash, &mut error) };
    assert!(ok, "Failed to get root hash");

    // Generate proof
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let proof = unsafe { grovedb_prove_query_key(db, &data_path, byte_buffer(b"key"), &mut error) };
    drop(path_bufs);
    assert!(!proof.data.is_null());

    // Verify proof and get root hash from proof
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let mut proof_hash = [0u8; 32];
    let results = unsafe {
        grovedb_verify_query_key(
            ByteBuffer {
                data: proof.data,
                len: proof.len,
            },
            &data_path,
            byte_buffer(b"key"),
            &mut proof_hash,
            &mut error,
        )
    };
    drop(path_bufs);
    assert!(!results.is_null());

    // Compare hashes
    assert_eq!(db_hash, proof_hash, "Root hashes should match");

    unsafe {
        grovedb_free_query_results(results);
        grovedb_free_byte_buffer(&mut OwnedByteBuffer {
            data: proof.data as *mut u8,
            len: proof.len,
            capacity: proof.capacity,
        });
        grovedb_close(db);
    }
}

#[test]
fn test_reference_element() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create target tree
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"target"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Insert item in target
    let (path_bufs, target_path) = make_path(&[b"target"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"original_value"), empty_buffer()) };

    unsafe {
        grovedb_insert(
            db,
            &target_path,
            byte_buffer(b"item"),
            item,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Create reference to target/item
    let (ref_path_bufs, ref_target_path) = make_path(&[b"target", b"item"]);
    let reference = unsafe { grovedb_element_new_reference(&ref_target_path, 0, empty_buffer(), &mut error) };
    drop(ref_path_bufs);

    assert!(!reference.is_null(), "Failed to create reference: {:?}", error);
    assert!(error.is_ok());

    // Verify reference type
    let ref_type = unsafe { grovedb_element_get_type(reference) };
    assert_eq!(ref_type, GroveDbElementType::Reference);

    // Create refs tree
    let refs_tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"refs"),
            refs_tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(refs_tree) };

    // Insert reference
    let (path_bufs, refs_path) = make_path(&[b"refs"]);
    unsafe {
        grovedb_insert(
            db,
            &refs_path,
            byte_buffer(b"myref"),
            reference,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    drop(path_bufs);
    assert!(error.is_ok(), "Failed to insert reference: {:?}", error);
    unsafe { grovedb_free_element(reference) };

    // Get through reference - should resolve to target's value
    let (path_bufs, refs_path) = make_path(&[b"refs"]);
    let result = unsafe { grovedb_get(db, &refs_path, byte_buffer(b"myref"), ptr::null(), &mut error) };
    drop(path_bufs);

    assert!(!result.is_null(), "Get through reference failed: {:?}", error);

    // Should resolve to the item's value
    unsafe {
        let elem = &*result;
        assert_eq!(elem.element_type, GroveDbElementType::Item);
        let returned_data = std::slice::from_raw_parts(elem.value.data, elem.value.len);
        assert_eq!(returned_data, b"original_value");
        grovedb_free_element(result);
    }

    unsafe { grovedb_close(db) };
}

#[test]
fn test_query_result_values() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"items"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Insert items with specific values
    let (path_bufs, items_path) = make_path(&[b"items"]);
    let values = [
        ("key1", "first_value"),
        ("key2", "second_value"),
        ("key3", "third_value"),
    ];

    for (key, value) in values.iter() {
        let item = unsafe { grovedb_element_new_item(byte_buffer(value.as_bytes()), empty_buffer()) };
        unsafe {
            grovedb_insert(
                db,
                &items_path,
                byte_buffer(key.as_bytes()),
                item,
                ptr::null(),
                ptr::null(),
                &mut error,
            )
        };
        unsafe { grovedb_free_element(item) };
    }
    drop(path_bufs);

    // Create query
    let (path_bufs, items_path) = make_path(&[b"items"]);
    let query = unsafe { grovedb_path_query_new(&items_path, &mut error) };
    drop(path_bufs);
    assert!(!query.is_null());

    // Add full range
    unsafe { grovedb_path_query_add_range_full(query, &mut error) };

    // Execute query
    let results = unsafe { grovedb_query(db, query, ptr::null(), &mut error) };
    assert!(!results.is_null(), "Query failed: {:?}", error);

    // Check count
    let count = unsafe { grovedb_query_results_count(results) };
    assert_eq!(count, 3);

    // Verify each result's value
    unsafe {
        for i in 0..count {
            let result_elem = grovedb_query_results_get(results, i);
            assert!(!result_elem.is_null());

            if (*result_elem).has_element && !(*result_elem).element.is_null() {
                let elem = &*(*result_elem).element;
                let key_slice = std::slice::from_raw_parts((*result_elem).key.data, (*result_elem).key.len);
                let value_slice = std::slice::from_raw_parts(elem.value.data, elem.value.len);

                // Verify correct key-value pairs (results are in key order)
                match i {
                    0 => {
                        assert_eq!(key_slice, b"key1");
                        assert_eq!(value_slice, b"first_value");
                    }
                    1 => {
                        assert_eq!(key_slice, b"key2");
                        assert_eq!(value_slice, b"second_value");
                    }
                    2 => {
                        assert_eq!(key_slice, b"key3");
                        assert_eq!(value_slice, b"third_value");
                    }
                    _ => panic!("Unexpected result index"),
                }
            }
        }

        grovedb_free_query_results(results);
        grovedb_close(db);
    }
}

#[test]
fn test_query_ordering() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"sorted"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Insert items in random order
    let (path_bufs, sorted_path) = make_path(&[b"sorted"]);
    let keys: &[&[u8]] = &[b"cherry", b"apple", b"banana", b"date"];

    for key in keys.iter() {
        let item = unsafe { grovedb_element_new_item(byte_buffer(*key), empty_buffer()) };
        unsafe {
            grovedb_insert(
                db,
                &sorted_path,
                byte_buffer(*key),
                item,
                ptr::null(),
                ptr::null(),
                &mut error,
            )
        };
        unsafe { grovedb_free_element(item) };
    }
    drop(path_bufs);

    // Query all
    let (path_bufs, sorted_path) = make_path(&[b"sorted"]);
    let query = unsafe { grovedb_path_query_new(&sorted_path, &mut error) };
    drop(path_bufs);

    unsafe { grovedb_path_query_add_range_full(query, &mut error) };

    let results = unsafe { grovedb_query(db, query, ptr::null(), &mut error) };
    assert!(!results.is_null());

    // Verify results are in sorted order
    let count = unsafe { grovedb_query_results_count(results) };
    assert_eq!(count, 4);

    let expected_order: &[&[u8]] = &[b"apple", b"banana", b"cherry", b"date"];

    unsafe {
        for (i, expected_key) in expected_order.iter().enumerate() {
            let result_elem = grovedb_query_results_get(results, i);
            assert!(!result_elem.is_null());

            let key_slice = std::slice::from_raw_parts((*result_elem).key.data, (*result_elem).key.len);
            assert_eq!(
                key_slice, *expected_key,
                "Key at position {} is incorrect: expected {:?}, got {:?}",
                i,
                std::str::from_utf8(expected_key).unwrap(),
                std::str::from_utf8(key_slice).unwrap()
            );
        }

        grovedb_free_query_results(results);
        grovedb_close(db);
    }
}

/// Regression test: Commit after rollback should fail.
///
/// This test verifies that after a transaction is rolled back, attempting to
/// commit it fails gracefully (transaction should no longer exist in the HashMap).
#[test]
fn test_commit_after_rollback_fails() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree first
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"data"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Start transaction
    let tx = unsafe { grovedb_start_transaction(db, &mut error) };
    assert!(!tx.is_null());
    assert!(error.is_ok());

    // Insert within transaction
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"value"), empty_buffer()) };

    unsafe {
        grovedb_insert(
            db,
            &data_path,
            byte_buffer(b"key"),
            item,
            ptr::null(),
            tx,
            &mut error,
        )
    };
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Rollback transaction
    let ok = unsafe { grovedb_rollback_transaction(db, tx, &mut error) };
    assert!(ok, "Rollback should succeed: {:?}", error);

    // Transaction should no longer be active after rollback
    let is_active = unsafe { grovedb_is_transaction_active(db, tx) };
    assert!(!is_active, "Transaction should not be active after rollback");

    // Attempt to commit after rollback should fail
    error = GroveDbError::default();
    let commit_ok = unsafe { grovedb_commit_transaction(db, tx, &mut error) };
    assert!(!commit_ok, "Commit after rollback should fail");
    assert!(!error.is_ok(), "Should have an error");
    // Transaction not found is an internal error
    assert_eq!(
        error.code,
        GroveDbErrorCode::InternalError,
        "Expected InternalError for transaction not found, got {:?}",
        error.code
    );

    // Clean up error message if any
    unsafe {
        if !error.message.is_null() {
            grovedb_free_string(error.message);
        }
    }

    // Free the transaction handle (should be safe even after rollback)
    unsafe { grovedb_free_transaction(tx) };

    unsafe { grovedb_close(db) };
}

/// Regression test: Operations using a rolled-back transaction should fail.
///
/// This test verifies that after a transaction is rolled back, attempting to
/// use it for database operations fails gracefully.
#[test]
fn test_operations_after_rollback_fail() {
    let (_temp_dir, db) = open_test_db();
    let mut error = GroveDbError::default();

    // Create a tree first
    let tree = unsafe { grovedb_element_new_tree(empty_buffer()) };
    let root_path = GroveDbPath {
        segments: ptr::null(),
        count: 0,
    };

    unsafe {
        grovedb_insert(
            db,
            &root_path,
            byte_buffer(b"data"),
            tree,
            ptr::null(),
            ptr::null(),
            &mut error,
        )
    };
    unsafe { grovedb_free_element(tree) };

    // Start transaction and rollback
    let tx = unsafe { grovedb_start_transaction(db, &mut error) };
    assert!(!tx.is_null());

    let ok = unsafe { grovedb_rollback_transaction(db, tx, &mut error) };
    assert!(ok, "Rollback should succeed");

    // Try to use the rolled-back transaction for an insert
    let (path_bufs, data_path) = make_path(&[b"data"]);
    let item = unsafe { grovedb_element_new_item(byte_buffer(b"value"), empty_buffer()) };

    error = GroveDbError::default();
    let insert_ok = unsafe {
        grovedb_insert(
            db,
            &data_path,
            byte_buffer(b"key"),
            item,
            ptr::null(),
            tx,
            &mut error,
        )
    };
    drop(path_bufs);
    unsafe { grovedb_free_element(item) };

    // Insert with rolled-back transaction should fail
    // (transaction ID no longer in HashMap after rollback)
    assert!(!insert_ok, "Insert with rolled-back transaction should fail");
    assert!(!error.is_ok(), "Should have an error");

    // Clean up
    unsafe {
        if !error.message.is_null() {
            grovedb_free_string(error.message);
        }
        grovedb_free_transaction(tx);
        grovedb_close(db);
    }
}
