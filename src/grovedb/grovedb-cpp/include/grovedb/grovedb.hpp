// grovedb-cpp - C++17 header-only wrapper for GroveDB FFI
//
// This is the umbrella header - include this to get all functionality.
//
// Usage:
//   #include <grovedb/grovedb.hpp>
//
// All classes are in the `grovedb` namespace.
//
// Link with: -lgrovedb_ffi

#ifndef GROVEDB_HPP
#define GROVEDB_HPP

// Version
#define GROVEDB_CPP_VERSION_MAJOR 1
#define GROVEDB_CPP_VERSION_MINOR 0
#define GROVEDB_CPP_VERSION_PATCH 0

// Compile-time struct layout verification
#include "detail/layout_assertions.hpp"

// Core types
#include "result.hpp"
#include "byte_buffer.hpp"
#include "path.hpp"
#include "element.hpp"

// Database operations
#include "transaction.hpp"
#include "database.hpp"
#include "query.hpp"
#include "proof.hpp"
#include "batch.hpp"

#endif // GROVEDB_HPP
