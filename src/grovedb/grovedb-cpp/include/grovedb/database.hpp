// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Database main class

#ifndef GROVEDB_DATABASE_HPP
#define GROVEDB_DATABASE_HPP

#include <grovedb.h>
#include "result.hpp"
#include "byte_buffer.hpp"
#include "path.hpp"
#include "element.hpp"
#include "transaction.hpp"
#include "detail/ffi_helpers.hpp"
#include "detail/db_inner.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <memory>

namespace grovedb {

// Forward declarations for query types
class PathQueryBuilder;
class QueryResults;

// Forward declaration for batch types
class BatchBuilder;

/**
 * Database RAII wrapper.
 *
 * This class uses a shared_ptr internally to ensure that the underlying
 * FFI handle remains valid as long as any Transaction references it.
 * When you call close(), the Database releases its reference, but the
 * underlying handle remains open until all Transactions are destroyed.
 *
 * Behavioral note:
 * - After close(), is_open() returns false
 * - But Transactions created before close() can still commit/rollback
 * - The actual FFI grovedb_close() is called when the last reference drops
 */
class Database {
public:
    // Default constructor (no database)
    Database() noexcept : inner_(nullptr) {}

    // Move only
    Database(Database&& other) noexcept : inner_(std::move(other.inner_)) {}

    Database& operator=(Database&& other) noexcept {
        if (this != &other) {
            inner_ = std::move(other.inner_);
        }
        return *this;
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Destructor - releases our reference to Inner
    ~Database() = default;

    // Open a database at the specified path
    [[nodiscard]] static Result<Database> open(std::string_view path) {
        std::string path_str(path);

        detail::ErrorGuard err;
        grovedb_t* handle = grovedb_open(path_str.c_str(), err.ptr());

        if (!handle || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        Database db;
        db.inner_ = std::make_shared<detail::DbInner>(handle);
        return db;
    }

    // Close the database
    // Note: This releases our reference. If Transactions exist, the
    // underlying FFI handle stays open until they're destroyed.
    void close() noexcept {
        inner_.reset();
    }

    // Check if database is open (from this Database object's perspective)
    [[nodiscard]] bool is_open() const noexcept {
        return inner_ && inner_->handle != nullptr;
    }
    explicit operator bool() const noexcept { return is_open(); }

    // Get underlying handle
    [[nodiscard]] grovedb_t* get() noexcept {
        return inner_ ? inner_->handle : nullptr;
    }
    [[nodiscard]] const grovedb_t* get() const noexcept {
        return inner_ ? inner_->handle : nullptr;
    }

    // ========================================================================
    // Database operations
    // ========================================================================

    // Flush all pending writes to disk
    [[nodiscard]] Result<void> flush() {
        if (!inner_ || !inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "Database not open"};
        }

        detail::ErrorGuard err;
        bool ok = grovedb_flush(inner_->handle, err.ptr());

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        return Result<void>{};
    }

    // Get root hash
    [[nodiscard]] Result<Hash> root_hash(const Transaction* tx = nullptr) const {
        if (!inner_ || !inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "Database not open"};
        }

        Hash hash;
        detail::ErrorGuard err;
        bool ok = grovedb_root_hash(
            inner_->handle,
            tx ? tx->get() : nullptr,
            reinterpret_cast<uint8_t(*)[32]>(hash.bytes.data()),
            err.ptr()
        );

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        return hash;
    }

    // ========================================================================
    // CRUD operations
    // ========================================================================

    // Insert an element
    [[nodiscard]] Result<void> insert(
        const Path& path,
        ByteSpan key,
        const Element& element,
        const InsertOptions& options = {},
        const Transaction* tx = nullptr
    ) {
        if (!inner_ || !inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "Database not open"};
        }
        if (!element.valid()) {
            return Error{ErrorCode::InvalidParameter, "Invalid element"};
        }

        detail::FFIPath ffi_path(path);
        ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());
        auto ffi_options = options.to_ffi();

