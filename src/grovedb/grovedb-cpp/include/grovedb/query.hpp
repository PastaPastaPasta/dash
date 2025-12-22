// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Query classes

#ifndef GROVEDB_QUERY_HPP
#define GROVEDB_QUERY_HPP

#include <grovedb.h>
#include "result.hpp"
#include "byte_buffer.hpp"
#include "path.hpp"
#include "element.hpp"
#include "detail/ffi_helpers.hpp"
#include <optional>
#include <cstdint>

namespace grovedb {

// Forward declaration
class Database;

// Query result item (view into QueryResults data)
class QueryResultItem {
public:
    QueryResultItem(const QueryResultElement* elem) noexcept : elem_(elem) {}

    // Check if element is present
    [[nodiscard]] bool has_element() const noexcept {
        return elem_ && elem_->has_element && elem_->element;
    }

    // Get path (if present)
    [[nodiscard]] std::optional<Path> path() const {
        if (!elem_ || elem_->path.count == 0) {
            return std::nullopt;
        }

        Path::Container segments;
        segments.reserve(elem_->path.count);
        for (std::size_t i = 0; i < elem_->path.count; ++i) {
            const auto& seg = elem_->path.segments[i];
            segments.emplace_back(seg.data, seg.data + seg.len);
        }
        return Path(std::move(segments));
    }

    // Get key (if present)
    [[nodiscard]] std::optional<ByteVector> key() const {
        if (!elem_ || elem_->key.len == 0) {
            return std::nullopt;
        }
        return ByteVector(elem_->key.data, elem_->key.data + elem_->key.len);
    }

    // Get key as span (valid only while QueryResults is alive)
    [[nodiscard]] ByteSpan key_span() const noexcept {
        if (!elem_) {
            return ByteSpan{};
        }
        return ByteSpan(elem_->key.data, elem_->key.len);
    }

    // Get element type (if element present)
    [[nodiscard]] std::optional<ElementType> element_type() const noexcept {
        if (!has_element()) {
            return std::nullopt;
        }
        return element_type_from_ffi(grovedb_element_get_type(elem_->element));
    }

    // Access the element data directly (borrowed, valid while QueryResults alive)
    // For Item elements
    [[nodiscard]] ByteSpan item_value() const noexcept {
        if (!has_element()) {
            return ByteSpan{};
        }
        return ByteSpan(elem_->element->value.data, elem_->element->value.len);
    }

    // For SumItem/SumTree/CountSumTree
    [[nodiscard]] std::int64_t sum_value() const noexcept {
        if (!has_element()) {
            return 0;
        }
        return elem_->element->sum_value;
    }

    // For CountTree/CountSumTree
    [[nodiscard]] std::uint64_t count_value() const noexcept {
        if (!has_element()) {
            return 0;
        }
        return elem_->element->count_value;
    }

    // For BigSumTree
    [[nodiscard]] BigSum big_sum_value() const noexcept {
        if (!has_element()) {
            return BigSum{};
        }
        return BigSum{elem_->element->big_sum_value_low, elem_->element->big_sum_value_high};
    }

private:
    const QueryResultElement* elem_;
};

// Query results container (RAII)
class QueryResults {
public:
    // Iterator
    class iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = QueryResultItem;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = QueryResultItem;

        iterator(const ::QueryResults* results, std::size_t index) noexcept
            : results_(results), index_(index) {}

        reference operator*() const noexcept {
            return QueryResultItem(grovedb_query_results_get(results_, index_));
        }

        iterator& operator++() noexcept { ++index_; return *this; }
        iterator operator++(int) noexcept { auto tmp = *this; ++index_; return tmp; }
        iterator& operator--() noexcept { --index_; return *this; }
        iterator operator--(int) noexcept { auto tmp = *this; --index_; return tmp; }

        iterator& operator+=(difference_type n) noexcept { index_ += n; return *this; }
        iterator& operator-=(difference_type n) noexcept { index_ -= n; return *this; }

        iterator operator+(difference_type n) const noexcept { return iterator(results_, index_ + n); }
        iterator operator-(difference_type n) const noexcept { return iterator(results_, index_ - n); }
        difference_type operator-(const iterator& other) const noexcept {
            return static_cast<difference_type>(index_) - static_cast<difference_type>(other.index_);
        }

