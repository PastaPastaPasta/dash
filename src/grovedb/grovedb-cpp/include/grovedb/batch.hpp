// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Batch operations header

#ifndef GROVEDB_BATCH_HPP
#define GROVEDB_BATCH_HPP

#include <grovedb.h>
#include "result.hpp"
#include "byte_buffer.hpp"
#include "path.hpp"
#include "element.hpp"
#include "detail/ffi_helpers.hpp"
#include <vector>
#include <cstdint>

namespace grovedb {

// Forward declarations
class Database;
class Transaction;

/**
 * Batch operation types
 */
enum class BatchOpType : std::uint32_t {
    InsertOnly = 0,      ///< Insert only if key doesn't exist
    InsertOrReplace = 1, ///< Insert or replace existing
    Replace = 2,         ///< Replace only if key exists
    Delete = 3,          ///< Delete an element
    DeleteTree = 4,      ///< Delete a non-sum tree
    DeleteSumTree = 5    ///< Delete a sum tree
};

/**
 * Batch operation builder
 *
 * Build up a list of operations and apply them atomically.
 *
 * Example:
 * ```cpp
 * BatchBuilder batch;
 * batch.add_insert(root, "tree1", Element::new_tree());
 * batch.add_insert(Path{"tree1"}, "key1", Element::new_item(ByteSpan("value")));
 * auto result = db.apply_batch(batch);
 * ```
 */
class BatchBuilder {
public:
    // Default constructor
    BatchBuilder() : handle_(nullptr) {
        detail::ErrorGuard err;
        handle_ = grovedb_batch_new(err.ptr());
    }

    // Move constructor
    BatchBuilder(BatchBuilder&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    // Move assignment
    BatchBuilder& operator=(BatchBuilder&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    // No copy
    BatchBuilder(const BatchBuilder&) = delete;
    BatchBuilder& operator=(const BatchBuilder&) = delete;

    // Destructor
    ~BatchBuilder() {
        reset();
    }

    // Reset (free resources)
    void reset() noexcept {
        if (handle_ != nullptr) {
            grovedb_free_batch(handle_);
            handle_ = nullptr;
        }
    }

    // Check if valid
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // Get number of operations
    [[nodiscard]] std::size_t size() const noexcept {
        return handle_ ? grovedb_batch_count(handle_) : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // =========================================================================
    // Insert operations
    // =========================================================================

    /**
     * Add an insert operation
     *
     * The element is borrowed, not consumed. Caller retains ownership and
     * must keep the element valid until the batch operation completes.
     *
     * @param path Path to the subtree
     * @param key Key within the subtree
     * @param element Element to insert (borrowed, caller retains ownership)
     * @param op_type Type of insert (InsertOnly, InsertOrReplace, Replace)
     * @return Result indicating success or error
     */
    Result<void> add_insert(const Path& path, ByteSpan key, const Element& element,
                            BatchOpType op_type = BatchOpType::InsertOrReplace) {
        if (!valid()) {
            return Error{ErrorCode::InvalidParameter, "Batch not valid"};
        }
        if (!element.valid()) {
            return Error{ErrorCode::InvalidParameter, "Element not valid"};
        }

        detail::FFIPath ffi_path(path);
        ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());
        detail::ErrorGuard err;

        // Element is borrowed, not consumed
        bool ok = grovedb_batch_add_insert(
            handle_,
            ffi_path.ptr(),
            key_buf,
            element.get(),
            static_cast<::BatchOpType>(op_type),
            err.ptr()
        );

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }
        return {};
    }

    /**
     * Add an insert-only operation (fails if key exists)
     */
    Result<void> add_insert_only(const Path& path, ByteSpan key, const Element& element) {
        return add_insert(path, key, element, BatchOpType::InsertOnly);
    }

    /**
     * Add a replace operation (fails if key doesn't exist)
     */
    Result<void> add_replace(const Path& path, ByteSpan key, const Element& element) {
        return add_insert(path, key, element, BatchOpType::Replace);
    }

    // =========================================================================
    // Delete operations
    // =========================================================================

    /**
     * Add a delete operation
     *
     * @param path Path to the subtree
     * @param key Key to delete
     * @return Result indicating success or error
     */
    Result<void> add_delete(const Path& path, ByteSpan key) {
        if (!valid()) {
            return Error{ErrorCode::InvalidParameter, "Batch not valid"};
        }

        detail::FFIPath ffi_path(path);
        ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());
        detail::ErrorGuard err;

        bool ok = grovedb_batch_add_delete(
            handle_,
            ffi_path.ptr(),
            key_buf,
            err.ptr()
        );

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }
        return {};
    }

    /**
     * Add a delete tree operation
     *
     * @param path Path to the subtree containing the tree
     * @param key Key of the tree to delete
     * @param is_sum_tree Whether the tree is a sum tree
     * @return Result indicating success or error
     */
    Result<void> add_delete_tree(const Path& path, ByteSpan key, bool is_sum_tree = false) {
        if (!valid()) {
            return Error{ErrorCode::InvalidParameter, "Batch not valid"};
        }

        detail::FFIPath ffi_path(path);
        ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());
        detail::ErrorGuard err;

        bool ok = grovedb_batch_add_delete_tree(
            handle_,
            ffi_path.ptr(),
            key_buf,
            is_sum_tree,
            err.ptr()
        );

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }
        return {};
    }

    // =========================================================================
    // Internal
    // =========================================================================

    // Get FFI handle (for Database::apply_batch)
    [[nodiscard]] ::BatchHandle* get() noexcept { return handle_; }
    [[nodiscard]] const ::BatchHandle* get() const noexcept { return handle_; }

    // Release ownership (called by Database::apply_batch)
    ::BatchHandle* release() noexcept {
        auto* h = handle_;
        handle_ = nullptr;
        return h;
    }

private:
    ::BatchHandle* handle_;
};

// ============================================================================
// Database batch method implementation
// ============================================================================

/**
 * Apply a batch of operations atomically
 *
 * This consumes the BatchBuilder.
 *
 * @param batch Batch builder (will be consumed)
 * @param tx Optional transaction
 * @return Result indicating success or error
 */
inline Result<void> Database::apply_batch(BatchBuilder& batch, const Transaction* tx) {
    if (!is_open()) {
        return Error{ErrorCode::InvalidParameter, "Database not open"};
    }
    if (!batch.valid()) {
        return Error{ErrorCode::InvalidParameter, "Batch not valid"};
    }

    detail::ErrorGuard err;
    bool ok = grovedb_apply_batch(
        get(),
        batch.release(),  // Ownership transfers to FFI
        tx ? tx->get() : nullptr,
        err.ptr()
    );

    if (!ok || !err.ok()) {
        return Error{error_code_from_ffi(err.code()), err.take_message()};
    }
    return {};
}

inline Result<void> Database::apply_batch(BatchBuilder&& batch, const Transaction* tx) {
    return apply_batch(batch, tx);
}

} // namespace grovedb

#endif // GROVEDB_BATCH_HPP
