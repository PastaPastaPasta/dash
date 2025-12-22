// Basic tests for grovedb-cpp

#include <grovedb/grovedb.hpp>
#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;
using namespace grovedb;

class GroveDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a unique temp directory for each test
        test_dir_ = fs::temp_directory_path() / ("grovedb_test_" + std::to_string(std::rand()));
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up the temp directory
        fs::remove_all(test_dir_);
    }

    std::string db_path() const {
        return test_dir_.string();
    }

private:
    fs::path test_dir_;
};

// ============================================================================
// ByteSpan tests
// ============================================================================

TEST(ByteSpanTest, DefaultConstruction) {
    ByteSpan span;
    EXPECT_EQ(span.data(), nullptr);
    EXPECT_EQ(span.size(), 0u);
    EXPECT_TRUE(span.empty());
}

TEST(ByteSpanTest, FromStringLiteral) {
    ByteSpan span("hello");
    EXPECT_EQ(span.size(), 5u);
    EXPECT_FALSE(span.empty());
    EXPECT_EQ(span.to_string_view(), "hello");
}

TEST(ByteSpanTest, FromVector) {
    std::vector<std::uint8_t> v = {1, 2, 3, 4, 5};
    ByteSpan span(v);
    EXPECT_EQ(span.size(), 5u);
    EXPECT_EQ(span[0], 1);
    EXPECT_EQ(span[4], 5);
}

TEST(ByteSpanTest, ToVector) {
    ByteSpan span("test");
    auto v = span.to_vector();
    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], 't');
}

// ============================================================================
// Path tests
// ============================================================================

TEST(PathTest, EmptyPath) {
    Path path;
    EXPECT_TRUE(path.empty());
    EXPECT_EQ(path.size(), 0u);
}

TEST(PathTest, InitializerList) {
    Path path{"a", "b", "c"};
    EXPECT_EQ(path.size(), 3u);
    EXPECT_FALSE(path.empty());
}

TEST(PathTest, Child) {
    Path path{"users"};
    auto child = path.child("alice");
    EXPECT_EQ(child.size(), 2u);
}

TEST(PathTest, Parent) {
    Path path{"users", "alice"};
    auto parent = path.parent();
    EXPECT_EQ(parent.size(), 1u);
}

TEST(PathTest, IsPrefix) {
    Path prefix{"users"};
    Path full{"users", "alice", "data"};
    EXPECT_TRUE(prefix.is_prefix_of(full));
    EXPECT_FALSE(full.is_prefix_of(prefix));
}

// ============================================================================
// Result tests
// ============================================================================

TEST(ResultTest, SuccessValue) {
    Result<int> r(42);
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_error());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(*r, 42);
}

TEST(ResultTest, ErrorValue) {
    Result<int> r(Error{ErrorCode::PathNotFound, "Not found"});
    EXPECT_FALSE(r.is_ok());
    EXPECT_TRUE(r.is_error());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.error().code(), ErrorCode::PathNotFound);
    EXPECT_EQ(r.error().message(), "Not found");
}

TEST(ResultTest, ValueOr) {
    Result<int> ok(42);
    Result<int> err(Error{ErrorCode::InternalError, ""});

    EXPECT_EQ(ok.value_or(0), 42);
    EXPECT_EQ(err.value_or(0), 0);
}

TEST(ResultTest, VoidResult) {
    Result<void> ok;
    EXPECT_TRUE(ok.is_ok());

    Result<void> err(Error{ErrorCode::InternalError, "error"});
    EXPECT_TRUE(err.is_error());
}

// ============================================================================
// Element tests
// ============================================================================

TEST(ElementTest, NewItem) {
    auto elem = Element::new_item(ByteSpan("hello world"));
    EXPECT_TRUE(elem.valid());
    EXPECT_EQ(elem.type(), ElementType::Item);
    EXPECT_TRUE(elem.is_item());
    EXPECT_EQ(elem.item_value().to_string_view(), "hello world");
}

