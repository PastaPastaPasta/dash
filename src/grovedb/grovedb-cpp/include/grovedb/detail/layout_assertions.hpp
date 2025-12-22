// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Compile-time struct layout assertions to ensure FFI safety
//
// These assertions verify that the C struct layouts match what the Rust FFI expects.
// If any assertion fails, it indicates a potential ABI mismatch between C++ and Rust.

#ifndef GROVEDB_DETAIL_LAYOUT_ASSERTIONS_HPP
#define GROVEDB_DETAIL_LAYOUT_ASSERTIONS_HPP

#include <grovedb.h>
#include <cstddef>
#include <cstdint>

namespace grovedb::detail {

// ============================================================================
// Basic type size assertions
// ============================================================================

// Verify pointer size matches expected (needed for size_t calculations)
static_assert(sizeof(void*) == sizeof(std::size_t),
    "Pointer size must match size_t for correct struct layouts");

// ============================================================================
// ByteBuffer layout assertions
// ============================================================================

// ByteBuffer: { const uint8_t* data, size_t len }
static_assert(sizeof(ByteBuffer) == sizeof(const uint8_t*) + sizeof(std::size_t),
    "ByteBuffer size mismatch - expected pointer + size_t");

static_assert(offsetof(ByteBuffer, data) == 0,
    "ByteBuffer::data must be at offset 0");

static_assert(offsetof(ByteBuffer, len) == sizeof(const uint8_t*),
    "ByteBuffer::len must follow data pointer");

// ============================================================================
// OwnedByteBuffer layout assertions
// ============================================================================

// OwnedByteBuffer: { uint8_t* data, size_t len, size_t capacity }
static_assert(sizeof(OwnedByteBuffer) == sizeof(uint8_t*) + 2 * sizeof(std::size_t),
    "OwnedByteBuffer size mismatch - expected pointer + 2 size_t");

static_assert(offsetof(OwnedByteBuffer, data) == 0,
    "OwnedByteBuffer::data must be at offset 0");

static_assert(offsetof(OwnedByteBuffer, len) == sizeof(uint8_t*),
    "OwnedByteBuffer::len must follow data pointer");

static_assert(offsetof(OwnedByteBuffer, capacity) == sizeof(uint8_t*) + sizeof(std::size_t),
    "OwnedByteBuffer::capacity must follow len");

// ============================================================================
// GroveDbPath layout assertions
// ============================================================================

// GroveDbPath: { const ByteBuffer* segments, size_t count }
static_assert(sizeof(GroveDbPath) == sizeof(const ByteBuffer*) + sizeof(std::size_t),
    "GroveDbPath size mismatch");

static_assert(offsetof(GroveDbPath, segments) == 0,
    "GroveDbPath::segments must be at offset 0");

static_assert(offsetof(GroveDbPath, count) == sizeof(const ByteBuffer*),
    "GroveDbPath::count must follow segments pointer");

// ============================================================================
// Enum size assertions
// ============================================================================

// GroveDbElementType should be uint8_t sized (C enum may vary)
static_assert(sizeof(GroveDbElementType) <= sizeof(int),
    "GroveDbElementType enum size must fit in int");

// BatchOpType should be int-sized C enum
static_assert(sizeof(BatchOpType) <= sizeof(int),
    "BatchOpType enum size must fit in int");

// ReferencePathTypeTag should be int-sized C enum
static_assert(sizeof(ReferencePathTypeTag) <= sizeof(int),
    "ReferencePathTypeTag enum size must fit in int");

// ============================================================================
// GroveDbElement layout assertions
// ============================================================================

// GroveDbElement should have element_type first
static_assert(offsetof(GroveDbElement, element_type) == 0,
    "GroveDbElement::element_type must be at offset 0");

// value should come after element_type (with padding)
static_assert(offsetof(GroveDbElement, value) > 0,
    "GroveDbElement::value must be after element_type");

// flags should come after value
static_assert(offsetof(GroveDbElement, flags) > offsetof(GroveDbElement, value),
    "GroveDbElement::flags must be after value");

// sum_value should come after flags
static_assert(offsetof(GroveDbElement, sum_value) > offsetof(GroveDbElement, flags),
    "GroveDbElement::sum_value must be after flags");

// Verify the element struct is reasonably sized (sanity check)
static_assert(sizeof(GroveDbElement) >= sizeof(OwnedByteBuffer) * 2 + sizeof(int64_t) * 3,
    "GroveDbElement seems too small");

// ============================================================================
// Transaction handle layout
// ============================================================================

// grovedb_transaction_t just wraps a uint64_t id
static_assert(sizeof(grovedb_transaction_t) >= sizeof(uint64_t),
    "Transaction handle must be at least uint64_t sized");

static_assert(offsetof(grovedb_transaction_t, id) == 0,
    "Transaction id must be at offset 0");

// ============================================================================
// InsertOptions and DeleteOptions
// ============================================================================

// InsertOptions: 3 bools
static_assert(sizeof(InsertOptions) >= 3,
    "InsertOptions must have at least 3 bool fields");

// DeleteOptions: 4 bools
static_assert(sizeof(DeleteOptions) >= 4,
    "DeleteOptions must have at least 4 bool fields");

// ============================================================================
// QueryResults layout
// ============================================================================

static_assert(offsetof(QueryResults, elements) == 0,
    "QueryResults::elements must be at offset 0");

static_assert(offsetof(QueryResults, count) == sizeof(QueryResultElement*),
    "QueryResults::count must follow elements pointer");

} // namespace grovedb::detail

#endif // GROVEDB_DETAIL_LAYOUT_ASSERTIONS_HPP
