//! Element type definitions and conversion for the GroveDB FFI.
//!
//! This module provides C-compatible representations of GroveDB elements
//! and functions to create and convert between C and Rust element types.

use grovedb::reference_path::ReferencePathType;
use grovedb::Element;

use crate::error::{GroveDbError, GroveDbErrorCode};
use crate::types::{ByteBuffer, GroveDbElementType, GroveDbPathOwned, OwnedByteBuffer, ReferencePathTypeTag};

/// C-compatible representation of a GroveDB element.
///
/// The interpretation of fields depends on `element_type`:
///
/// - `Item`: `value` contains the item data
/// - `Reference`: `reference_path_type`, `reference_path`, and optionally `reference_hop_count`
/// - `Tree`: `value` may contain the root key (usually empty for new trees)
/// - `SumItem`: `sum_value` contains the i64 value
/// - `SumTree`: `sum_value` contains the tree's sum
/// - `BigSumTree`: `big_sum_value_low` and `big_sum_value_high` form an i128
/// - `CountTree`: `count_value` contains the count
/// - `CountSumTree`: `sum_value` and `count_value` both used
///
/// Free with `grovedb_free_element()` when done.
#[repr(C)]
#[derive(Debug)]
pub struct GroveDbElement {
    /// The type of this element
    pub element_type: GroveDbElementType,

    /// For Item: the item data
    /// For Tree/SumTree/etc: optional root key
    pub value: OwnedByteBuffer,

    /// Optional element flags
    pub flags: OwnedByteBuffer,

    /// For SumItem/SumTree/CountSumTree: the sum value (i64)
    pub sum_value: i64,

    /// For BigSumTree: low 64 bits of i128
    pub big_sum_value_low: u64,
    /// For BigSumTree: high 64 bits of i128 (including sign)
    pub big_sum_value_high: i64,

    /// For CountTree/CountSumTree: the count value (u64)
    pub count_value: u64,

    /// For Reference: the reference path type tag
    pub reference_path_type: ReferencePathTypeTag,
    /// For Reference: the reference path
    pub reference_path: GroveDbPathOwned,
    /// For Reference: max hops (0 means no limit)
    pub reference_hop_count: u8,
    /// For Reference: whether hop count is set
    pub reference_hop_count_set: bool,
}

impl GroveDbElement {
    /// Create a null/empty element
    pub fn null() -> Self {
        Self {
            element_type: GroveDbElementType::Item,
            value: OwnedByteBuffer::null(),
            flags: OwnedByteBuffer::null(),
            sum_value: 0,
            big_sum_value_low: 0,
            big_sum_value_high: 0,
            count_value: 0,
            reference_path_type: ReferencePathTypeTag::AbsolutePath,
            reference_path: GroveDbPathOwned::null(),
            reference_hop_count: 0,
            reference_hop_count_set: false,
        }
    }

    /// Create from a GroveDB Element
    pub fn from_grovedb_element(elem: &Element) -> Self {
        match elem {
            Element::Item(data, flags) => Self {
                element_type: GroveDbElementType::Item,
                value: OwnedByteBuffer::from_vec(data.clone()),
                flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                ..Self::null()
            },
            Element::Reference(ref_path, max_hop, flags) => {
                let (path_type, path) = reference_path_to_c(ref_path);
                Self {
                    element_type: GroveDbElementType::Reference,
                    reference_path_type: path_type,
                    reference_path: path,
                    reference_hop_count: max_hop.unwrap_or(0),
                    reference_hop_count_set: max_hop.is_some(),
                    flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                    ..Self::null()
                }
            }
            Element::Tree(root_key, flags) => Self {
                element_type: GroveDbElementType::Tree,
                value: OwnedByteBuffer::from_option_vec(root_key.clone()),
                flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                ..Self::null()
            },
            Element::SumItem(sum, flags) => Self {
                element_type: GroveDbElementType::SumItem,
                sum_value: *sum,
                flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                ..Self::null()
            },
            Element::SumTree(root_key, sum, flags) => Self {
                element_type: GroveDbElementType::SumTree,
                value: OwnedByteBuffer::from_option_vec(root_key.clone()),
                sum_value: *sum,
                flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                ..Self::null()
            },
            Element::BigSumTree(root_key, sum, flags) => {
                let (low, high) = i128_to_parts(*sum);
                Self {
                    element_type: GroveDbElementType::BigSumTree,
                    value: OwnedByteBuffer::from_option_vec(root_key.clone()),
                    big_sum_value_low: low,
                    big_sum_value_high: high,
                    flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                    ..Self::null()
                }
            }
            Element::CountTree(root_key, count, flags) => Self {
                element_type: GroveDbElementType::CountTree,
                value: OwnedByteBuffer::from_option_vec(root_key.clone()),
                count_value: *count,
                flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                ..Self::null()
            },
            Element::CountSumTree(root_key, count, sum, flags) => Self {
                element_type: GroveDbElementType::CountSumTree,
                value: OwnedByteBuffer::from_option_vec(root_key.clone()),
                count_value: *count,
                sum_value: *sum,
                flags: OwnedByteBuffer::from_option_vec(flags.clone()),
                ..Self::null()
            },
        }
    }

