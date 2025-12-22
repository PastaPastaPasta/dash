//! C-compatible type definitions for the GroveDB FFI.
//!
//! This module contains all the `#[repr(C)]` types that are exposed to C code.

use libc::size_t;

use crate::error::{GroveDbError, GroveDbErrorCode};

/// Maximum number of path segments allowed.
/// This prevents excessive memory allocation from malformed input.
pub const MAX_PATH_SEGMENTS: usize = 256;

/// Maximum size of a single path segment (10 MB).
/// This prevents excessive memory allocation from malformed input.
pub const MAX_SEGMENT_SIZE: usize = 10_000_000;

/// A borrowed byte buffer (caller owns the data, must remain valid during call).
///
/// This is used for input parameters where the FFI function only needs to read
/// the data during the call.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ByteBuffer {
    /// Pointer to the data (may be null for empty/none)
    pub data: *const u8,
    /// Length of the data in bytes
    pub len: size_t,
}

impl ByteBuffer {
    /// Create a null/empty buffer
    pub fn null() -> Self {
        Self {
            data: std::ptr::null(),
            len: 0,
        }
    }

    /// Check if this buffer is null/empty
    pub fn is_null(&self) -> bool {
        self.data.is_null()
    }

    /// Convert to a Rust slice (unsafe: caller must ensure validity)
    ///
    /// Returns None if the buffer is null.
    pub unsafe fn as_slice(&self) -> Option<&[u8]> {
        if self.data.is_null() {
            None
        } else {
            Some(std::slice::from_raw_parts(self.data, self.len))
        }
    }

    /// Convert to a Rust slice, returning empty slice if null
    pub unsafe fn as_slice_or_empty(&self) -> &[u8] {
        if self.data.is_null() {
            &[]
        } else {
            std::slice::from_raw_parts(self.data, self.len)
        }
    }

    /// Convert to a Vec<u8> by copying
    pub unsafe fn to_vec(&self) -> Vec<u8> {
        self.as_slice().map(|s| s.to_vec()).unwrap_or_default()
    }

    /// Convert to Option<Vec<u8>> - None if null, Some with copy otherwise
    pub unsafe fn to_option_vec(&self) -> Option<Vec<u8>> {
        self.as_slice().map(|s| s.to_vec())
    }
}

impl From<&[u8]> for ByteBuffer {
    fn from(slice: &[u8]) -> Self {
        Self {
            data: slice.as_ptr(),
            len: slice.len(),
        }
    }
}

/// An owned byte buffer (caller must free with `grovedb_free_byte_buffer`).
///
/// This is used for output data that the FFI function allocates and returns
/// to the caller.
#[repr(C)]
#[derive(Debug)]
pub struct OwnedByteBuffer {
    /// Pointer to the data (may be null if allocation failed or empty)
    pub data: *mut u8,
    /// Length of the data in bytes
    pub len: size_t,
    /// Capacity of the allocation (for proper deallocation)
    pub capacity: size_t,
}

impl OwnedByteBuffer {
    /// Create a null/empty buffer
    pub fn null() -> Self {
        Self {
            data: std::ptr::null_mut(),
            len: 0,
            capacity: 0,
        }
    }

    /// Create from a Vec<u8>, taking ownership
    pub fn from_vec(vec: Vec<u8>) -> Self {
        let mut vec = std::mem::ManuallyDrop::new(vec);
        Self {
            data: vec.as_mut_ptr(),
            len: vec.len(),
            capacity: vec.capacity(),
        }
    }

    /// Create from Option<Vec<u8>>
    pub fn from_option_vec(opt: Option<Vec<u8>>) -> Self {
        match opt {
            Some(vec) => Self::from_vec(vec),
            None => Self::null(),
        }
    }

    /// Check if this buffer is null/empty
    pub fn is_null(&self) -> bool {
        self.data.is_null()
    }

    /// Convert back to a Vec<u8>, taking ownership (unsafe)
    ///
    /// # Safety
    /// This consumes the buffer. The caller must ensure this is only called once.
    pub unsafe fn into_vec(self) -> Option<Vec<u8>> {
        if self.data.is_null() {
            None
        } else {
            Some(Vec::from_raw_parts(self.data, self.len, self.capacity))
        }
    }

    /// Free this buffer's memory
    ///
    /// # Safety
    /// Must only be called once per buffer.
    pub unsafe fn free(&mut self) {
        if !self.data.is_null() {
            drop(Vec::from_raw_parts(self.data, self.len, self.capacity));
            self.data = std::ptr::null_mut();
            self.len = 0;
            self.capacity = 0;
        }
    }
}

impl Default for OwnedByteBuffer {
    fn default() -> Self {
        Self::null()
    }
}

/// A path in GroveDB represented as an array of byte buffer segments.
///
/// This is used for input paths (borrowed data).
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct GroveDbPath {
    /// Array of path segments
    pub segments: *const ByteBuffer,
    /// Number of segments in the path
    pub count: size_t,
}

