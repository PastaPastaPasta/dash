// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Internal FFI conversion helpers

#ifndef GROVEDB_DETAIL_FFI_HELPERS_HPP
#define GROVEDB_DETAIL_FFI_HELPERS_HPP

#include <grovedb.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace grovedb {
namespace detail {

// Convert a ByteSpan-like object to FFI ByteBuffer
inline ByteBuffer to_byte_buffer(const std::uint8_t* data, std::size_t len) noexcept {
    return ByteBuffer{data, len};
}

inline ByteBuffer to_byte_buffer(std::string_view sv) noexcept {
    return ByteBuffer{reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()};
}

inline ByteBuffer to_byte_buffer(const std::vector<std::uint8_t>& v) noexcept {
    return ByteBuffer{v.data(), v.size()};
}

// Empty byte buffer (for optional parameters)
inline ByteBuffer empty_byte_buffer() noexcept {
    return ByteBuffer{nullptr, 0};
}

// RAII helper for GroveDbPath conversion
// Holds both the segments array and the path struct
class FFIPath {
public:
    FFIPath() noexcept : path_{nullptr, 0} {}

    template <typename PathT>
    explicit FFIPath(const PathT& p) {
        const auto& segments = p.segments();
        if (segments.empty()) {
            path_ = GroveDbPath{nullptr, 0};
            return;
        }

        buffers_.reserve(segments.size());
        for (const auto& seg : segments) {
            buffers_.push_back(ByteBuffer{seg.data(), seg.size()});
        }
        path_ = GroveDbPath{buffers_.data(), buffers_.size()};
    }

    // For initializer list paths
    FFIPath(std::initializer_list<std::string_view> segments) {
        if (segments.size() == 0) {
            path_ = GroveDbPath{nullptr, 0};
            return;
        }

        storage_.reserve(segments.size());
        buffers_.reserve(segments.size());

        for (const auto& seg : segments) {
            storage_.emplace_back(seg.begin(), seg.end());
        }
        for (const auto& seg : storage_) {
            buffers_.push_back(ByteBuffer{seg.data(), seg.size()});
        }
        path_ = GroveDbPath{buffers_.data(), buffers_.size()};
    }

    // Non-copyable and non-movable: path_ contains pointers into buffers_,
    // so moving would leave dangling pointers in the moved-to object
    FFIPath(const FFIPath&) = delete;
    FFIPath& operator=(const FFIPath&) = delete;
    FFIPath(FFIPath&&) = delete;
    FFIPath& operator=(FFIPath&&) = delete;

    const GroveDbPath* ptr() const noexcept {
        return &path_;
    }

    const GroveDbPath& get() const noexcept {
        return path_;
    }

private:
    std::vector<std::vector<std::uint8_t>> storage_;  // Owns data for init-list case
    std::vector<ByteBuffer> buffers_;
    GroveDbPath path_;
};

// Convert FFI error to our Error class
// Returns true if there was an error
inline bool check_error(const GroveDbError& err, std::string& out_message) {
    if (err.code == GROVE_DB_ERROR_CODE_OK) {
        return false;
    }
    if (err.message != nullptr) {
        out_message = err.message;
        grovedb_free_string(err.message);
    } else {
        out_message = "Unknown error";
    }
    return true;
}

// RAII guard for error struct cleanup
class ErrorGuard {
public:
    ErrorGuard() noexcept : error_{GROVE_DB_ERROR_CODE_OK, nullptr} {}
    ~ErrorGuard() {
        if (error_.message != nullptr) {
            grovedb_free_string(error_.message);
        }
    }

    ErrorGuard(const ErrorGuard&) = delete;
    ErrorGuard& operator=(const ErrorGuard&) = delete;

    GroveDbError* ptr() noexcept { return &error_; }
    GroveDbErrorCode code() const noexcept { return error_.code; }
    bool ok() const noexcept { return error_.code == GROVE_DB_ERROR_CODE_OK; }

    std::string take_message() {
        std::string msg;
        if (error_.message != nullptr) {
            msg = error_.message;
            grovedb_free_string(error_.message);
            error_.message = nullptr;
        }
        return msg;
    }

private:
    GroveDbError error_;
};

// Copy data from OwnedByteBuffer to vector
inline std::vector<std::uint8_t> copy_owned_buffer(const OwnedByteBuffer& buf) {
    if (buf.data == nullptr || buf.len == 0) {
        return {};
    }
    return std::vector<std::uint8_t>(buf.data, buf.data + buf.len);
}

} // namespace detail
} // namespace grovedb

#endif // GROVEDB_DETAIL_FFI_HELPERS_HPP
