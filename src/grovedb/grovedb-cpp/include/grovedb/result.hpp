// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Error and Result types

#ifndef GROVEDB_RESULT_HPP
#define GROVEDB_RESULT_HPP

#include <grovedb.h>
#include <cassert>
#include <string>
#include <variant>
#include <utility>

namespace grovedb {

// Error codes matching GroveDB FFI
enum class ErrorCode : std::uint8_t {
    Ok = 0,
    NullPointer = 1,
    AllocationError = 2,
    InternalError = 3,
    NotSupported = 4,
    InvalidInput = 5,
    InvalidParameter = 6,
    MissingParameter = 7,
    DatabaseClosed = 8,

    PathNotFound = 10,
    PathKeyNotFound = 11,
    PathParentLayerNotFound = 12,
    InvalidPath = 13,
    InvalidParentLayerPath = 14,
    CorruptedPath = 15,

    CyclicReference = 20,
    ReferenceLimit = 21,
    MissingReference = 22,
    CorruptedReferencePathNotFound = 23,
    CorruptedReferencePathKeyNotFound = 24,

    CorruptedData = 30,
    CorruptedStorage = 31,
    StorageError = 32,
    WrongElementType = 33,

    InvalidQuery = 40,
    InvalidProof = 41,

    OverrideNotAllowed = 50,
    DeletingNonEmptyTree = 51,
    InvalidBatchOperation = 52,
    DeleteUpTreeStopHeightError = 53,
    ClearingTreeWithSubtreesNotAllowed = 54,

    VersionError = 60,
    MerkError = 70,

    InvalidCodeExecution = 80,
    CorruptedCodeExecution = 81,

    JustInTimeElementFlagsClientError = 90,
    SplitRemovalBytesClientError = 91,
    ClientReturnedNonClientError = 92,

    Unknown = 255
};

// Convert FFI error code to our enum
inline ErrorCode error_code_from_ffi(GroveDbErrorCode code) noexcept {
    // Direct mapping since they have same values
    return static_cast<ErrorCode>(static_cast<std::uint8_t>(code));
}

// Error class holding code and message
class Error {
public:
    Error() noexcept : code_(ErrorCode::Ok) {}
    Error(ErrorCode code, std::string message) noexcept
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    [[nodiscard]] bool is_not_found() const noexcept {
        return code_ == ErrorCode::PathNotFound ||
               code_ == ErrorCode::PathKeyNotFound;
    }

    [[nodiscard]] bool is_path_error() const noexcept {
        const auto c = static_cast<std::uint8_t>(code_);
        return c >= 10 && c <= 15;
    }

    [[nodiscard]] bool is_reference_error() const noexcept {
        const auto c = static_cast<std::uint8_t>(code_);
        return c >= 20 && c <= 24;
    }

    [[nodiscard]] bool is_corruption_error() const noexcept {
        const auto c = static_cast<std::uint8_t>(code_);
        return c >= 30 && c <= 33;
    }

    // Create from FFI error
    static Error from_ffi(GroveDbErrorCode code, const char* message) {
        return Error{
            error_code_from_ffi(code),
            message ? std::string(message) : std::string()
        };
    }

private:
    ErrorCode code_;
    std::string message_;
};

// Result type using std::variant
template <typename T>
class Result {
public:
    // Construct success result
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : data_(std::move(value)) {}

    // Construct error result
    Result(Error error) noexcept
        : data_(std::move(error)) {}

    // Check if result is success
    [[nodiscard]] bool is_ok() const noexcept {
        return std::holds_alternative<T>(data_);
    }

    [[nodiscard]] bool is_error() const noexcept {
        return std::holds_alternative<Error>(data_);
    }

    // Boolean conversion (true if ok)
    explicit operator bool() const noexcept {
        return is_ok();
    }

    // Access value (asserts on error)
    [[nodiscard]] T& value() & {
        assert(is_ok() && "Result::value() called on error result");
        return std::get<T>(data_);
    }

    [[nodiscard]] const T& value() const& {
        assert(is_ok() && "Result::value() called on error result");
        return std::get<T>(data_);
    }

    [[nodiscard]] T&& value() && {
        assert(is_ok() && "Result::value() called on error result");
        return std::get<T>(std::move(data_));
    }

    // Access error (asserts on success)
    [[nodiscard]] const Error& error() const {
        assert(is_error() && "Result::error() called on success result");
        return std::get<Error>(data_);
    }

    // Dereference operators (like optional)
    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T&& operator*() && { return std::move(*this).value(); }

    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }

    // Value or default
    template <typename U>
    [[nodiscard]] T value_or(U&& default_value) const& {
        if (is_ok()) {
            return value();
        }
        return static_cast<T>(std::forward<U>(default_value));
    }

    template <typename U>
    [[nodiscard]] T value_or(U&& default_value) && {
        if (is_ok()) {
            return std::move(*this).value();
        }
        return static_cast<T>(std::forward<U>(default_value));
    }

private:
    std::variant<T, Error> data_;
};

// Specialization for void (Result<void>)
template <>
class Result<void> {
public:
    // Construct success result
    Result() noexcept : error_{} {}

    // Construct error result
    Result(Error error) noexcept : error_(std::move(error)) {}

    [[nodiscard]] bool is_ok() const noexcept {
        return error_.code() == ErrorCode::Ok;
    }

    [[nodiscard]] bool is_error() const noexcept {
        return error_.code() != ErrorCode::Ok;
    }

    explicit operator bool() const noexcept {
        return is_ok();
    }

    [[nodiscard]] const Error& error() const {
        assert(is_error() && "Result::error() called on success result");
        return error_;
    }

private:
    Error error_;
};

} // namespace grovedb

#endif // GROVEDB_RESULT_HPP