    /// Convert to a GroveDB Element
    pub fn to_grovedb_element(&self) -> Result<Element, GroveDbError> {
        let flags = unsafe { self.flags.to_option_vec() };

        match self.element_type {
            GroveDbElementType::Item => {
                let value = unsafe { self.value.to_vec() };
                Ok(Element::Item(value, flags))
            }
            GroveDbElementType::Reference => {
                let ref_path = unsafe { c_to_reference_path(self)? };
                let max_hop = if self.reference_hop_count_set {
                    Some(self.reference_hop_count)
                } else {
                    None
                };
                Ok(Element::Reference(ref_path, max_hop, flags))
            }
            GroveDbElementType::Tree => {
                let root_key = unsafe { self.value.to_option_vec() };
                Ok(Element::Tree(root_key, flags))
            }
            GroveDbElementType::SumItem => Ok(Element::SumItem(self.sum_value, flags)),
            GroveDbElementType::SumTree => {
                let root_key = unsafe { self.value.to_option_vec() };
                Ok(Element::SumTree(root_key, self.sum_value, flags))
            }
            GroveDbElementType::BigSumTree => {
                let root_key = unsafe { self.value.to_option_vec() };
                let sum = parts_to_i128(self.big_sum_value_low, self.big_sum_value_high);
                Ok(Element::BigSumTree(root_key, sum, flags))
            }
            GroveDbElementType::CountTree => {
                let root_key = unsafe { self.value.to_option_vec() };
                Ok(Element::CountTree(root_key, self.count_value, flags))
            }
            GroveDbElementType::CountSumTree => {
                let root_key = unsafe { self.value.to_option_vec() };
                Ok(Element::CountSumTree(
                    root_key,
                    self.count_value,
                    self.sum_value,
                    flags,
                ))
            }
        }
    }

    /// Free this element's allocated memory
    ///
    /// # Safety
    /// Must only be called once.
    pub unsafe fn free(&mut self) {
        self.value.free();
        self.flags.free();
        self.reference_path.free();
    }

}

impl OwnedByteBuffer {
    /// Convert to Option<Vec<u8>> without consuming
    unsafe fn to_option_vec(&self) -> Option<Vec<u8>> {
        if self.is_null() {
            None
        } else {
            Some(std::slice::from_raw_parts(self.data, self.len).to_vec())
        }
    }

    /// Convert to Vec<u8> without consuming
    unsafe fn to_vec(&self) -> Vec<u8> {
        if self.is_null() {
            Vec::new()
        } else {
            std::slice::from_raw_parts(self.data, self.len).to_vec()
        }
    }
}

/// Convert ReferencePathType to C representation
fn reference_path_to_c(ref_path: &ReferencePathType) -> (ReferencePathTypeTag, GroveDbPathOwned) {
    match ref_path {
        ReferencePathType::AbsolutePathReference(path) => {
            (ReferencePathTypeTag::AbsolutePath, GroveDbPathOwned::from_vec(path.clone()))
        }
        ReferencePathType::UpstreamRootHeightReference(_, path) => (
            ReferencePathTypeTag::UpstreamRootHeight,
            GroveDbPathOwned::from_vec(path.clone()),
        ),
        ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(_, path) => (
            ReferencePathTypeTag::UpstreamRootHeightWithParentPathAddition,
            GroveDbPathOwned::from_vec(path.clone()),
        ),
        ReferencePathType::UpstreamFromElementHeightReference(_, path) => (
            ReferencePathTypeTag::UpstreamFromElementHeight,
            GroveDbPathOwned::from_vec(path.clone()),
        ),
        ReferencePathType::CousinReference(key) => (
            ReferencePathTypeTag::Cousin,
            GroveDbPathOwned::from_vec(vec![key.clone()]),
        ),
        ReferencePathType::RemovedCousinReference(path) => {
            (ReferencePathTypeTag::RemovedCousin, GroveDbPathOwned::from_vec(path.clone()))
        }
        ReferencePathType::SiblingReference(key) => {
            (ReferencePathTypeTag::Sibling, GroveDbPathOwned::from_vec(vec![key.clone()]))
        }
    }
}

