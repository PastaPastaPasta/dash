//! Memory management functions for the GroveDB FFI.
//!
//! All data returned by FFI functions that is owned by the caller must be
//! freed using the appropriate function from this module.

use libc::c_char;
use std::ffi::CString;

use crate::error::GroveDbError;
use crate::types::{GroveDbPathOwned, OwnedByteBuffer};

/// Free a null-terminated string allocated by grovedb-ffi.
///
/// # Safety
///
/// * `s` must have been allocated by grovedb-ffi (e.g., from error messages)
/// * `s` must not be used after this call
/// * It is safe to pass null (no-op)
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

/// Free an owned byte buffer allocated by grovedb-ffi.
///
/// # Safety
///
/// * `buf` must point to a valid OwnedByteBuffer or be null
/// * The buffer must have been allocated by grovedb-ffi
/// * The buffer must not be used after this call
/// * It is safe to pass null (no-op)
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_byte_buffer(buf: *mut OwnedByteBuffer) {
    if !buf.is_null() {
        (*buf).free();
    }
}

/// Free an owned path allocated by grovedb-ffi.
///
/// This frees all segments and the segment array.
///
/// # Safety
///
/// * `path` must point to a valid GroveDbPathOwned or be null
/// * The path must have been allocated by grovedb-ffi
/// * The path must not be used after this call
/// * It is safe to pass null (no-op)
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_path_owned(path: *mut GroveDbPathOwned) {
    if !path.is_null() {
        (*path).free();
    }
}

/// Free an error structure allocated by grovedb-ffi.
///
/// This frees the error message string if present.
///
/// # Safety
///
/// * `error` must point to a valid GroveDbError or be null
/// * The error must not be used after this call
/// * It is safe to pass null (no-op)
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_error(error: *mut GroveDbError) {
    if !error.is_null() {
        (*error).free_message();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;

    #[test]
    fn test_free_string_null() {
        // Should not crash
        unsafe {
            grovedb_free_string(ptr::null_mut());
        }
    }

    #[test]
    fn test_free_string() {
        let s = CString::new("test string").unwrap();
        let ptr = s.into_raw();
        unsafe {
            grovedb_free_string(ptr);
        }
        // If we get here without crashing, the test passes
    }

    #[test]
    fn test_free_byte_buffer_null() {
        unsafe {
            grovedb_free_byte_buffer(ptr::null_mut());
        }
    }

    #[test]
    fn test_free_byte_buffer() {
        let vec = vec![1u8, 2, 3, 4, 5];
        let mut buf = OwnedByteBuffer::from_vec(vec);
        unsafe {
            grovedb_free_byte_buffer(&mut buf);
        }
        assert!(buf.is_null());
    }

    #[test]
    fn test_free_path_owned_null() {
        unsafe {
            grovedb_free_path_owned(ptr::null_mut());
        }
    }

    #[test]
    fn test_free_path_owned() {
        let path = vec![b"tree1".to_vec(), b"subtree".to_vec()];
        let mut owned = GroveDbPathOwned::from_vec(path);
        unsafe {
            grovedb_free_path_owned(&mut owned);
        }
        assert!(owned.segments.is_null());
    }

    #[test]
    fn test_free_error_null() {
        unsafe {
            grovedb_free_error(ptr::null_mut());
        }
    }

    #[test]
    fn test_free_error() {
        use crate::error::GroveDbErrorCode;

        let mut error = GroveDbError::new(GroveDbErrorCode::PathNotFound, "test error");
        assert!(!error.message.is_null());
        unsafe {
            grovedb_free_error(&mut error);
        }
        assert!(error.message.is_null());
    }
}
