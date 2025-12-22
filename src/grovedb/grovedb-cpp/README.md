# GroveDB C++ Wrapper

Header-only C++17 wrapper for the GroveDB FFI library. Provides a modern, type-safe interface with RAII resource management.

## Features

- **Header-Only** - No compilation required, just include the headers
- **Modern C++17** - Uses `std::variant`, `std::optional`, `[[nodiscard]]`
- **RAII** - Automatic resource management for all handles
- **Type-Safe** - Strongly typed Result<T> for error handling
- **Ergonomic** - Convenient overloads for paths, keys, and elements

## Requirements

- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- grovedb-ffi library (libgrovedb_ffi.so/dylib/dll)
- grovedb.h header from grovedb-ffi

## Build with CMake

```bash
# Build grovedb-ffi first
cd /path/to/grovedb
cargo build -p grovedb-ffi --release

# Build grovedb-cpp
cd grovedb-cpp
mkdir build && cd build
cmake -DGROVEDB_FFI_DIR=/path/to/grovedb/target/release ..
make

# Build with tests
cmake -DGROVEDB_FFI_DIR=/path/to/grovedb/target/release \
      -DGROVEDB_CPP_BUILD_TESTS=ON \
      -DGROVEDB_CPP_BUILD_EXAMPLES=ON ..
make
```

## Build with Autoconf

```bash
cd grovedb-cpp
autoreconf -i
./configure --with-grovedb-ffi=/path/to/ffi
make
```

## Usage

### Basic Example

```cpp
#include <grovedb/grovedb.hpp>
#include <iostream>

using namespace grovedb;

int main() {
    // Open database
    auto db_result = Database::open("/tmp/mydb");
    if (!db_result) {
        std::cerr << "Error: " << db_result.error().message() << "\n";
        return 1;
    }
    auto& db = db_result.value();

    // Create a tree at root
    Path root_path{};
    auto tree = Element::new_tree();
    db.insert(root_path, "users", tree);

    // Insert items
    Path users_path{"users"};
    db.insert(users_path, "alice", Element::new_item(ByteSpan("Alice Smith")));
    db.insert(users_path, "bob", Element::new_item(ByteSpan("Bob Jones")));

    // Read an item
    auto get_result = db.get(users_path, "alice");
    if (get_result && get_result->has_value()) {
        auto& elem = get_result->value();
        std::cout << "alice: " << elem.item_value().to_string_view() << "\n";
    }

    // Get root hash
    auto hash = db.root_hash();
    if (hash) {
        std::cout << "Root hash: ";
        for (auto b : hash->bytes) {
            printf("%02x", b);
        }
        std::cout << "\n";
    }

    return 0;
}
```

### Using Transactions

```cpp
// Start a transaction
auto tx_result = db.start_transaction();
if (!tx_result) {
    std::cerr << "Failed to start transaction\n";
    return 1;
}
auto& tx = tx_result.value();

// Make changes within transaction
db.insert(users_path, "charlie", Element::new_item(ByteSpan("Charlie")), {}, &tx);

// Changes are visible within the transaction
auto charlie = db.get(users_path, "charlie", &tx);

// Commit or rollback
auto commit_result = tx.commit();
if (!commit_result) {
    std::cerr << "Commit failed: " << commit_result.error().message() << "\n";
    // Transaction auto-aborts on destruction if not committed
}
```

### Using Queries

```cpp
// Create a query for all items in a path
PathQueryBuilder query(scores_path);
query.add_range_full()
     .set_limit(100);

auto results = db.query(query);
if (results) {
    for (const auto& item : results.value()) {
        std::cout << item.key_span().to_string_view() << ": "
                  << item.sum_value() << "\n";
    }
}
```

### Using Sum Trees

```cpp
// Create a sum tree
db.insert(root_path, "scores", Element::new_sum_tree());

Path scores_path{"scores"};
db.insert(scores_path, "alice", Element::new_sum_item(100));
db.insert(scores_path, "bob", Element::new_sum_item(85));
db.insert(scores_path, "charlie", Element::new_sum_item(92));

// Query to get all scores
PathQueryBuilder query(scores_path);
query.add_range_full();

auto results = db.query(query);
// The sum tree automatically tracks the total
```

## API Reference

### Database Class