impl GroveDbPath {
    /// Create an empty path (root)
    pub fn empty() -> Self {
        Self {
            segments: std::ptr::null(),
            count: 0,
        }
    }

    /// Convert to a Vec<Vec<u8>> by copying all segments with validation.
    ///
    /// Returns an error if:
    /// - `segments` is null but `count > 0`
    /// - `count` exceeds `MAX_PATH_SEGMENTS`
    /// - Any segment size exceeds `MAX_SEGMENT_SIZE`
    ///
    /// # Safety
    /// Caller must ensure the path and its segments are valid pointers.
    pub unsafe fn to_vec_validated(&self) -> Result<Vec<Vec<u8>>, GroveDbError> {
        // Handle null segments pointer
        if self.segments.is_null() {
            if self.count != 0 {
                return Err(GroveDbError::new(
                    GroveDbErrorCode::InvalidParameter,
                    "path segments pointer is null but count > 0",
                ));
            }
            return Ok(Vec::new());
        }

        // Check segment count bounds
        if self.count > MAX_PATH_SEGMENTS {
            return Err(GroveDbError::new(
                GroveDbErrorCode::InvalidParameter,
                format!(
                    "path has {} segments, maximum is {}",
                    self.count, MAX_PATH_SEGMENTS
                ),
            ));
        }

        // Process segments with size validation
        let segments = std::slice::from_raw_parts(self.segments, self.count);
        segments
            .iter()
            .map(|seg| {
                if seg.len > MAX_SEGMENT_SIZE {
                    Err(GroveDbError::new(
                        GroveDbErrorCode::InvalidParameter,
                        format!(
                            "path segment size {} exceeds maximum {}",
                            seg.len, MAX_SEGMENT_SIZE
                        ),
                    ))
                } else {
                    Ok(seg.to_vec())
                }
            })
            .collect()
    }

    /// Convert to a Vec<Vec<u8>> by copying all segments.
    ///
    /// This is a convenience wrapper that panics on validation errors.
    /// For FFI use, prefer `to_vec_validated()` which returns errors properly.
    ///
    /// # Safety
    /// Caller must ensure the path and its segments are valid.
    ///
    /// # Panics
    /// Panics if path validation fails.
    #[deprecated(note = "Use to_vec_validated() for proper error handling")]
    pub unsafe fn to_vec(&self) -> Vec<Vec<u8>> {
        self.to_vec_validated().expect("path validation failed")
    }
}

/// An owned path (caller must free with `grovedb_free_path_owned`).
#[repr(C)]
#[derive(Debug)]
pub struct GroveDbPathOwned {
    /// Array of owned path segments
    pub segments: *mut OwnedByteBuffer,
    /// Number of segments in the path
    pub count: size_t,
    /// Capacity of the segments array (for proper deallocation)
    pub capacity: size_t,
}

impl GroveDbPathOwned {
    /// Create a null/empty path
    pub fn null() -> Self {
        Self {
            segments: std::ptr::null_mut(),
            count: 0,
            capacity: 0,
        }
    }

    /// Create from a Vec<Vec<u8>>
    pub fn from_vec(path: Vec<Vec<u8>>) -> Self {
        if path.is_empty() {
            return Self::null();
        }

        let segments: Vec<OwnedByteBuffer> = path.into_iter().map(OwnedByteBuffer::from_vec).collect();

        let mut segments = std::mem::ManuallyDrop::new(segments);
        Self {
            segments: segments.as_mut_ptr(),
            count: segments.len(),
            capacity: segments.capacity(),
        }
    }

    /// Free this path's memory
    ///
    /// # Safety
    /// Must only be called once per path.
    pub unsafe fn free(&mut self) {
        if !self.segments.is_null() {
            let segments = Vec::from_raw_parts(self.segments, self.count, self.capacity);
            for mut seg in segments {
                seg.free();
            }
            self.segments = std::ptr::null_mut();
            self.count = 0;
            self.capacity = 0;
        }
    }
}

impl Default for GroveDbPathOwned {
    fn default() -> Self {
        Self::null()
    }
}

/// Element type enumeration matching GroveDB's Element variants.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GroveDbElementType {
    /// An item containing arbitrary bytes
    Item = 0,
    /// A reference to another element
    Reference = 1,
    /// A subtree
    Tree = 2,
    /// A sum item (i64 value for aggregation)
    SumItem = 3,
    /// A sum tree (tree that tracks sum of SumItems)
    SumTree = 4,
    /// A big sum tree (tree that tracks i128 sum)
    BigSumTree = 5,
    /// A count tree (tree that tracks count)
    CountTree = 6,
    /// A count sum tree (tree that tracks both count and sum)
    CountSumTree = 7,
}

