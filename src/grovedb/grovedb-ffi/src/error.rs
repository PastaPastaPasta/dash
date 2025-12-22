//! Error handling for the GroveDB FFI.
//!
//! This module provides C-compatible error codes and structures for reporting
//! errors across the FFI boundary.

use libc::c_char;
use std::ffi::CString;

/// Error codes returned by FFI functions.
///
/// Check for `GROVEDB_ERROR_OK` (0) to confirm success.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GroveDbErrorCode {
    /// Operation succeeded
    Ok = 0,

    // General errors (1-9)
    /// Null pointer passed where non-null was expected
    NullPointer = 1,
    /// Memory allocation failed
    AllocationError = 2,
    /// Internal error
    InternalError = 3,
    /// Operation not supported
    NotSupported = 4,
    /// Invalid input provided
    InvalidInput = 5,
    /// Invalid parameter
    InvalidParameter = 6,
    /// Missing parameter
    MissingParameter = 7,
    /// Database has been closed
    DatabaseClosed = 8,

    // Path errors (10-19)
    /// Path not found in database
    PathNotFound = 10,
    /// Key not found at path
    PathKeyNotFound = 11,
    /// Parent layer not found
    PathParentLayerNotFound = 12,
    /// Invalid path
    InvalidPath = 13,
    /// Invalid parent layer path
    InvalidParentLayerPath = 14,
    /// Corrupted path
    CorruptedPath = 15,

    // Reference errors (20-29)
    /// Cyclic reference detected
    CyclicReference = 20,
    /// Reference hop limit exceeded
    ReferenceLimit = 21,
    /// Missing reference
    MissingReference = 22,
    /// Corrupted reference path not found
    CorruptedReferencePathNotFound = 23,
    /// Corrupted reference path key not found
    CorruptedReferencePathKeyNotFound = 24,

    // Data errors (30-39)
    /// Corrupted data detected
    CorruptedData = 30,
    /// Corrupted storage
    CorruptedStorage = 31,
    /// Storage/IO error
    StorageError = 32,
    /// Wrong element type
    WrongElementType = 33,

    // Query/Proof errors (40-49)
    /// Invalid query
    InvalidQuery = 40,
    /// Invalid proof
    InvalidProof = 41,

    // Operation errors (50-59)
    /// Override not allowed
    OverrideNotAllowed = 50,
    /// Deleting non-empty tree
    DeletingNonEmptyTree = 51,
    /// Invalid batch operation
    InvalidBatchOperation = 52,
    /// Delete up tree stop height error
    DeleteUpTreeStopHeightError = 53,
    /// Clearing tree with subtrees not allowed
    ClearingTreeWithSubtreesNotAllowed = 54,

    // Version errors (60-69)
    /// Version mismatch or error
    VersionError = 60,

    // Merk errors (70-79)
    /// Merk tree error
    MerkError = 70,

    // Code execution errors (80-89)
    /// Invalid code execution
    InvalidCodeExecution = 80,
    /// Corrupted code execution
    CorruptedCodeExecution = 81,

    // Client errors (90-99)
    /// Just-in-time element flags client error
    JustInTimeElementFlagsClientError = 90,
    /// Split removal bytes client error
    SplitRemovalBytesClientError = 91,
    /// Client returned non-client error
    ClientReturnedNonClientError = 92,

    /// Unknown/unhandled error
    Unknown = 255,
}

/// Detailed error information returned by FFI functions.
///
/// # Memory Management
/// If `message` is non-null, the caller must free it with `grovedb_free_string()`.
#[repr(C)]
#[derive(Debug)]
pub struct GroveDbError {
    /// Error code indicating the type of error
    pub code: GroveDbErrorCode,
    /// Null-terminated error message (may be null)
    pub message: *mut c_char,
}

impl GroveDbError {
    /// Create a success result
    pub fn ok() -> Self {
        Self {
            code: GroveDbErrorCode::Ok,
            message: std::ptr::null_mut(),
        }
    }

    /// Create a null pointer error
    pub fn null_pointer(param_name: &str) -> Self {
        Self::new(
            GroveDbErrorCode::NullPointer,
            format!("null pointer: {}", param_name),
        )
    }

