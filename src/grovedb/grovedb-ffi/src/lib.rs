//! GroveDB C FFI - Foreign Function Interface for GroveDB
//!
//! This crate provides a C-compatible interface to GroveDB, allowing non-Rust
//! languages to use GroveDB through standard FFI mechanisms.
//!
//! # Usage
//!
//! After building this crate, you will have:
//! - A dynamic library (`libgrovedb_ffi.so` / `.dylib` / `.dll`)
//! - A static library (`libgrovedb_ffi.a`)
//! - A C header file (`include/grovedb.h`)
//!
//! # Memory Management
//!
//! The FFI follows these ownership rules:
//! - **Handles** (GroveDbHandle, TransactionHandle, etc.) are owned by Rust
//! - **Input buffers** (ByteBuffer) are borrowed and must remain valid during calls
//! - **Output buffers** (OwnedByteBuffer, elements, results) are owned by the caller
//!   and must be freed using the appropriate `grovedb_free_*` function
//!
//! # Error Handling
//!
//! Functions that can fail accept an `error_out` parameter. On error:
//! - The return value indicates failure (null pointer, false, etc.)
//! - The error details are written to `error_out` if non-null
//! - Error messages must be freed with `grovedb_free_string()`
//!
//! # Thread Safety
//!
//! - The GroveDb handle can be shared across threads (uses internal synchronization)
//! - Transaction handles should be used by a single thread at a time
//! - Query and element handles are not thread-safe
//!
//! # Example (C)
//!
//! ```c
//! #include "grovedb.h"
//!
//! int main() {
//!     grovedb_error_t error = {0};
//!
//!     // Open database
//!     grovedb_t* db = grovedb_open("/path/to/db", &error);
//!     if (!db) {
//!         printf("Error: %s\n", error.message);
//!         grovedb_free_string(error.message);
//!         return 1;
//!     }
//!
//!     // Create a tree at root
//!     byte_buffer_t tree_key = { (uint8_t*)"my_tree", 7 };
//!     grovedb_element_t* tree_elem = grovedb_element_new_tree((byte_buffer_t){NULL, 0});
//!     grovedb_insert(db, NULL, tree_key, tree_elem, NULL, NULL, &error);
//!     grovedb_free_element(tree_elem);
//!
//!     // Insert an item
//!     byte_buffer_t path_seg = { (uint8_t*)"my_tree", 7 };
//!     grovedb_path_t path = { &path_seg, 1 };
//!     byte_buffer_t item_key = { (uint8_t*)"hello", 5 };
//!     byte_buffer_t item_value = { (uint8_t*)"world", 5 };
//!     grovedb_element_t* item = grovedb_element_new_item(item_value, (byte_buffer_t){NULL, 0});
//!     grovedb_insert(db, &path, item_key, item, NULL, NULL, &error);
//!     grovedb_free_element(item);
//!
//!     // Get the item
//!     grovedb_element_t* retrieved = grovedb_get(db, &path, item_key, NULL, &error);
//!     if (retrieved) {
//!         // Use the element...
//!         grovedb_free_element(retrieved);
//!     }
//!
//!     // Get root hash
//!     uint8_t hash[32];
//!     grovedb_root_hash(db, NULL, &hash, &error);
//!
//!     // Close
//!     grovedb_close(db);
//!     return 0;
//! }
//! ```

#![allow(clippy::missing_safety_doc)]

// Module declarations
pub mod batch;
mod database;
pub mod element;
pub mod error;
pub mod handles;
mod memory;
mod panic;
pub mod proof;
pub mod query;
pub mod transaction;
pub mod types;

// Re-export all public FFI functions

// Database lifecycle
pub use database::{
    grovedb_close, grovedb_delete, grovedb_flush, grovedb_get, grovedb_insert, grovedb_open,
    grovedb_root_hash,
};

// Element construction
pub use element::{
    grovedb_element_get_type, grovedb_element_new_big_sum_tree, grovedb_element_new_count_sum_tree,
    grovedb_element_new_count_tree, grovedb_element_new_item, grovedb_element_new_reference,
    grovedb_element_new_sum_item, grovedb_element_new_sum_tree, grovedb_element_new_tree,
    grovedb_free_element, GroveDbElement,
};

// Error handling
pub use error::{GroveDbError, GroveDbErrorCode};

// Memory management
pub use memory::{
    grovedb_free_byte_buffer, grovedb_free_error, grovedb_free_path_owned, grovedb_free_string,
};

// Transaction management
pub use transaction::{
    grovedb_abort_transaction, grovedb_commit_transaction, grovedb_free_transaction,
    grovedb_is_transaction_active, grovedb_rollback_transaction, grovedb_start_transaction,
};

// Query operations
pub use query::{
    grovedb_free_path_query, grovedb_free_query_results, grovedb_path_query_add_key,
    grovedb_path_query_add_range, grovedb_path_query_add_range_after,
    grovedb_path_query_add_range_from, grovedb_path_query_add_range_full,
    grovedb_path_query_add_range_inclusive, grovedb_path_query_new, grovedb_path_query_set_limit,
    grovedb_path_query_set_offset, grovedb_query, grovedb_query_key, grovedb_query_results_count,
    grovedb_query_results_get, QueryResultElement, QueryResults,
};

// Proof operations
pub use proof::{
    grovedb_prove_query, grovedb_prove_query_key, grovedb_verify_query,
    grovedb_verify_query_key, grovedb_verify_query_with_absence_proof,
};

// Type definitions
pub use types::{
    ByteBuffer, DeleteOptions, GroveDbElementType, GroveDbPath, GroveDbPathOwned, InsertOptions,
    OwnedByteBuffer, QueryItemType, ReferencePathTypeTag,
};

// Handle types (opaque)
pub use handles::{GroveDbHandle, PathQueryHandle, TransactionHandle};

// Batch operations
pub use batch::{
    grovedb_apply_batch, grovedb_batch_add_delete, grovedb_batch_add_delete_tree,
    grovedb_batch_add_insert, grovedb_batch_count, grovedb_batch_new, grovedb_free_batch,
    BatchHandle, BatchOpType,
};