/// Convert C representation to ReferencePathType
unsafe fn c_to_reference_path(elem: &GroveDbElement) -> Result<ReferencePathType, GroveDbError> {
    let path = if elem.reference_path.segments.is_null() {
        Vec::new()
    } else {
        let segments =
            std::slice::from_raw_parts(elem.reference_path.segments, elem.reference_path.count);
        segments.iter().map(|s| s.to_vec()).collect()
    };

    match elem.reference_path_type {
        ReferencePathTypeTag::AbsolutePath => Ok(ReferencePathType::AbsolutePathReference(path)),
        ReferencePathTypeTag::UpstreamRootHeight => {
            // Note: We need the height value. For now, store it in reference_hop_count
            // This is a limitation of the current C API design
            Ok(ReferencePathType::UpstreamRootHeightReference(
                elem.reference_hop_count,
                path,
            ))
        }
        ReferencePathTypeTag::UpstreamRootHeightWithParentPathAddition => {
            Ok(ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
                elem.reference_hop_count,
                path,
            ))
        }
        ReferencePathTypeTag::UpstreamFromElementHeight => {
            Ok(ReferencePathType::UpstreamFromElementHeightReference(
                elem.reference_hop_count,
                path,
            ))
        }
        ReferencePathTypeTag::Cousin => {
            if path.is_empty() {
                return Err(GroveDbError::new(
                    GroveDbErrorCode::InvalidInput,
                    "cousin reference requires a key",
                ));
            }
            Ok(ReferencePathType::CousinReference(path.into_iter().next().unwrap()))
        }
        ReferencePathTypeTag::RemovedCousin => Ok(ReferencePathType::RemovedCousinReference(path)),
        ReferencePathTypeTag::Sibling => {
            if path.is_empty() {
                return Err(GroveDbError::new(
                    GroveDbErrorCode::InvalidInput,
                    "sibling reference requires a key",
                ));
            }
            Ok(ReferencePathType::SiblingReference(path.into_iter().next().unwrap()))
        }
    }
}

/// Split i128 into low and high parts for C representation
fn i128_to_parts(value: i128) -> (u64, i64) {
    let low = value as u64;
    let high = (value >> 64) as i64;
    (low, high)
}

/// Reconstruct i128 from low and high parts
fn parts_to_i128(low: u64, high: i64) -> i128 {
    ((high as i128) << 64) | (low as i128)
}

// ============================================================================
// FFI Functions for Element Construction
// ============================================================================