/// Reference path type enumeration.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReferencePathTypeTag {
    /// Absolute path reference
    AbsolutePath = 0,
    /// Upstream from root height
    UpstreamRootHeight = 1,
    /// Upstream from root height with parent path addition
    UpstreamRootHeightWithParentPathAddition = 2,
    /// Upstream from element height
    UpstreamFromElementHeight = 3,
    /// Cousin reference (sibling tree swap)
    Cousin = 4,
    /// Removed cousin reference
    RemovedCousin = 5,
    /// Sibling reference (same tree)
    Sibling = 6,
}

/// Options for insert operations.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct InsertOptions {
    /// If true, fail if key already exists (default: false)
    pub validate_insertion_does_not_override: bool,
    /// If true, fail if overriding a tree element (default: true)
    pub validate_insertion_does_not_override_tree: bool,
    /// If true, base root storage operations are free (default: true)
    pub base_root_storage_is_free: bool,
}

impl Default for InsertOptions {
    fn default() -> Self {
        Self {
            validate_insertion_does_not_override: false,
            validate_insertion_does_not_override_tree: true,
            base_root_storage_is_free: true,
        }
    }
}

impl InsertOptions {
    /// Convert from C pointer to Rust Option
    pub unsafe fn from_ptr(ptr: *const InsertOptions) -> Option<grovedb::operations::insert::InsertOptions> {
        if ptr.is_null() {
            None
        } else {
            let opts = &*ptr;
            Some(grovedb::operations::insert::InsertOptions {
                validate_insertion_does_not_override: opts.validate_insertion_does_not_override,
                validate_insertion_does_not_override_tree: opts
                    .validate_insertion_does_not_override_tree,
                base_root_storage_is_free: opts.base_root_storage_is_free,
            })
        }
    }
}

/// Options for delete operations.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct DeleteOptions {
    /// If true, allow deleting non-empty trees (default: false)
    pub allow_deleting_non_empty_trees: bool,
    /// If true, return error when deleting non-empty trees (default: true)
    pub deleting_non_empty_trees_returns_error: bool,
    /// If true, base root storage operations are free (default: true)
    pub base_root_storage_is_free: bool,
    /// If true, validate tree at path exists (default: false)
    pub validate_tree_at_path_exists: bool,
}

impl Default for DeleteOptions {
    fn default() -> Self {
        Self {
            allow_deleting_non_empty_trees: false,
            deleting_non_empty_trees_returns_error: true,
            base_root_storage_is_free: true,
            validate_tree_at_path_exists: false,
        }
    }
}

impl DeleteOptions {
    /// Convert from C pointer to Rust Option
    pub unsafe fn from_ptr(ptr: *const DeleteOptions) -> Option<grovedb::operations::delete::DeleteOptions> {
        if ptr.is_null() {
            None
        } else {
            let opts = &*ptr;
            Some(grovedb::operations::delete::DeleteOptions {
                allow_deleting_non_empty_trees: opts.allow_deleting_non_empty_trees,
                deleting_non_empty_trees_returns_error: opts.deleting_non_empty_trees_returns_error,
                base_root_storage_is_free: opts.base_root_storage_is_free,
                validate_tree_at_path_exists: opts.validate_tree_at_path_exists,
            })
        }
    }
}

/// Query item type enumeration.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueryItemType {
    /// Single key lookup
    Key = 0,
    /// Range [start, end)
    Range = 1,
    /// Range [start, end]
    RangeInclusive = 2,
    /// Full range (..)
    RangeFull = 3,
    /// Range [start, ..)
    RangeFrom = 4,
    /// Range (.., end)
    RangeTo = 5,
    /// Range (.., end]
    RangeToInclusive = 6,
    /// Range (start, ..)
    RangeAfter = 7,
    /// Range (start, end)
    RangeAfterTo = 8,
    /// Range (start, end]
    RangeAfterToInclusive = 9,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_byte_buffer_null() {
        let buf = ByteBuffer::null();
        assert!(buf.is_null());
        assert_eq!(buf.len, 0);
    }

    #[test]
    fn test_byte_buffer_from_slice() {
        let data = b"hello";
        let buf = ByteBuffer::from(data.as_slice());
        assert!(!buf.is_null());
        assert_eq!(buf.len, 5);
        unsafe {
            assert_eq!(buf.as_slice(), Some(data.as_slice()));
        }
    }

    #[test]
    fn test_owned_byte_buffer() {
        let vec = vec![1u8, 2, 3, 4, 5];
        let buf = OwnedByteBuffer::from_vec(vec);
        assert!(!buf.is_null());
        assert_eq!(buf.len, 5);

        unsafe {
            let recovered = buf.into_vec().unwrap();
            assert_eq!(recovered, vec![1, 2, 3, 4, 5]);
        }
    }

    #[test]
    fn test_path_owned() {
        let path = vec![b"tree1".to_vec(), b"subtree".to_vec()];
        let owned = GroveDbPathOwned::from_vec(path);
        assert!(!owned.segments.is_null());
        assert_eq!(owned.count, 2);
    }
}
