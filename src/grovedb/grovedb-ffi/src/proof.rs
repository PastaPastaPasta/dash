//! Proof generation and verification for the GroveDB FFI.
//!
//! This module provides functions for generating and verifying cryptographic
//! proofs in GroveDB.

use grovedb::{GroveDb, PathQuery};
use grovedb_version::version::GroveVersion;

use crate::error::{set_error, set_ok, GroveDbError, GroveDbErrorCode};
use crate::handles::{GroveDbHandle, PathQueryHandle};
use crate::panic::{catch_panic, catch_panic_ptr};
use crate::query::{PathQueryBuilder, QueryResults};
use crate::types::{ByteBuffer, GroveDbPath, OwnedByteBuffer};

/// Generate a proof for a query.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `query` - PathQuery handle (consumed - do not use after this call)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// The serialized proof as a byte buffer, or null buffer on error.
/// Caller must free with `grovedb_free_byte_buffer()`.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `query` is consumed by this call
#[no_mangle]
pub unsafe extern "C" fn grovedb_prove_query(
    handle: *const GroveDbHandle,
    query: *mut PathQueryHandle,
    error_out: *mut GroveDbError,
) -> OwnedByteBuffer {
    catch_panic(error_out, OwnedByteBuffer::null(), || {
        // Validate handle
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return OwnedByteBuffer::null();
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return OwnedByteBuffer::null();
        }

        // Validate query
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return OwnedByteBuffer::null();
        }

        // Take ownership and build the PathQuery
        let builder = Box::from_raw(query as *mut PathQueryBuilder);
        let path_query = builder.build();

        let version = GroveVersion::latest();

        // Generate proof (extract result from CostContext)
        let grovedb_costs::CostContext { value: result, .. } =
            wrapper.db().prove_query(&path_query, None, version);
        match result {
            Ok(proof_bytes) => {
                set_ok(error_out);
                OwnedByteBuffer::from_vec(proof_bytes)
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                OwnedByteBuffer::null()
            }
        }
    })
}

/// Generate a proof for a single-key query.
///
/// This is a convenience function that combines query creation and proof generation.
///
/// # Arguments
///
/// * `handle` - Database handle
/// * `path` - The path to query
/// * `key` - The key to prove
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// The serialized proof as a byte buffer, or null buffer on error.
/// Caller must free with `grovedb_free_byte_buffer()`.
///
/// # Safety
///
/// * `handle` must be a valid database handle
/// * `path` and `key` data must be valid during the call
#[no_mangle]
pub unsafe extern "C" fn grovedb_prove_query_key(
    handle: *const GroveDbHandle,
    path: *const GroveDbPath,
    key: ByteBuffer,
    error_out: *mut GroveDbError,
) -> OwnedByteBuffer {
    catch_panic(error_out, OwnedByteBuffer::null(), || {
        // Validate handle
        let wrapper = match GroveDbHandle::get_wrapper(handle) {
            Some(w) => w,
            None => {
                set_error(error_out, GroveDbError::null_pointer("handle"));
                return OwnedByteBuffer::null();
            }
        };

        if wrapper.is_closed() {
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::DatabaseClosed, "Database has been closed"),
            );
            return OwnedByteBuffer::null();
        }

        // Build path query
        let path_vec = if path.is_null() {
            Vec::new()
        } else {
            match (*path).to_vec_validated() {
                Ok(v) => v,
                Err(e) => {
                    set_error(error_out, e);
                    return OwnedByteBuffer::null();
                }
            }
        };
        let path_query = PathQuery::new_single_key(path_vec, key.to_vec());

        let version = GroveVersion::latest();

        // Generate proof (extract result from CostContext)
        let grovedb_costs::CostContext { value: result, .. } =
            wrapper.db().prove_query(&path_query, None, version);
        match result {
            Ok(proof_bytes) => {
                set_ok(error_out);
                OwnedByteBuffer::from_vec(proof_bytes)
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                OwnedByteBuffer::null()
            }
        }
    })
}