        bool operator==(const iterator& other) const noexcept { return index_ == other.index_; }
        bool operator!=(const iterator& other) const noexcept { return index_ != other.index_; }
        bool operator<(const iterator& other) const noexcept { return index_ < other.index_; }
        bool operator>(const iterator& other) const noexcept { return index_ > other.index_; }
        bool operator<=(const iterator& other) const noexcept { return index_ <= other.index_; }
        bool operator>=(const iterator& other) const noexcept { return index_ >= other.index_; }

        reference operator[](difference_type n) const noexcept {
            return QueryResultItem(grovedb_query_results_get(results_, index_ + n));
        }

    private:
        const ::QueryResults* results_;
        std::size_t index_;
    };

    using const_iterator = iterator;

    // Default constructor
    QueryResults() noexcept : handle_(nullptr) {}

    // Move only
    QueryResults(QueryResults&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    QueryResults& operator=(QueryResults&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    QueryResults(const QueryResults&) = delete;
    QueryResults& operator=(const QueryResults&) = delete;

    // Destructor
    ~QueryResults() {
        reset();
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            grovedb_free_query_results(handle_);
            handle_ = nullptr;
        }
    }

    // Check if valid
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // Get number of results
    [[nodiscard]] std::size_t size() const noexcept {
        return handle_ ? grovedb_query_results_count(handle_) : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Get number of skipped elements (due to offset)
    [[nodiscard]] std::uint16_t skipped() const noexcept {
        return handle_ ? handle_->skipped : 0;
    }

    // Access element by index
    [[nodiscard]] QueryResultItem at(std::size_t index) const noexcept {
        return QueryResultItem(grovedb_query_results_get(handle_, index));
    }

    [[nodiscard]] QueryResultItem operator[](std::size_t index) const noexcept {
        return at(index);
    }

    // Iterators
    [[nodiscard]] iterator begin() const noexcept { return iterator(handle_, 0); }
    [[nodiscard]] iterator end() const noexcept { return iterator(handle_, size()); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return end(); }

    // Internal: create from FFI handle
    static QueryResults from_handle(::QueryResults* handle) noexcept {
        QueryResults r;
        r.handle_ = handle;
        return r;
    }

private:
    ::QueryResults* handle_;
};

// Path query builder
class PathQueryBuilder {
public:
    // Construct with path
    explicit PathQueryBuilder(const Path& path) : handle_(nullptr), consumed_(false) {
        detail::FFIPath ffi_path(path);
        detail::ErrorGuard err;
        handle_ = grovedb_path_query_new(ffi_path.ptr(), err.ptr());
    }

    // Convenience: construct with initializer list
    PathQueryBuilder(std::initializer_list<std::string_view> path)
        : handle_(nullptr), consumed_(false) {
        detail::FFIPath ffi_path(path);
        detail::ErrorGuard err;
        handle_ = grovedb_path_query_new(ffi_path.ptr(), err.ptr());
    }

    // Move only
    PathQueryBuilder(PathQueryBuilder&& other) noexcept
        : handle_(other.handle_), consumed_(other.consumed_) {
        other.handle_ = nullptr;
        other.consumed_ = true;
    }

    PathQueryBuilder& operator=(PathQueryBuilder&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            consumed_ = other.consumed_;
            other.handle_ = nullptr;
            other.consumed_ = true;
        }
        return *this;
    }

    PathQueryBuilder(const PathQueryBuilder&) = delete;
    PathQueryBuilder& operator=(const PathQueryBuilder&) = delete;

    // Destructor
    ~PathQueryBuilder() {
        reset();
    }

    void reset() noexcept {
        if (handle_ != nullptr && !consumed_) {
            grovedb_free_path_query(handle_);
        }
        handle_ = nullptr;
        consumed_ = true;
    }

    // Check if valid
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr && !consumed_; }

    // Get handle (for internal use)
    [[nodiscard]] grovedb_path_query_t* get() noexcept { return handle_; }
    [[nodiscard]] const grovedb_path_query_t* get() const noexcept { return handle_; }

    // Mark as consumed (called after query execution)
    void mark_consumed() noexcept { consumed_ = true; }

