// grovedb-cpp - C++17 wrapper for GroveDB FFI
// Element class

#ifndef GROVEDB_ELEMENT_HPP
#define GROVEDB_ELEMENT_HPP

#include <grovedb.h>
#include "byte_buffer.hpp"
#include "path.hpp"
#include "result.hpp"
#include "detail/ffi_helpers.hpp"
#include <cassert>
#include <cstdint>
#include <optional>
#include <utility>

namespace grovedb {

// Element type enumeration
enum class ElementType : std::uint8_t {
    Item = 0,
    Reference = 1,
    Tree = 2,
    SumItem = 3,
    SumTree = 4,
    BigSumTree = 5,
    CountTree = 6,
    CountSumTree = 7
};

// Convert FFI type to our enum
inline ElementType element_type_from_ffi(GroveDbElementType t) noexcept {
    return static_cast<ElementType>(static_cast<std::uint8_t>(t));
}

// Convert our enum to FFI type
inline GroveDbElementType element_type_to_ffi(ElementType t) noexcept {
    return static_cast<GroveDbElementType>(static_cast<std::uint8_t>(t));
}

// Reference path type enumeration
enum class ReferencePathType : std::uint8_t {
    AbsolutePath = 0,
    UpstreamRootHeight = 1,
    UpstreamRootHeightWithParentPathAddition = 2,
    UpstreamFromElementHeight = 3,
    Cousin = 4,
    RemovedCousin = 5,
    Sibling = 6
};

// Big sum value (i128 represented as two parts)
struct BigSum {
    std::uint64_t low;
    std::int64_t high;

    BigSum() noexcept : low(0), high(0) {}
    BigSum(std::uint64_t l, std::int64_t h) noexcept : low(l), high(h) {}

    // Convenience for small values
    explicit BigSum(std::int64_t value) noexcept
        : low(static_cast<std::uint64_t>(value)),
          high(value < 0 ? -1 : 0) {}

    [[nodiscard]] bool operator==(const BigSum& other) const noexcept {
        return low == other.low && high == other.high;
    }
};

// Element RAII wrapper
class Element {
public:
    // Default constructor creates a null element
    Element() noexcept : handle_(nullptr) {}