/// Verify a proof against a query.
///
/// This is a static method that does not require a database handle.
/// It verifies that the proof is valid for the given query and returns
/// the root hash and query results.
///
/// # Arguments
///
/// * `proof` - The serialized proof bytes
/// * `path` - The path that was queried
/// * `key` - The key that was queried (for single-key verification)
/// * `root_hash_out` - Pointer to 32-byte buffer to receive the verified root hash
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// Query results if verification succeeds, or null on error.
/// Caller must free with `grovedb_free_query_results()`.
///
/// # Safety
///
/// * `proof` data must be valid during the call
/// * `root_hash_out` must point to at least 32 bytes of writable memory
#[no_mangle]
pub unsafe extern "C" fn grovedb_verify_query_key(
    proof: ByteBuffer,
    path: *const GroveDbPath,
    key: ByteBuffer,
    root_hash_out: *mut [u8; 32],
    error_out: *mut GroveDbError,
) -> *mut QueryResults {
    catch_panic_ptr(error_out, || {
        // Validate proof
        let proof_bytes = match proof.as_slice() {
            Some(p) => p,
            None => {
                set_error(error_out, GroveDbError::null_pointer("proof"));
                return std::ptr::null_mut();
            }
        };

        // Build path query
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

        let version = GroveVersion::latest();

        // Verify proof
        match GroveDb::verify_query(proof_bytes, &path_query, version) {
            Ok((hash, results)) => {
                if !root_hash_out.is_null() {
                    (*root_hash_out).copy_from_slice(&hash);
                }
                set_ok(error_out);
                let query_results = QueryResults::from_path_key_optional_elements(results);
                Box::into_raw(Box::new(query_results))
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Verify a proof against a custom query.
///
/// This verifies a proof using a PathQuery builder that was configured
/// with the same parameters as the original proof generation.
///
/// # Arguments
///
/// * `proof` - The serialized proof bytes
/// * `query` - PathQuery handle (consumed - do not use after this call)
/// * `root_hash_out` - Pointer to 32-byte buffer to receive the verified root hash
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// Query results if verification succeeds, or null on error.
/// Caller must free with `grovedb_free_query_results()`.
///
/// # Safety
///
/// * `proof` data must be valid during the call
/// * `query` is consumed by this call
/// * `root_hash_out` must point to at least 32 bytes of writable memory
#[no_mangle]
pub unsafe extern "C" fn grovedb_verify_query(
    proof: ByteBuffer,
    query: *mut PathQueryHandle,
    root_hash_out: *mut [u8; 32],
    error_out: *mut GroveDbError,
) -> *mut QueryResults {
    catch_panic_ptr(error_out, || {
        // Validate proof
        let proof_bytes = match proof.as_slice() {
            Some(p) => p,
            None => {
                set_error(error_out, GroveDbError::null_pointer("proof"));
                return std::ptr::null_mut();
            }
        };

        // Validate query
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return std::ptr::null_mut();
        }

        // Take ownership and build the PathQuery
        let builder = Box::from_raw(query as *mut PathQueryBuilder);
        let path_query = builder.build();

        let version = GroveVersion::latest();

        // Verify proof
        match GroveDb::verify_query(proof_bytes, &path_query, version) {
            Ok((hash, results)) => {
                if !root_hash_out.is_null() {
                    (*root_hash_out).copy_from_slice(&hash);
                }
                set_ok(error_out);
                let query_results = QueryResults::from_path_key_optional_elements(results);
                Box::into_raw(Box::new(query_results))
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Verify a proof with absence proof support.
///
/// This verifies a proof that may include proofs of absence for non-existing keys.
///
/// # Arguments
///
/// * `proof` - The serialized proof bytes
/// * `query` - PathQuery handle (consumed - do not use after this call)
/// * `root_hash_out` - Pointer to 32-byte buffer to receive the verified root hash
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// Query results if verification succeeds, or null on error.
/// Caller must free with `grovedb_free_query_results()`.
///
/// # Safety
///
/// * `proof` data must be valid during the call
/// * `query` is consumed by this call
/// * `root_hash_out` must point to at least 32 bytes of writable memory
#[no_mangle]
pub unsafe extern "C" fn grovedb_verify_query_with_absence_proof(
    proof: ByteBuffer,
    query: *mut PathQueryHandle,
    root_hash_out: *mut [u8; 32],
    error_out: *mut GroveDbError,
) -> *mut QueryResults {
    catch_panic_ptr(error_out, || {
        // Validate proof
        let proof_bytes = match proof.as_slice() {
            Some(p) => p,
            None => {
                set_error(error_out, GroveDbError::null_pointer("proof"));
                return std::ptr::null_mut();
            }
        };

        // Validate query
        if query.is_null() {
            set_error(error_out, GroveDbError::null_pointer("query"));
            return std::ptr::null_mut();
        }

        // Take ownership and build the PathQuery
        let builder = Box::from_raw(query as *mut PathQueryBuilder);
        let path_query = builder.build();

        let version = GroveVersion::latest();

        // Verify proof with absence support
        match GroveDb::verify_query_with_absence_proof(proof_bytes, &path_query, version) {
            Ok((hash, results)) => {
                if !root_hash_out.is_null() {
                    (*root_hash_out).copy_from_slice(&hash);
                }
                set_ok(error_out);
                let query_results = QueryResults::from_path_key_optional_elements(results);
                Box::into_raw(Box::new(query_results))
            }
            Err(e) => {
                set_error(error_out, GroveDbError::from_grovedb_error(e));
                std::ptr::null_mut()
            }
        }
    })
}
