// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Forward declarations

#ifndef GROVEDB_FWD_HPP
#define GROVEDB_FWD_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace grovedb {

// Core types
class Error;
template <typename T> class Result;

// Byte handling
class ByteSpan;
using ByteVector = std::vector<std::uint8_t>;
struct Hash;

// Path
class Path;

// Element
enum class ElementType : std::uint8_t;
class Element;

// Database
class Database;
class Transaction;

// Query
class PathQueryBuilder;
class QueryResults;
class QueryResultItem;

// Options
struct InsertOptions;
struct DeleteOptions;

} // namespace grovedb

#endif // GROVEDB_FWD_HPP
