// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Database inner state (shared between Database and Transaction)

#ifndef GROVEDB_DETAIL_DB_INNER_HPP
#define GROVEDB_DETAIL_DB_INNER_HPP

#include <grovedb.h>

namespace grovedb {
namespace detail {

/**
 * Inner state that holds the FFI database handle.
 *
 * This is shared between Database and Transaction via shared_ptr.
 * The FFI handle is closed when the last reference is dropped.
 */
struct DbInner {
    grovedb_t* handle;

    explicit DbInner(grovedb_t* h) : handle(h) {}

    ~DbInner() {
        if (handle) {
            grovedb_close(handle);
            handle = nullptr;
        }
    }

    DbInner(const DbInner&) = delete;
    DbInner& operator=(const DbInner&) = delete;
};

} // namespace detail
} // namespace grovedb

#endif // GROVEDB_DETAIL_DB_INNER_HPP