```cpp
class Database {
    static Result<Database> open(std::string_view path);
    void close() noexcept;
    bool is_open() const noexcept;

    Result<void> flush();
    Result<Hash> root_hash(const Transaction* tx = nullptr) const;

    Result<void> insert(const Path& path, ByteSpan key, const Element& element,
                        const InsertOptions& options = {},
                        const Transaction* tx = nullptr);

    Result<std::optional<Element>> get(const Path& path, ByteSpan key,
                                        const Transaction* tx = nullptr) const;

    Result<void> delete_element(const Path& path, ByteSpan key,
                                 const DeleteOptions& options = {},
                                 const Transaction* tx = nullptr);

    Result<Transaction> start_transaction();
    Result<QueryResults> query(PathQueryBuilder& query, const Transaction* tx = nullptr);
};
```

### Element Class

```cpp
class Element {
    static Element new_item(ByteSpan data, std::optional<ByteSpan> flags = std::nullopt);
    static Element new_tree(std::optional<ByteSpan> flags = std::nullopt);
    static Element new_sum_item(int64_t value, std::optional<ByteSpan> flags = std::nullopt);
    static Element new_sum_tree(std::optional<ByteSpan> flags = std::nullopt);
    static Element new_big_sum_tree(std::optional<ByteSpan> flags = std::nullopt);
    static Element new_count_tree(std::optional<ByteSpan> flags = std::nullopt);
    static Element new_count_sum_tree(std::optional<ByteSpan> flags = std::nullopt);
    static Element new_reference(const Path& path, uint8_t max_hops = 0,
                                  std::optional<ByteSpan> flags = std::nullopt);

    ElementType type() const noexcept;
    bool is_item() const noexcept;
    bool is_tree() const noexcept;
    // ... other type checks

    ByteSpan item_value() const noexcept;      // For Item
    int64_t sum_value() const noexcept;        // For SumItem/SumTree/CountSumTree
    BigSum big_sum_value() const noexcept;     // For BigSumTree
    uint64_t count_value() const noexcept;     // For CountTree/CountSumTree
};
```

### Transaction Class

```cpp
class Transaction {
    bool valid() const noexcept;
    bool is_active() const noexcept;
    TransactionState state() const noexcept;

    Result<void> commit();
    Result<void> rollback();
    void abort() noexcept;
    // Auto-aborts on destruction if not committed
};
```

### Result<T> Template

```cpp
template<typename T>
class Result {
    bool is_ok() const noexcept;
    bool is_error() const noexcept;
    explicit operator bool() const noexcept;

    T& value();
    const T& value() const;
    const Error& error() const;

    T& operator*();
    T* operator->();
    T value_or(U&& default_value) const;
};
```

### Path Class

```cpp
class Path {
    Path();                                          // Empty path (root)
    Path(std::initializer_list<std::string_view>);  // From string literals
    Path(Container segments);                        // From vector

    size_t size() const noexcept;
    bool empty() const noexcept;
    Path child(ByteSpan segment) const;
    Path parent() const;
};
```

### ByteSpan Class

```cpp
class ByteSpan {
    ByteSpan();
    ByteSpan(const uint8_t* data, size_t len);
    ByteSpan(std::string_view sv);
    ByteSpan(const std::vector<uint8_t>& vec);
    ByteSpan(const char* literal);

    const uint8_t* data() const noexcept;
    size_t size() const noexcept;
    bool empty() const noexcept;

    std::string_view to_string_view() const noexcept;
    std::vector<uint8_t> to_vector() const;
};
```

## Error Handling

The library uses `Result<T>` for all fallible operations. Check success before accessing values:

```cpp
auto result = db.get(path, key);
if (result) {
    // Success - can access value
    if (result->has_value()) {
        auto& elem = result->value();
        // Use element
    }
} else {
    // Error - check error details
    const auto& err = result.error();
    std::cerr << "Error code: " << static_cast<int>(err.code())
              << " - " << err.message() << "\n";

    if (err.is_not_found()) {
        // Handle not found case
    }
}
```

## Header Files

- `grovedb/grovedb.hpp` - Main umbrella header (include this)
- `grovedb/database.hpp` - Database class
- `grovedb/element.hpp` - Element class and types
- `grovedb/transaction.hpp` - Transaction class
- `grovedb/query.hpp` - Query builder and results
- `grovedb/result.hpp` - Result and Error types
- `grovedb/path.hpp` - Path class
- `grovedb/byte_buffer.hpp` - ByteSpan and Hash types

## License

MIT License - see repository root for details.
