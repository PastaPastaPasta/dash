//! Panic handling utilities for the FFI boundary.
//!
//! This module provides utilities to catch Rust panics at the FFI boundary
//! and convert them to error returns, preventing undefined behavior when
//! panics would otherwise unwind across FFI boundaries.

use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::error::{set_error, GroveDbError, GroveDbErrorCode};

/// Catch panics and convert them to FFI errors.
///
/// This function wraps a closure that may panic, catches the panic, and
/// converts it to a GroveDbError that can be returned across the FFI boundary.
///
/// # Arguments
///
/// * `error_out` - Pointer to write error details (may be null)
/// * `default` - Default value to return on panic
/// * `f` - The function to execute
///
/// # Returns
///
/// The result of `f` on success, or `default` on panic.
///
/// # Example
///
/// ```ignore
/// pub unsafe extern "C" fn my_ffi_function(
///     error_out: *mut GroveDbError,
/// ) -> bool {
///     catch_panic(error_out, false, || {
///         // Do some work that might panic
///         true
///     })
/// }
/// ```
pub fn catch_panic<T, F>(error_out: *mut GroveDbError, default: T, f: F) -> T
where
    F: FnOnce() -> T,
{
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(panic) => {
            let message = panic_to_message(panic);
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::InternalError, format!("panic: {}", message)),
            );
            default
        }
    }
}

/// Catch panics and convert them to FFI errors, with a Result return.
///
/// Similar to `catch_panic`, but for functions that return a Result-like
/// value where we want to distinguish between panic and regular errors.
///
/// # Arguments
///
/// * `error_out` - Pointer to write error details (may be null)
/// * `default` - Default value to return on panic or error
/// * `f` - The function to execute, returns Result<T, GroveDbError>
///
/// # Returns
///
/// The Ok value on success, or `default` on panic or error.
#[allow(dead_code)] // Utility function available for future FFI implementations
pub fn catch_panic_result<T, F>(error_out: *mut GroveDbError, default: T, f: F) -> T
where
    F: FnOnce() -> Result<T, GroveDbError>,
{
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(result)) => result,
        Ok(Err(err)) => {
            set_error(error_out, err);
            default
        }
        Err(panic) => {
            let message = panic_to_message(panic);
            set_error(
                error_out,
                GroveDbError::new(GroveDbErrorCode::InternalError, format!("panic: {}", message)),
            );
            default
        }
    }
}

/// Catch panics for functions that return a pointer.
///
/// Convenience wrapper for functions returning pointers that should return
/// null on error.
///
/// # Arguments
///
/// * `error_out` - Pointer to write error details (may be null)
/// * `f` - The function to execute
///
/// # Returns
///
/// The pointer on success, or null on panic.
pub fn catch_panic_ptr<T, F>(error_out: *mut GroveDbError, f: F) -> *mut T
where
    F: FnOnce() -> *mut T,
{
    catch_panic(error_out, std::ptr::null_mut(), f)
}

/// Catch panics for functions that return bool success.
///
/// Convenience wrapper for functions returning bool where false indicates error.
///
/// # Arguments
///
/// * `error_out` - Pointer to write error details (may be null)
/// * `f` - The function to execute
///
/// # Returns
///
/// true on success, false on panic.
#[allow(dead_code)] // Utility function available for future FFI implementations
pub fn catch_panic_bool<F>(error_out: *mut GroveDbError, f: F) -> bool
where
    F: FnOnce() -> bool,
{
    catch_panic(error_out, false, f)
}

/// Extract a message from a panic payload.
fn panic_to_message(panic: Box<dyn std::any::Any + Send + 'static>) -> String {
    if let Some(s) = panic.downcast_ref::<&str>() {
        s.to_string()
    } else if let Some(s) = panic.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic".to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;

    #[test]
    fn test_catch_panic_success() {
        let mut error = GroveDbError::default();
        let result = catch_panic(&mut error, 0, || 42);
        assert_eq!(result, 42);
        assert!(error.is_ok());
    }

    #[test]
    fn test_catch_panic_panic() {
        let mut error = GroveDbError::default();
        let result = catch_panic(&mut error, -1, || {
            panic!("test panic");
            #[allow(unreachable_code)]
            42
        });
        assert_eq!(result, -1);
        assert!(!error.is_ok());
        assert_eq!(error.code, GroveDbErrorCode::InternalError);

        // Clean up the error message
        unsafe {
            error.free_message();
        }
    }

    #[test]
    fn test_catch_panic_null_error_out() {
        let result = catch_panic(ptr::null_mut(), -1, || {
            panic!("test panic");
            #[allow(unreachable_code)]
            42
        });
        assert_eq!(result, -1);
        // Should not crash even with null error_out
    }

    #[test]
    fn test_catch_panic_result_success() {
        let mut error = GroveDbError::default();
        let result: i32 = catch_panic_result(&mut error, -1, || Ok(42));
        assert_eq!(result, 42);
        assert!(error.is_ok());
    }

    #[test]
    fn test_catch_panic_result_error() {
        let mut error = GroveDbError::default();
        let result: i32 = catch_panic_result(&mut error, -1, || {
            Err(GroveDbError::new(GroveDbErrorCode::PathNotFound, "test"))
        });
        assert_eq!(result, -1);
        assert_eq!(error.code, GroveDbErrorCode::PathNotFound);

        // Clean up
        unsafe {
            error.free_message();
        }
    }

    #[test]
    fn test_catch_panic_ptr_success() {
        let mut error = GroveDbError::default();
        let value = Box::new(42i32);
        let ptr = Box::into_raw(value);

        let result = catch_panic_ptr(&mut error, || ptr);
        assert_eq!(result, ptr);
        assert!(error.is_ok());

        // Clean up
        unsafe {
            drop(Box::from_raw(result));
        }
    }

    #[test]
    fn test_catch_panic_ptr_panic() {
        let mut error = GroveDbError::default();
        let result: *mut i32 = catch_panic_ptr(&mut error, || {
            panic!("test panic");
            #[allow(unreachable_code)]
            std::ptr::null_mut()
        });
        assert!(result.is_null());
        assert!(!error.is_ok());

        // Clean up
        unsafe {
            error.free_message();
        }
    }
}
