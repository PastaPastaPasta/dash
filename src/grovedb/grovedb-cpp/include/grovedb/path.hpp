// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Path class

#ifndef GROVEDB_PATH_HPP
#define GROVEDB_PATH_HPP

#include "byte_buffer.hpp"
#include <initializer_list>
#include <string_view>
#include <vector>

namespace grovedb {

// Path represents a location in the GroveDB tree hierarchy
// Each segment is a byte array key
class Path {
public:
    using Segment = std::vector<std::uint8_t>;
    using Container = std::vector<Segment>;
    using iterator = Container::const_iterator;
    using size_type = Container::size_type;

    // Default constructor (root path)
    Path() = default;

    // From initializer list of string_views (most common use)
    // Example: Path{"users", "alice", "data"}
    // Note: Use ByteSpan explicitly for non-string keys, e.g., Path{ByteSpan(key1), ByteSpan(key2)}
    Path(std::initializer_list<std::string_view> segments) {
        segments_.reserve(segments.size());
        for (const auto& sv : segments) {
            segments_.emplace_back(
                reinterpret_cast<const std::uint8_t*>(sv.data()),
                reinterpret_cast<const std::uint8_t*>(sv.data()) + sv.size()
            );
        }
    }

    // From vector of byte vectors
    explicit Path(Container segments) : segments_(std::move(segments)) {}

    // Access segments
    [[nodiscard]] const Container& segments() const noexcept { return segments_; }
    [[nodiscard]] size_type size() const noexcept { return segments_.size(); }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }

    // Element access
    [[nodiscard]] const Segment& operator[](size_type idx) const noexcept {
        return segments_[idx];
    }

    [[nodiscard]] const Segment& at(size_type idx) const {
        return segments_.at(idx);
    }

    // Iterators
    [[nodiscard]] iterator begin() const noexcept { return segments_.begin(); }
    [[nodiscard]] iterator end() const noexcept { return segments_.end(); }

    // Create child path by appending a key
    [[nodiscard]] Path child(ByteSpan key) const {
        Path result = *this;
        result.segments_.emplace_back(key.begin(), key.end());
        return result;
    }

    [[nodiscard]] Path child(std::string_view key) const {
        return child(ByteSpan(key));
    }

    // Create parent path by removing last segment
    [[nodiscard]] Path parent() const {
        if (segments_.empty()) {
            return *this;
        }
        Path result;
        result.segments_.assign(segments_.begin(), segments_.end() - 1);
        return result;
    }

    // Get last segment (key at this path)
    [[nodiscard]] ByteSpan last() const noexcept {
        if (segments_.empty()) {
            return ByteSpan{};
        }
        const auto& last_seg = segments_.back();
        return ByteSpan(last_seg.data(), last_seg.size());
    }

    // Comparison
    [[nodiscard]] bool operator==(const Path& other) const noexcept {
        return segments_ == other.segments_;
    }

    [[nodiscard]] bool operator!=(const Path& other) const noexcept {
        return segments_ != other.segments_;
    }

    // Check if this path is a prefix of another
    [[nodiscard]] bool is_prefix_of(const Path& other) const noexcept {
        if (segments_.size() > other.segments_.size()) {
            return false;
        }
        for (size_type i = 0; i < segments_.size(); ++i) {
            if (segments_[i] != other.segments_[i]) {
                return false;
            }
        }
        return true;
    }

    // Concatenate paths
    [[nodiscard]] Path operator+(const Path& other) const {
        Path result = *this;
        result.segments_.insert(
            result.segments_.end(),
            other.segments_.begin(),
            other.segments_.end()
        );
        return result;
    }

    Path& operator+=(const Path& other) {
        segments_.insert(
            segments_.end(),
            other.segments_.begin(),
            other.segments_.end()
        );
        return *this;
    }

private:
    Container segments_;
};

// Factory function for root path
inline Path root_path() {
    return Path{};
}

} // namespace grovedb

#endif // GROVEDB_PATH_HPP
