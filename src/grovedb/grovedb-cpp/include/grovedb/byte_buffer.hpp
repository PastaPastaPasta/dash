// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Byte buffer types

#ifndef GROVEDB_BYTE_BUFFER_HPP
#define GROVEDB_BYTE_BUFFER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace grovedb {

// Non-owning view of bytes (like std::span but C++17 compatible)
class ByteSpan {
public:
    using value_type = std::uint8_t;
    using pointer = const std::uint8_t*;
    using reference = const std::uint8_t&;
    using iterator = pointer;
    using size_type = std::size_t;

    // Default constructor (empty span)
    constexpr ByteSpan() noexcept : data_(nullptr), size_(0) {}

    // From pointer and size
    constexpr ByteSpan(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    // From vector
    ByteSpan(const std::vector<std::uint8_t>& v) noexcept
        : data_(v.data()), size_(v.size()) {}

    // From string_view
    ByteSpan(std::string_view sv) noexcept
        : data_(reinterpret_cast<const std::uint8_t*>(sv.data())), size_(sv.size()) {}

    // From string literal (deduce size at compile time)
    template <std::size_t N>
    constexpr ByteSpan(const char (&str)[N]) noexcept
        : data_(reinterpret_cast<const std::uint8_t*>(str)), size_(N - 1) {}

    // From std::array
    template <std::size_t N>
    constexpr ByteSpan(const std::array<std::uint8_t, N>& arr) noexcept
        : data_(arr.data()), size_(N) {}

    // Accessors
    [[nodiscard]] constexpr pointer data() const noexcept { return data_; }
    [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    // Element access
    [[nodiscard]] constexpr reference operator[](size_type idx) const noexcept {
        return data_[idx];
    }

    // Iterators
    [[nodiscard]] constexpr iterator begin() const noexcept { return data_; }
    [[nodiscard]] constexpr iterator end() const noexcept { return data_ + size_; }

    // Subspan
    [[nodiscard]] constexpr ByteSpan subspan(size_type offset, size_type count) const noexcept {
        return ByteSpan(data_ + offset, count);
    }

    [[nodiscard]] constexpr ByteSpan subspan(size_type offset) const noexcept {
        return ByteSpan(data_ + offset, size_ - offset);
    }

    // Convert to vector (copy)
    [[nodiscard]] std::vector<std::uint8_t> to_vector() const {
        return std::vector<std::uint8_t>(data_, data_ + size_);
    }

    // Convert to string_view
    [[nodiscard]] std::string_view to_string_view() const noexcept {
        return std::string_view(reinterpret_cast<const char*>(data_), size_);
    }

    // Comparison
    [[nodiscard]] bool operator==(const ByteSpan& other) const noexcept {
        if (size_ != other.size_) return false;
        if (size_ == 0) return true;
        return std::memcmp(data_, other.data_, size_) == 0;
    }

    [[nodiscard]] bool operator!=(const ByteSpan& other) const noexcept {
        return !(*this == other);
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
};

// Convenience type alias for owned byte data
using ByteVector = std::vector<std::uint8_t>;

// 32-byte hash type
struct Hash {
    static constexpr std::size_t SIZE = 32;
    std::array<std::uint8_t, SIZE> bytes;

    // Default constructor (zero-initialized)
    Hash() noexcept : bytes{} {}

    // From raw array
    explicit Hash(const std::uint8_t (&data)[SIZE]) noexcept {
        std::memcpy(bytes.data(), data, SIZE);
    }

    // From array
    explicit Hash(const std::array<std::uint8_t, SIZE>& arr) noexcept : bytes(arr) {}

    // Convert to ByteSpan
    [[nodiscard]] ByteSpan as_span() const noexcept {
        return ByteSpan(bytes.data(), SIZE);
    }

    // Access underlying data
    [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes.data(); }
    [[nodiscard]] std::uint8_t* data() noexcept { return bytes.data(); }

    // Comparison
    [[nodiscard]] bool operator==(const Hash& other) const noexcept {
        return bytes == other.bytes;
    }

    [[nodiscard]] bool operator!=(const Hash& other) const noexcept {
        return bytes != other.bytes;
    }

    // Check if all zeros
    [[nodiscard]] bool is_zero() const noexcept {
        for (auto b : bytes) {
            if (b != 0) return false;
        }
        return true;
    }
};

} // namespace grovedb

#endif // GROVEDB_BYTE_BUFFER_HPP