    // Release ownership
    grovedb_path_query_t* release() noexcept {
        auto* h = handle_;
        handle_ = nullptr;
        consumed_ = true;
        return h;
    }

    // ========================================================================
    // Query item configuration (builder pattern)
    // ========================================================================

    // Add a single key lookup
    PathQueryBuilder& add_key(ByteSpan key) {
        if (!valid()) return *this;

        ByteBuffer key_buf = detail::to_byte_buffer(key.data(), key.size());
        detail::ErrorGuard err;
        grovedb_path_query_add_key(handle_, key_buf, err.ptr());
        return *this;
    }

    // Add a range [start, end)
    PathQueryBuilder& add_range(ByteSpan start, ByteSpan end) {
        if (!valid()) return *this;

        ByteBuffer start_buf = detail::to_byte_buffer(start.data(), start.size());
        ByteBuffer end_buf = detail::to_byte_buffer(end.data(), end.size());
        detail::ErrorGuard err;
        grovedb_path_query_add_range(handle_, start_buf, end_buf, err.ptr());
        return *this;
    }

    // Add a range [start, end]
    PathQueryBuilder& add_range_inclusive(ByteSpan start, ByteSpan end) {
        if (!valid()) return *this;

        ByteBuffer start_buf = detail::to_byte_buffer(start.data(), start.size());
        ByteBuffer end_buf = detail::to_byte_buffer(end.data(), end.size());
        detail::ErrorGuard err;
        grovedb_path_query_add_range_inclusive(handle_, start_buf, end_buf, err.ptr());
        return *this;
    }


    // Add a full range (all keys)
    PathQueryBuilder& add_range_full() {
        if (!valid()) return *this;

        detail::ErrorGuard err;
        grovedb_path_query_add_range_full(handle_, err.ptr());
        return *this;
    }

    // Add a range from [start, ..)
    PathQueryBuilder& add_range_from(ByteSpan start) {
        if (!valid()) return *this;

        ByteBuffer start_buf = detail::to_byte_buffer(start.data(), start.size());
        detail::ErrorGuard err;
        grovedb_path_query_add_range_from(handle_, start_buf, err.ptr());
        return *this;
    }

    PathQueryBuilder& add_range_from(std::string_view start) {
        return add_range_from(ByteSpan(start));
    }

    // Add a range after (start, ..)
    PathQueryBuilder& add_range_after(ByteSpan start) {
        if (!valid()) return *this;

        ByteBuffer start_buf = detail::to_byte_buffer(start.data(), start.size());
        detail::ErrorGuard err;
        grovedb_path_query_add_range_after(handle_, start_buf, err.ptr());
        return *this;
    }

    PathQueryBuilder& add_range_after(std::string_view start) {
        return add_range_after(ByteSpan(start));
    }

    // Set result limit
    PathQueryBuilder& set_limit(std::uint16_t limit) {
        if (!valid()) return *this;

        grovedb_path_query_set_limit(handle_, limit);
        return *this;
    }

    // Set result offset
    PathQueryBuilder& set_offset(std::uint16_t offset) {
        if (!valid()) return *this;

        grovedb_path_query_set_offset(handle_, offset);
        return *this;
    }

private:
    grovedb_path_query_t* handle_;
    bool consumed_;
};

// ============================================================================
// Database query method implementations
// ============================================================================

inline Result<QueryResults> Database::query(PathQueryBuilder& builder, const Transaction* tx) {
    if (!is_open()) {
        return Error{ErrorCode::InvalidParameter, "Database not open"};
    }
    if (!builder.valid()) {
        return Error{ErrorCode::InvalidParameter, "Invalid query builder"};
    }

    detail::ErrorGuard err;
    ::QueryResults* results = grovedb_query(
        get(),
        builder.release(),
        tx ? tx->get() : nullptr,
        err.ptr()
    );
    builder.mark_consumed();

    if (!err.ok()) {
        return Error{error_code_from_ffi(err.code()), err.take_message()};
    }

    return QueryResults::from_handle(results);
}

inline Result<QueryResults> Database::query(PathQueryBuilder&& builder, const Transaction* tx) {
    return query(builder, tx);
}

} // namespace grovedb

#endif // GROVEDB_QUERY_HPP
