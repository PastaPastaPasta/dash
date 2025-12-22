// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Proof generation and verification

#ifndef GROVEDB_PROOF_HPP
#define GROVEDB_PROOF_HPP

#include <grovedb.h>
#include "result.hpp"
#include "byte_buffer.hpp"
#include "path.hpp"
#include "query.hpp"
#include "database.hpp"
#include "detail/ffi_helpers.hpp"

namespace grovedb {

// Proof data (owned)
class Proof {
public:
    Proof() = default;
    explicit Proof(ByteVector data) : data_(std::move(data)) {}

    [[nodiscard]] const ByteVector& data() const noexcept { return data_; }
    [[nodiscard]] ByteSpan as_span() const noexcept {
        return ByteSpan(data_.data(), data_.size());
    }

    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
    ByteVector data_;
};

// Verification result containing root hash and query results
struct VerificationResult {
    Hash root_hash;
    QueryResults results;
};

// ============================================================================
// Proof generation (requires full Database)
// ============================================================================

// Generate a proof for a single key query
inline Result<Proof> prove_key(
    const Database& db,
    const Path& path,
    ByteSpan key
) {
    if (!db.is_open()) {
        return Error{ErrorCode::InvalidParameter, "Database not open"};
    }

    detail::FFIPath ffi_path(path);
    ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());

    detail::ErrorGuard err;
    OwnedByteBuffer proof = grovedb_prove_query_key(
        db.get(),
        ffi_path.ptr(),
        key_buf,
        err.ptr()
    );

    if (!err.ok()) {
        return Error{error_code_from_ffi(err.code()), err.take_message()};
    }

    if (proof.data == nullptr) {
        return Error{ErrorCode::InternalError, "Proof generation returned null"};
    }

    ByteVector data(proof.data, proof.data + proof.len);
    grovedb_free_byte_buffer(&proof);

    return Proof{std::move(data)};
}

// Convenience: prove_key with string key
inline Result<Proof> prove_key(
    const Database& db,
    const Path& path,
    std::string_view key
) {
    return prove_key(db, path, ByteSpan(key));
}

// Convenience: prove_key with initializer list path
inline Result<Proof> prove_key(
    const Database& db,
    std::initializer_list<std::string_view> path,
    std::string_view key
) {
    return prove_key(db, Path(path), ByteSpan(key));
}

// Generate a proof for a path query
inline Result<Proof> prove_query(
    const Database& db,
    PathQueryBuilder& query
) {
    if (!db.is_open()) {
        return Error{ErrorCode::InvalidParameter, "Database not open"};
    }
    if (!query.valid()) {
        return Error{ErrorCode::InvalidParameter, "Invalid query builder"};
    }

    detail::ErrorGuard err;
    OwnedByteBuffer proof = grovedb_prove_query(
        db.get(),
        query.release(),
        err.ptr()
    );
    query.mark_consumed();

    if (!err.ok()) {
        return Error{error_code_from_ffi(err.code()), err.take_message()};
    }

    if (proof.data == nullptr) {
        return Error{ErrorCode::InternalError, "Proof generation returned null"};
    }

    ByteVector data(proof.data, proof.data + proof.len);
    grovedb_free_byte_buffer(&proof);

    return Proof{std::move(data)};
}

inline Result<Proof> prove_query(
    const Database& db,
    PathQueryBuilder&& query
) {
    return prove_query(db, query);
}

// ============================================================================
// Proof verification (does not require Database - for light clients)
// ============================================================================

// Verify a proof for a single key query
inline Result<VerificationResult> verify_key(
    ByteSpan proof,
    const Path& path,
    ByteSpan key
) {
    detail::FFIPath ffi_path(path);
    ByteBuffer proof_buf = detail::to_byte_buffer(proof.data(), proof.size());
    ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());

    Hash root_hash;
    detail::ErrorGuard err;

    ::QueryResults* results = grovedb_verify_query_key(
        proof_buf,
        ffi_path.ptr(),
        key_buf,
        reinterpret_cast<uint8_t(*)[32]>(root_hash.bytes.data()),
        err.ptr()
    );

    if (!err.ok()) {
        return Error{error_code_from_ffi(err.code()), err.take_message()};
    }

    return VerificationResult{
        root_hash,
        QueryResults::from_handle(results)
    };
}