    /// Create an error with a message
    pub fn new(code: GroveDbErrorCode, message: impl Into<String>) -> Self {
        let message_ptr = CString::new(message.into())
            .map(|s| s.into_raw())
            .unwrap_or(std::ptr::null_mut());

        Self {
            code,
            message: message_ptr,
        }
    }

    /// Create from a GroveDB error
    pub fn from_grovedb_error(err: grovedb::Error) -> Self {
        let (code, message) = error_to_code_and_message(err);
        Self::new(code, message)
    }

    /// Check if this represents success
    pub fn is_ok(&self) -> bool {
        self.code == GroveDbErrorCode::Ok
    }

    /// Free the message string if present
    ///
    /// # Safety
    /// Must only be called once.
    pub unsafe fn free_message(&mut self) {
        if !self.message.is_null() {
            drop(CString::from_raw(self.message));
            self.message = std::ptr::null_mut();
        }
    }
}

impl Default for GroveDbError {
    fn default() -> Self {
        Self::ok()
    }
}

/// Convert a GroveDB error to an error code and message
fn error_to_code_and_message(err: grovedb::Error) -> (GroveDbErrorCode, String) {
    use grovedb::Error;

    match err {
        // Reference errors
        Error::CyclicReference => (GroveDbErrorCode::CyclicReference, "cyclic reference".into()),
        Error::ReferenceLimit => (
            GroveDbErrorCode::ReferenceLimit,
            "reference hops limit exceeded".into(),
        ),
        Error::MissingReference(s) => {
            (GroveDbErrorCode::MissingReference, format!("missing reference: {}", s))
        }

        // Path errors
        Error::PathKeyNotFound(s) => {
            (GroveDbErrorCode::PathKeyNotFound, format!("path key not found: {}", s))
        }
        Error::PathNotFound(s) => {
            (GroveDbErrorCode::PathNotFound, format!("path not found: {}", s))
        }
        Error::PathParentLayerNotFound(s) => (
            GroveDbErrorCode::PathParentLayerNotFound,
            format!("path parent layer not found: {}", s),
        ),
        Error::InvalidPath(s) => (GroveDbErrorCode::InvalidPath, format!("invalid path: {}", s)),
        Error::InvalidParentLayerPath(s) => (
            GroveDbErrorCode::InvalidParentLayerPath,
            format!("invalid parent layer path: {}", s),
        ),
        Error::CorruptedPath(s) => {
            (GroveDbErrorCode::CorruptedPath, format!("corrupted path: {}", s))
        }

        // Corrupted reference errors
        Error::CorruptedReferencePathNotFound(s) => (
            GroveDbErrorCode::CorruptedReferencePathNotFound,
            format!("corrupted reference path not found: {}", s),
        ),
        Error::CorruptedReferencePathKeyNotFound(s) => (
            GroveDbErrorCode::CorruptedReferencePathKeyNotFound,
            format!("corrupted reference path key not found: {}", s),
        ),
        Error::CorruptedReferencePathParentLayerNotFound(s) => (
            GroveDbErrorCode::CorruptedReferencePathKeyNotFound,
            format!("corrupted reference path parent layer not found: {}", s),
        ),

        // Input errors
        Error::InvalidInput(s) => (GroveDbErrorCode::InvalidInput, format!("invalid input: {}", s)),
        Error::InvalidParameter(s) => {
            (GroveDbErrorCode::InvalidParameter, format!("invalid parameter: {}", s))
        }
        Error::MissingParameter(s) => {
            (GroveDbErrorCode::MissingParameter, format!("missing parameter: {}", s))
        }
        Error::WrongElementType(s) => {
            (GroveDbErrorCode::WrongElementType, format!("wrong element type: {}", s))
        }

        // Data errors
        Error::CorruptedData(s) => {
            (GroveDbErrorCode::CorruptedData, format!("corrupted data: {}", s))
        }
        Error::CorruptedStorage(s) => {
            (GroveDbErrorCode::CorruptedStorage, format!("corrupted storage: {}", s))
        }
        #[cfg(feature = "full")]
        Error::StorageError(e) => {
            (GroveDbErrorCode::StorageError, format!("storage error: {}", e))
        }

        // Query/Proof errors
        Error::InvalidQuery(s) => (GroveDbErrorCode::InvalidQuery, format!("invalid query: {}", s)),
        Error::InvalidProof(_, s) => {
            (GroveDbErrorCode::InvalidProof, format!("invalid proof: {}", s))
        }

        // Operation errors
        Error::OverrideNotAllowed(s) => (
            GroveDbErrorCode::OverrideNotAllowed,
            format!("override not allowed: {}", s),
        ),
        Error::DeletingNonEmptyTree(s) => (
            GroveDbErrorCode::DeletingNonEmptyTree,
            format!("deleting non-empty tree: {}", s),
        ),
        Error::InvalidBatchOperation(s) => (
            GroveDbErrorCode::InvalidBatchOperation,
            format!("invalid batch operation: {}", s),
        ),
        Error::DeleteUpTreeStopHeightMoreThanInitialPathSize(s) => (
            GroveDbErrorCode::DeleteUpTreeStopHeightError,
            format!("delete up tree stop height error: {}", s),
        ),
        Error::ClearingTreeWithSubtreesNotAllowed(s) => (
            GroveDbErrorCode::ClearingTreeWithSubtreesNotAllowed,
            format!("clearing tree with subtrees not allowed: {}", s),
        ),

        // Code execution errors
        Error::InvalidCodeExecution(s) => (
            GroveDbErrorCode::InvalidCodeExecution,
            format!("invalid code execution: {}", s),
        ),
        Error::CorruptedCodeExecution(s) => (
            GroveDbErrorCode::CorruptedCodeExecution,
            format!("corrupted code execution: {}", s),
        ),

        // Version errors
        Error::VersionError(e) => {
            (GroveDbErrorCode::VersionError, format!("version error: {}", e))
        }

        // Merk errors
        Error::MerkError(e) => (GroveDbErrorCode::MerkError, format!("merk error: {}", e)),

        // Client errors
        Error::JustInTimeElementFlagsClientError(s) => (
            GroveDbErrorCode::JustInTimeElementFlagsClientError,
            format!("just-in-time element flags client error: {}", s),
        ),
        Error::SplitRemovalBytesClientError(s) => (
            GroveDbErrorCode::SplitRemovalBytesClientError,
            format!("split removal bytes client error: {}", s),
        ),
        Error::ClientReturnedNonClientError(s) => (
            GroveDbErrorCode::ClientReturnedNonClientError,
            format!("client returned non-client error: {}", s),
        ),

        // Other
        Error::InternalError(s) => {
            (GroveDbErrorCode::InternalError, format!("internal error: {}", s))
        }
        Error::NotSupported(s) => (GroveDbErrorCode::NotSupported, format!("not supported: {}", s)),
        Error::PathNotFoundInCacheForEstimatedCosts(s) => (
            GroveDbErrorCode::PathNotFound,
            format!("path not found in cache: {}", s),
        ),
        Error::CyclicError(s) => (GroveDbErrorCode::CyclicReference, format!("cyclic error: {}", s)),
        Error::Infallible => (GroveDbErrorCode::InternalError, "infallible error".into()),
    }
}

/// Helper to set error output if pointer is non-null
pub fn set_error(error_out: *mut GroveDbError, error: GroveDbError) {
    if !error_out.is_null() {
        unsafe {
            *error_out = error;
        }
    }
}

/// Helper to set success if pointer is non-null
pub fn set_ok(error_out: *mut GroveDbError) {
    set_error(error_out, GroveDbError::ok());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_error_ok() {
        let err = GroveDbError::ok();
        assert!(err.is_ok());
        assert!(err.message.is_null());
    }

    #[test]
    fn test_error_with_message() {
        let err = GroveDbError::new(GroveDbErrorCode::PathNotFound, "test path");
        assert!(!err.is_ok());
        assert!(!err.message.is_null());

        // Clean up
        unsafe {
            let _ = CString::from_raw(err.message);
        }
    }

    #[test]
    fn test_null_pointer_error() {
        let err = GroveDbError::null_pointer("handle");
        assert_eq!(err.code, GroveDbErrorCode::NullPointer);

        // Clean up
        unsafe {
            let _ = CString::from_raw(err.message);
        }
    }
}
