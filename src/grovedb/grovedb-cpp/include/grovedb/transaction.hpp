// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Transaction RAII wrapper

#ifndef GROVEDB_TRANSACTION_HPP
#define GROVEDB_TRANSACTION_HPP

#include <grovedb.h>
#include <memory>
#include "result.hpp"
#include "detail/ffi_helpers.hpp"
#include "detail/db_inner.hpp"

namespace grovedb {

// Forward declaration
class Database;

// Transaction state
enum class TransactionState {
    Active,      // Transaction is active and can be used
    Committed,   // Transaction was committed
    RolledBack,  // Transaction was rolled back (cannot be reused)
    Aborted      // Transaction was aborted
};

/**
 * Transaction RAII wrapper.
 * The transaction will be automatically aborted if not committed or rolled back.
 *
 * LIFETIME SAFETY:
 * This Transaction holds a shared_ptr to the database's inner state, ensuring
 * that the database FFI handle remains valid for the Transaction's lifetime.
 * Even if the Database object is destroyed or close() is called, this
 * Transaction can still safely commit or rollback.
 *
 * USAGE PATTERN:
 * @code
 * {
 *     auto& db = get_database();
 *     auto tx_result = db.start_transaction();
 *     if (tx_result.is_ok()) {
 *         auto& tx = tx_result.value();
 *         // ... use transaction ...
 *         tx.commit();  // or tx.rollback()
 *     }
 * }
 * @endcode
 *
 * After rollback(), the transaction cannot be reused.
 */
class Transaction {
public:
    // Default constructor (no transaction)
    Transaction() noexcept : handle_(nullptr), db_inner_(nullptr), state_(TransactionState::Aborted) {}

    // Move only
    Transaction(Transaction&& other) noexcept
        : handle_(other.handle_)
        , db_inner_(std::move(other.db_inner_))
        , state_(other.state_)
    {
        other.handle_ = nullptr;
        other.state_ = TransactionState::Aborted;
    }

    Transaction& operator=(Transaction&& other) noexcept {
        if (this != &other) {
            abort_if_active();
            handle_ = other.handle_;
            db_inner_ = std::move(other.db_inner_);
            state_ = other.state_;
            other.handle_ = nullptr;
            other.state_ = TransactionState::Aborted;
        }
        return *this;
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    // Destructor - aborts if still active
    ~Transaction() {
        abort_if_active();
    }

    // Check if transaction is valid/active
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && state_ == TransactionState::Active;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return state_ == TransactionState::Active;
    }

    explicit operator bool() const noexcept { return valid(); }

    // Get current state
    [[nodiscard]] TransactionState state() const noexcept { return state_; }

    // Get underlying handle (for FFI calls)
    [[nodiscard]] grovedb_transaction_t* get() noexcept { return handle_; }
    [[nodiscard]] const grovedb_transaction_t* get() const noexcept { return handle_; }

    // Commit the transaction
    [[nodiscard]] Result<void> commit() {
        if (!handle_ || !db_inner_ || !db_inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "No active transaction"};
        }
        if (state_ != TransactionState::Active) {
            return Error{ErrorCode::InvalidParameter, "Transaction not active"};
        }

        detail::ErrorGuard err;
        bool ok = grovedb_commit_transaction(db_inner_->handle, handle_, err.ptr());

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        state_ = TransactionState::Committed;
        handle_ = nullptr;  // Handle is consumed on successful commit
        return Result<void>{};
    }

    // Rollback the transaction (cannot be reused after rollback)
    [[nodiscard]] Result<void> rollback() {
        if (!handle_ || !db_inner_ || !db_inner_->handle) {
            return Error{ErrorCode::InvalidParameter, "No active transaction"};
        }
        if (state_ != TransactionState::Active) {
            return Error{ErrorCode::InvalidParameter, "Transaction not active"};
        }

        detail::ErrorGuard err;
        bool ok = grovedb_rollback_transaction(db_inner_->handle, handle_, err.ptr());

        if (!ok || !err.ok()) {
            return Error{error_code_from_ffi(err.code()), err.take_message()};
        }

        state_ = TransactionState::RolledBack;
        // Transaction cannot be reused after rollback.
        // Free with grovedb_free_transaction() or let destructor handle it.
        return Result<void>{};
    }

    // Abort the transaction (cannot be reused)
    void abort() noexcept {
        abort_if_active();
    }

    // Internal: create from FFI handles
    static Transaction from_handles(
        grovedb_transaction_t* tx,
        std::shared_ptr<detail::DbInner> db_inner
    ) noexcept {
        Transaction t;
        t.handle_ = tx;
        t.db_inner_ = std::move(db_inner);
        t.state_ = TransactionState::Active;
        return t;
    }

private:
    void abort_if_active() noexcept {
        if (handle_ == nullptr) {
            return;
        }

        if (state_ == TransactionState::Active && db_inner_ && db_inner_->handle) {
            grovedb_abort_transaction(db_inner_->handle, handle_);
        } else if (db_inner_ && db_inner_->handle) {
            // Only free if we have a valid db_inner to ensure proper cleanup
            grovedb_free_transaction(handle_);
        }
        // If db_inner is null but handle is not, the shared_ptr guarantees
        // this cannot happen - database is kept alive by our reference.

        handle_ = nullptr;
        state_ = TransactionState::Aborted;
    }

    grovedb_transaction_t* handle_;
    std::shared_ptr<detail::DbInner> db_inner_;  // Keeps database alive
    TransactionState state_;
};

} // namespace grovedb

#endif // GROVEDB_TRANSACTION_HPP