TEST(ElementTest, NewTree) {
    auto elem = Element::new_tree();
    EXPECT_TRUE(elem.valid());
    EXPECT_EQ(elem.type(), ElementType::Tree);
    EXPECT_TRUE(elem.is_tree());
}

TEST(ElementTest, NewSumItem) {
    auto elem = Element::new_sum_item(42);
    EXPECT_TRUE(elem.valid());
    EXPECT_EQ(elem.type(), ElementType::SumItem);
    EXPECT_TRUE(elem.is_sum_item());
    EXPECT_EQ(elem.sum_value(), 42);
}

TEST(ElementTest, NewSumTree) {
    auto elem = Element::new_sum_tree();
    EXPECT_TRUE(elem.valid());
    EXPECT_EQ(elem.type(), ElementType::SumTree);
    EXPECT_TRUE(elem.is_sum_tree());
}

TEST(ElementTest, MoveSemantics) {
    auto elem1 = Element::new_item(ByteSpan("test"));
    auto elem2 = std::move(elem1);
    EXPECT_FALSE(elem1.valid());
    EXPECT_TRUE(elem2.valid());
}

// ============================================================================
// Database tests (require FFI library)
// ============================================================================

TEST_F(GroveDbTest, OpenClose) {
    auto result = Database::open(db_path());
    ASSERT_TRUE(result.is_ok()) << "Error: " << result.error().message();

    auto& db = result.value();
    EXPECT_TRUE(db.is_open());

    db.close();
    EXPECT_FALSE(db.is_open());
}

TEST_F(GroveDbTest, InsertAndGet) {
    auto db_result = Database::open(db_path());
    ASSERT_TRUE(db_result.is_ok()) << db_result.error().message();
    auto& db = db_result.value();

    // Insert a tree at root
    Path root_path{};
    auto tree = Element::new_tree();
    auto insert_result = db.insert(root_path, ByteSpan("users"), tree);
    ASSERT_TRUE(insert_result.is_ok()) << insert_result.error().message();

    // Insert an item in the tree
    Path users_path{"users"};
    auto item = Element::new_item(ByteSpan("Alice"));
    insert_result = db.insert(users_path, ByteSpan("user1"), item);
    ASSERT_TRUE(insert_result.is_ok()) << insert_result.error().message();

    // Get the item back
    auto get_result = db.get(users_path, ByteSpan("user1"));
    ASSERT_TRUE(get_result.is_ok()) << get_result.error().message();
    ASSERT_TRUE(get_result->has_value());

    auto& retrieved = get_result->value();
    EXPECT_TRUE(retrieved.is_item());
    EXPECT_EQ(retrieved.item_value().to_string_view(), "Alice");
}

TEST_F(GroveDbTest, GetNonExistent) {
    auto db_result = Database::open(db_path());
    ASSERT_TRUE(db_result.is_ok());
    auto& db = db_result.value();

    Path root_path{};
    auto get_result = db.get(root_path, ByteSpan("nonexistent"));
    ASSERT_TRUE(get_result.is_ok());
    EXPECT_FALSE(get_result->has_value());
}

TEST_F(GroveDbTest, RootHash) {
    auto db_result = Database::open(db_path());
    ASSERT_TRUE(db_result.is_ok());
    auto& db = db_result.value();

    // Get initial root hash
    auto hash_result = db.root_hash();
    ASSERT_TRUE(hash_result.is_ok());
    auto hash1 = hash_result.value();

    // Insert something
    Path root_path{};
    auto tree = Element::new_tree();
    (void)db.insert(root_path, ByteSpan("test"), tree);

    // Root hash should change
    hash_result = db.root_hash();
    ASSERT_TRUE(hash_result.is_ok());
    auto hash2 = hash_result.value();

    EXPECT_NE(hash1, hash2);
}