// Convenience overloads for verify_key
inline Result<VerificationResult> verify_key(
    const Proof& proof,
    const Path& path,
    ByteSpan key
) {
    return verify_key(proof.as_span(), path, key);
}

inline Result<VerificationResult> verify_key(
    ByteSpan proof,
    const Path& path,
    std::string_view key
) {
    return verify_key(proof, path, ByteSpan(key));
}

inline Result<VerificationResult> verify_key(
    const Proof& proof,
    const Path& path,
    std::string_view key
) {
    return verify_key(proof.as_span(), path, ByteSpan(key));
}

inline Result<VerificationResult> verify_key(
    ByteSpan proof,
    std::initializer_list<std::string_view> path,
    std::string_view key
) {
    return verify_key(proof, Path(path), ByteSpan(key));
}

inline Result<VerificationResult> verify_key(
    const Proof& proof,
    std::initializer_list<std::string_view> path,
    std::string_view key
) {
    return verify_key(proof.as_span(), Path(path), ByteSpan(key));
}

// Verify a proof against a path query
inline Result<VerificationResult> verify_query(
    ByteSpan proof,
    PathQueryBuilder& query
) {
    if (!query.valid()) {
        return Error{ErrorCode::InvalidParameter, "Invalid query builder"};
    }

    ByteBuffer proof_buf = detail::to_byte_buffer(proof.data(), proof.size());

    Hash root_hash;
    detail::ErrorGuard err;

    ::QueryResults* results = grovedb_verify_query(
        proof_buf,
        query.release(),
        reinterpret_cast<uint8_t(*)[32]>(root_hash.bytes.data()),
        err.ptr()
    );
    query.mark_consumed();

    if (!err.ok()) {
        return Error{error_code_from_ffi(err.code()), err.take_message()};
    }

    return VerificationResult{
        root_hash,
        QueryResults::from_handle(results)
    };
}

inline Result<VerificationResult> verify_query(
    const Proof& proof,
    PathQueryBuilder& query
) {
    return verify_query(proof.as_span(), query);
}

inline Result<VerificationResult> verify_query(
    ByteSpan proof,
    PathQueryBuilder&& query
) {
    return verify_query(proof, query);
}

inline Result<VerificationResult> verify_query(
    const Proof& proof,
    PathQueryBuilder&& query
) {
    return verify_query(proof.as_span(), query);
}

// Verify a proof with absence proof support
inline Result<VerificationResult> verify_query_with_absence(
    ByteSpan proof,
    PathQueryBuilder& query
) {
    if (!query.valid()) {
        return Error{ErrorCode::InvalidParameter, "Invalid query builder"};
    }

    ByteBuffer proof_buf = detail::to_byte_buffer(proof.data(), proof.size());

    Hash root_hash;
    detail::ErrorGuard err;

    ::QueryResults* results = grovedb_verify_query_with_absence_proof(
        proof_buf,
        query.release(),
        reinterpret_cast<uint8_t(*)[32]>(root_hash.bytes.data()),
        err.ptr()
    );
    query.mark_consumed();

    if (!err.ok()) {
        return Error{error_code_from_ffi(err.code()), err.take_message()};
    }

    return VerificationResult{
        root_hash,
        QueryResults::from_handle(results)
    };
}

inline Result<VerificationResult> verify_query_with_absence(
    const Proof& proof,
    PathQueryBuilder& query
) {
    return verify_query_with_absence(proof.as_span(), query);
}

inline Result<VerificationResult> verify_query_with_absence(
    ByteSpan proof,
    PathQueryBuilder&& query
) {
    return verify_query_with_absence(proof, query);
}

inline Result<VerificationResult> verify_query_with_absence(
    const Proof& proof,
    PathQueryBuilder&& query
) {
    return verify_query_with_absence(proof.as_span(), query);
}

} // namespace grovedb

#endif // GROVEDB_PROOF_HPP