        detail::ErrorGuard err;
        bool ok = grovedb_insert(
            inner_->handle,
            ffi_path.ptr(),
            key_buf,
            element.get(),
            &ffi_options,
            tx ? tx->get() : nullptr,
            err.ptr()
        );

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        return Result<void>{};
    }

    // Convenience: insert with initializer list path
    [[nodiscard]] Result<void> insert(
        std::initializer_list<std::string_view> path,
        std::string_view key,
        const Element& element,
        const InsertOptions& options = {},
        const Transaction* tx = nullptr
    ) {
        return insert(Path(path), ByteSpan(key), element, options, tx);
    }

    // Get an element
    [[nodiscard]] Result<std::optional<Element>> get(
        const Path& path,
        ByteSpan key,
        const Transaction* tx = nullptr
    ) const {
        if (!inner_ || !inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "Database not open"};
        }

        detail::FFIPath ffi_path(path);
        ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());

        detail::ErrorGuard err;
        GroveDbElement* elem = grovedb_get(
            inner_->handle,
            ffi_path.ptr(),
            key_buf,
            tx ? tx->get() : nullptr,
            err.ptr()
        );

        if (!err.ok()) {
            // Check if it's a "not found" error
            if (err.code() == GROVE_DB_ERROR_CODE_PATH_KEY_NOT_FOUND ||
                err.code() == GROVE_DB_ERROR_CODE_PATH_NOT_FOUND) {
                return std::optional<Element>{std::nullopt};
            }
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        if (!elem) {
            return std::optional<Element>{std::nullopt};
        }

        return std::optional<Element>{Element::from_handle(elem)};
    }

    // Convenience: get with initializer list path
    [[nodiscard]] Result<std::optional<Element>> get(
        std::initializer_list<std::string_view> path,
        std::string_view key,
        const Transaction* tx = nullptr
    ) const {
        return get(Path(path), ByteSpan(key), tx);
    }

    // Delete an element
    [[nodiscard]] Result<void> delete_element(
        const Path& path,
        ByteSpan key,
        const DeleteOptions& options = {},
        const Transaction* tx = nullptr
    ) {
        if (!inner_ || !inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "Database not open"};
        }

        detail::FFIPath ffi_path(path);
        ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());
        auto ffi_options = options.to_ffi();

        detail::ErrorGuard err;
        bool ok = grovedb_delete(
            inner_->handle,
            ffi_path.ptr(),
            key_buf,
            &ffi_options,
            tx ? tx->get() : nullptr,
            err.ptr()
        );

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        return Result<void>{};
    }

    // Convenience: delete with initializer list path
    [[nodiscard]] Result<void> delete_element(
        std::initializer_list<std::string_view> path,
        std::string_view key,
        const DeleteOptions& options = {},
        const Transaction* tx = nullptr
    ) {
        return delete_element(Path(path), ByteSpan(key), options, tx);
    }

    // ========================================================================
    // Transaction operations
    // ========================================================================

    // Start a new transaction
    [[nodiscard]] Result<Transaction> start_transaction() {
        if (!inner_ || !inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "Database not open"};
        }

        detail::ErrorGuard err;
        grovedb_transaction_t* tx = grovedb_start_transaction(inner_->handle, err.ptr());

        if (!tx || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        // Pass shared_ptr to Transaction - this keeps database alive
        return Transaction::from_handles(tx, inner_);
    }

    // Check if a transaction is active
    [[nodiscard]] bool is_transaction_active(const Transaction& tx) const {
        if (!inner_ || !inner_->handle || !tx.get()) {
            return false;
        }
        return grovedb_is_transaction_active(inner_->handle, tx.get());
    }

    // ========================================================================
    // Query operations (implemented in query.hpp)
    // ========================================================================

    // Execute a path query
    Result<QueryResults> query(PathQueryBuilder& query, const Transaction* tx = nullptr);
    Result<QueryResults> query(PathQueryBuilder&& query, const Transaction* tx = nullptr);

    // ========================================================================
    // Batch operations (implemented in batch.hpp)
    // ========================================================================

    // Apply a batch of operations atomically
    Result<void> apply_batch(BatchBuilder& batch, const Transaction* tx = nullptr);
    Result<void> apply_batch(BatchBuilder&& batch, const Transaction* tx = nullptr);

private:
    std::shared_ptr<detail::DbInner> inner_;
};

} // namespace grovedb

#endif // GROVEDB_DATABASE_HPP