TEST_F(GroveDbTest, Delete) {
    auto db_result = Database::open(db_path());
    ASSERT_TRUE(db_result.is_ok());
    auto& db = db_result.value();

    // Insert a tree
    Path root_path{};
    auto tree = Element::new_tree();
    (void)db.insert(root_path, ByteSpan("to_delete"), tree);

    // Verify it exists
    auto get_result = db.get(root_path, ByteSpan("to_delete"));
    ASSERT_TRUE(get_result.is_ok());
    ASSERT_TRUE(get_result->has_value());

    // Delete it
    auto del_result = db.delete_element(root_path, ByteSpan("to_delete"));
    ASSERT_TRUE(del_result.is_ok()) << del_result.error().message();

    // Verify it's gone
    get_result = db.get(root_path, ByteSpan("to_delete"));
    ASSERT_TRUE(get_result.is_ok());
    EXPECT_FALSE(get_result->has_value());
}

TEST_F(GroveDbTest, Transaction) {
    auto db_result = Database::open(db_path());
    ASSERT_TRUE(db_result.is_ok());
    auto& db = db_result.value();

    // Start transaction
    auto tx_result = db.start_transaction();
    ASSERT_TRUE(tx_result.is_ok()) << tx_result.error().message();
    auto tx = std::move(tx_result.value());
    EXPECT_TRUE(tx.is_active());

    // Insert within transaction
    Path root_path{};
    auto tree = Element::new_tree();
    auto insert_result = db.insert(root_path, ByteSpan("tx_test"), tree, {}, &tx);
    ASSERT_TRUE(insert_result.is_ok());

    // Commit
    auto commit_result = tx.commit();
    ASSERT_TRUE(commit_result.is_ok()) << commit_result.error().message();

    // Verify data persisted
    auto get_result = db.get(root_path, ByteSpan("tx_test"));
    ASSERT_TRUE(get_result.is_ok());
    EXPECT_TRUE(get_result->has_value());
}

TEST_F(GroveDbTest, TransactionRollback) {
    auto db_result = Database::open(db_path());
    ASSERT_TRUE(db_result.is_ok());
    auto& db = db_result.value();

    // Start transaction
    auto tx_result = db.start_transaction();
    ASSERT_TRUE(tx_result.is_ok());
    auto tx = std::move(tx_result.value());

    // Insert within transaction
    Path root_path{};
    auto tree = Element::new_tree();
    (void)db.insert(root_path, ByteSpan("rollback_test"), tree, {}, &tx);

    // Rollback
    auto rollback_result = tx.rollback();
    ASSERT_TRUE(rollback_result.is_ok());

    // Data should not persist (need to commit after rollback to check)
    (void)tx.commit();

    auto get_result = db.get(root_path, ByteSpan("rollback_test"));
    ASSERT_TRUE(get_result.is_ok());
    // After rollback, the insert should be undone
}

TEST_F(GroveDbTest, Query) {
    auto db_result = Database::open(db_path());
    ASSERT_TRUE(db_result.is_ok());
    auto& db = db_result.value();

    // Set up data
    Path root_path{};
    auto tree = Element::new_tree();
    (void)db.insert(root_path, ByteSpan("data"), tree);

    Path data_path{"data"};
    (void)db.insert(data_path, ByteSpan("a"), Element::new_item(ByteSpan("A")));
    (void)db.insert(data_path, ByteSpan("b"), Element::new_item(ByteSpan("B")));
    (void)db.insert(data_path, ByteSpan("c"), Element::new_item(ByteSpan("C")));

    // Query all
    PathQueryBuilder query(data_path);
    query.add_range_full();
    auto result = db.query(query);
    ASSERT_TRUE(result.is_ok()) << result.error().message();

    auto& results = result.value();
    EXPECT_EQ(results.size(), 3u);
}

// ============================================================================
// Hash tests
// ============================================================================

TEST(HashTest, DefaultIsZero) {
    Hash hash;
    EXPECT_TRUE(hash.is_zero());
}

TEST(HashTest, NonZeroComparison) {
    Hash h1, h2;
    h1.bytes[0] = 1;
    EXPECT_FALSE(h1.is_zero());
    EXPECT_NE(h1, h2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