    // Move only
    Element(Element&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Element& operator=(Element&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    // Destructor
    ~Element() {
        reset();
    }

    // Release ownership and return handle
    GroveDbElement* release() noexcept {
        auto* h = handle_;
        handle_ = nullptr;
        return h;
    }

    // Reset to null
    void reset() noexcept {
        if (handle_ != nullptr) {
            grovedb_free_element(handle_);
            handle_ = nullptr;
        }
    }

    // Check if valid
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // Get underlying handle (for FFI calls)
    [[nodiscard]] GroveDbElement* get() noexcept { return handle_; }
    [[nodiscard]] const GroveDbElement* get() const noexcept { return handle_; }

    // Get element type
    [[nodiscard]] ElementType type() const noexcept {
        if (!handle_) return ElementType::Item;
        return element_type_from_ffi(grovedb_element_get_type(handle_));
    }

    // Type checking helpers
    [[nodiscard]] bool is_item() const noexcept { return type() == ElementType::Item; }
    [[nodiscard]] bool is_reference() const noexcept { return type() == ElementType::Reference; }
    [[nodiscard]] bool is_tree() const noexcept { return type() == ElementType::Tree; }
    [[nodiscard]] bool is_sum_item() const noexcept { return type() == ElementType::SumItem; }
    [[nodiscard]] bool is_sum_tree() const noexcept { return type() == ElementType::SumTree; }
    [[nodiscard]] bool is_big_sum_tree() const noexcept { return type() == ElementType::BigSumTree; }
    [[nodiscard]] bool is_count_tree() const noexcept { return type() == ElementType::CountTree; }
    [[nodiscard]] bool is_count_sum_tree() const noexcept { return type() == ElementType::CountSumTree; }

    // Value accessors (assert on wrong type)

    // For Item: get the value data
    [[nodiscard]] ByteSpan item_value() const noexcept {
        assert(is_item() && "item_value() called on non-Item element");
        assert(handle_ && "item_value() called on null element");
        return ByteSpan(handle_->value.data, handle_->value.len);
    }

    // For SumItem/SumTree/CountSumTree: get the sum value
    [[nodiscard]] std::int64_t sum_value() const noexcept {
        assert((is_sum_item() || is_sum_tree() || is_count_sum_tree()) &&
               "sum_value() called on element without sum");
        assert(handle_ && "sum_value() called on null element");
        return handle_->sum_value;
    }

    // For BigSumTree: get the big sum value
    [[nodiscard]] BigSum big_sum_value() const noexcept {
        assert(is_big_sum_tree() && "big_sum_value() called on non-BigSumTree element");
        assert(handle_ && "big_sum_value() called on null element");
        return BigSum{handle_->big_sum_value_low, handle_->big_sum_value_high};
    }

    // For CountTree/CountSumTree: get the count value
    [[nodiscard]] std::uint64_t count_value() const noexcept {
        assert((is_count_tree() || is_count_sum_tree()) &&
               "count_value() called on element without count");
        assert(handle_ && "count_value() called on null element");
        return handle_->count_value;
    }

    // Get element flags (if any)
    [[nodiscard]] std::optional<ByteSpan> flags() const noexcept {
        if (!handle_ || handle_->flags.len == 0) {
            return std::nullopt;
        }
        return ByteSpan(handle_->flags.data, handle_->flags.len);
    }

    // ========================================================================
    // Factory methods
    // ========================================================================

    // Create a new Item element
    // ByteSpan accepts string literals, std::string_view, std::vector<uint8_t>, etc.
    static Element new_item(ByteSpan data, std::optional<ByteSpan> flags = std::nullopt) {
        ByteBuffer data_buf = detail::to_byte_buffer(data.data(), data.size());
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        Element e;
        e.handle_ = grovedb_element_new_item(data_buf, flags_buf);
        return e;
    }

    // Create a new empty Tree element
    static Element new_tree(std::optional<ByteSpan> flags = std::nullopt) {
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        Element e;
        e.handle_ = grovedb_element_new_tree(flags_buf);
        return e;
    }

    // Create a new SumItem element
    static Element new_sum_item(std::int64_t value, std::optional<ByteSpan> flags = std::nullopt) {
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        Element e;
        e.handle_ = grovedb_element_new_sum_item(value, flags_buf);
        return e;
    }

    // Create a new empty SumTree element
    static Element new_sum_tree(std::optional<ByteSpan> flags = std::nullopt) {
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        Element e;
        e.handle_ = grovedb_element_new_sum_tree(flags_buf);
        return e;
    }

    // Create a new empty BigSumTree element
    static Element new_big_sum_tree(std::optional<ByteSpan> flags = std::nullopt) {
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        Element e;
        e.handle_ = grovedb_element_new_big_sum_tree(flags_buf);
        return e;
    }

    // Create a new empty CountTree element
    static Element new_count_tree(std::optional<ByteSpan> flags = std::nullopt) {
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        Element e;
        e.handle_ = grovedb_element_new_count_tree(flags_buf);
        return e;
    }

    // Create a new empty CountSumTree element
    static Element new_count_sum_tree(std::optional<ByteSpan> flags = std::nullopt) {
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        Element e;
        e.handle_ = grovedb_element_new_count_sum_tree(flags_buf);
        return e;
    }

    // Create a new Reference element with absolute path
    // Returns Result<Element> to handle path validation errors
    static Result<Element> new_reference(const Path& path, std::uint8_t max_hops = 0,
                                          std::optional<ByteSpan> flags = std::nullopt) {
        detail::FFIPath ffi_path(path);
        ByteBuffer flags_buf = flags
            ? detail::to_byte_buffer(flags->data(), flags->size())
            : detail::empty_byte_buffer();

        detail::ErrorGuard err;
        auto* handle = grovedb_element_new_reference(ffi_path.ptr(), max_hops, flags_buf, err.ptr());

        if (!err.ok() || handle == nullptr) {
            auto msg = err.take_message();
            return Error{error_code_from_ffi(err.code()), std::move(msg)};
        }

        Element e;
        e.handle_ = handle;
        return e;
    }

    // ========================================================================
    // Internal: create from FFI handle (takes ownership)
    // ========================================================================
    static Element from_handle(GroveDbElement* handle) noexcept {
        Element e;
        e.handle_ = handle;
        return e;
    }

private:
    GroveDbElement* handle_;
};

// Insert options
struct InsertOptions {
    bool validate_insertion_does_not_override = false;
    bool validate_insertion_does_not_override_tree = true;
    bool base_root_storage_is_free = true;

    // Convert to FFI struct
    [[nodiscard]] ::InsertOptions to_ffi() const noexcept {
        return ::InsertOptions{
            validate_insertion_does_not_override,
            validate_insertion_does_not_override_tree,
            base_root_storage_is_free
        };
    }
};

// Delete options
struct DeleteOptions {
    bool allow_deleting_non_empty_trees = false;
    bool deleting_non_empty_trees_returns_error = true;
    bool base_root_storage_is_free = true;
    bool validate_tree_at_path_exists = false;

    // Convert to FFI struct
    [[nodiscard]] ::DeleteOptions to_ffi() const noexcept {
        return ::DeleteOptions{
            allow_deleting_non_empty_trees,
            deleting_non_empty_trees_returns_error,
            base_root_storage_is_free,
            validate_tree_at_path_exists
        };
    }
};

} // namespace grovedb

#endif // GROVEDB_ELEMENT_HPP