/// Create a new Item element.
///
/// # Arguments
///
/// * `data` - The item data
/// * `flags` - Optional element flags (may be null/empty)
///
/// # Returns
///
/// A new element. Caller must free with `grovedb_free_element()`.
///
/// # Safety
///
/// * `data` must be valid during the call
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_item(
    data: ByteBuffer,
    flags: ByteBuffer,
) -> *mut GroveDbElement {
    let value = data.to_vec();
    let flags_opt = flags.to_option_vec();

    let elem = Element::Item(value, flags_opt);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Create a new empty Tree element.
///
/// # Arguments
///
/// * `flags` - Optional element flags (may be null/empty)
///
/// # Returns
///
/// A new element. Caller must free with `grovedb_free_element()`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_tree(flags: ByteBuffer) -> *mut GroveDbElement {
    let flags_opt = flags.to_option_vec();
    let elem = Element::Tree(None, flags_opt);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Create a new SumItem element.
///
/// # Arguments
///
/// * `value` - The sum value (i64)
/// * `flags` - Optional element flags (may be null/empty)
///
/// # Returns
///
/// A new element. Caller must free with `grovedb_free_element()`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_sum_item(
    value: i64,
    flags: ByteBuffer,
) -> *mut GroveDbElement {
    let flags_opt = flags.to_option_vec();
    let elem = Element::SumItem(value, flags_opt);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Create a new empty SumTree element.
///
/// # Arguments
///
/// * `flags` - Optional element flags (may be null/empty)
///
/// # Returns
///
/// A new element. Caller must free with `grovedb_free_element()`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_sum_tree(flags: ByteBuffer) -> *mut GroveDbElement {
    let flags_opt = flags.to_option_vec();
    let elem = Element::SumTree(None, 0, flags_opt);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Create a new empty BigSumTree element.
///
/// # Arguments
///
/// * `flags` - Optional element flags (may be null/empty)
///
/// # Returns
///
/// A new element. Caller must free with `grovedb_free_element()`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_big_sum_tree(flags: ByteBuffer) -> *mut GroveDbElement {
    let flags_opt = flags.to_option_vec();
    let elem = Element::BigSumTree(None, 0, flags_opt);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Create a new empty CountTree element.
///
/// # Arguments
///
/// * `flags` - Optional element flags (may be null/empty)
///
/// # Returns
///
/// A new element. Caller must free with `grovedb_free_element()`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_count_tree(flags: ByteBuffer) -> *mut GroveDbElement {
    let flags_opt = flags.to_option_vec();
    let elem = Element::CountTree(None, 0, flags_opt);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Create a new empty CountSumTree element.
///
/// # Arguments
///
/// * `flags` - Optional element flags (may be null/empty)
///
/// # Returns
///
/// A new element. Caller must free with `grovedb_free_element()`.
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_count_sum_tree(
    flags: ByteBuffer,
) -> *mut GroveDbElement {
    let flags_opt = flags.to_option_vec();
    let elem = Element::CountSumTree(None, 0, 0, flags_opt);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Create a new Reference element with an absolute path.
///
/// # Arguments
///
/// * `path` - The absolute path to reference
/// * `max_hops` - Maximum reference hops (0 for no limit)
/// * `flags` - Optional element flags (may be null/empty)
/// * `error_out` - Pointer to receive error details (may be null)
///
/// # Returns
///
/// A new element, or null on error. Caller must free with `grovedb_free_element()`.
///
/// # Safety
///
/// * `path` must be valid during the call
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_new_reference(
    path: *const crate::types::GroveDbPath,
    max_hops: u8,
    flags: ByteBuffer,
    error_out: *mut GroveDbError,
) -> *mut GroveDbElement {
    use crate::error::{set_error, set_ok};

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

    let flags_opt = flags.to_option_vec();
    let max_hop = if max_hops == 0 { None } else { Some(max_hops) };

    let elem = Element::Reference(ReferencePathType::AbsolutePathReference(path_vec), max_hop, flags_opt);
    set_ok(error_out);
    Box::into_raw(Box::new(GroveDbElement::from_grovedb_element(&elem)))
}

/// Get the type of an element.
///
/// # Arguments
///
/// * `element` - The element to inspect
///
/// # Returns
///
/// The element type, or `Item` if the element is null.
#[no_mangle]
pub unsafe extern "C" fn grovedb_element_get_type(
    element: *const GroveDbElement,
) -> GroveDbElementType {
    if element.is_null() {
        GroveDbElementType::Item
    } else {
        (*element).element_type
    }
}

/// Free an element allocated by grovedb-ffi.
///
/// # Safety
///
/// * `element` must have been allocated by grovedb-ffi
/// * `element` must not be used after this call
/// * It is safe to pass null (no-op)
#[no_mangle]
pub unsafe extern "C" fn grovedb_free_element(element: *mut GroveDbElement) {
    if !element.is_null() {
        let mut elem = Box::from_raw(element);
        elem.free();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_i128_conversion() {
        let values = [0i128, 1, -1, i64::MAX as i128, i64::MIN as i128, i128::MAX, i128::MIN];

        for val in values {
            let (low, high) = i128_to_parts(val);
            let recovered = parts_to_i128(low, high);
            assert_eq!(val, recovered, "Failed for value {}", val);
        }
    }

    #[test]
    fn test_element_item_roundtrip() {
        let data = vec![1u8, 2, 3, 4, 5];
        let flags = Some(vec![0xFFu8]);
        let elem = Element::Item(data.clone(), flags.clone());

        let c_elem = GroveDbElement::from_grovedb_element(&elem);
        assert_eq!(c_elem.element_type, GroveDbElementType::Item);

        let recovered = c_elem.to_grovedb_element().unwrap();
        assert_eq!(recovered, elem);
    }

    #[test]
    fn test_element_sum_item_roundtrip() {
        let elem = Element::SumItem(42, None);
        let c_elem = GroveDbElement::from_grovedb_element(&elem);
        assert_eq!(c_elem.element_type, GroveDbElementType::SumItem);
        assert_eq!(c_elem.sum_value, 42);

        let recovered = c_elem.to_grovedb_element().unwrap();
        assert_eq!(recovered, elem);
    }

    #[test]
    fn test_element_tree_roundtrip() {
        let elem = Element::Tree(None, None);
        let c_elem = GroveDbElement::from_grovedb_element(&elem);
        assert_eq!(c_elem.element_type, GroveDbElementType::Tree);

        let recovered = c_elem.to_grovedb_element().unwrap();
        assert_eq!(recovered, elem);
    }
}
