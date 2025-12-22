/**
 * GroveDB C++ Integration Tests
 *
 * These tests verify the C++ wrapper functionality using Catch2.
 */

#include <catch2/catch_test_macros.hpp>
#include <grovedb/grovedb.hpp>
#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;

/**
 * Helper to create a unique temporary database path
 */
std::string temp_db_path() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 999999);

    return (fs::temp_directory_path() / ("grovedb_test_" + std::to_string(dis(gen)))).string();
}

/**
 * RAII helper to clean up test database
 */
class TempDb {
public:
    TempDb() : path_(temp_db_path()) {}

    ~TempDb() {
        db_.reset();  // Close database first
        if (fs::exists(path_)) {
            fs::remove_all(path_);
        }
    }

    const std::string& path() const { return path_; }

    grovedb::Result<grovedb::Database>& open() {
        if (!db_) {
            db_ = std::make_unique<grovedb::Result<grovedb::Database>>(
                grovedb::Database::open(path_)
            );
        }
        return *db_;
    }

    grovedb::Database& db() {
        auto& result = open();
        REQUIRE(result.is_ok());
        return result.value();
    }

private:
    std::string path_;
    std::unique_ptr<grovedb::Result<grovedb::Database>> db_;
};

// =============================================================================
// Database Lifecycle Tests
// =============================================================================

TEST_CASE("Database lifecycle", "[database]") {
    TempDb temp;

    SECTION("Open creates new database") {
        auto result = grovedb::Database::open(temp.path());
        REQUIRE(result.is_ok());
        REQUIRE(result.value().is_open());
    }

    SECTION("Close and reopen preserves data") {
        {
            auto result = grovedb::Database::open(temp.path());
            REQUIRE(result.is_ok());
            auto& db = result.value();

            // Insert a tree
            grovedb::Path root{};
            auto tree = grovedb::Element::new_tree();
            auto insert_result = db.insert(root, grovedb::ByteSpan("test"), tree);
            REQUIRE(insert_result.is_ok());

            db.close();
        }

        // Reopen
        auto result = grovedb::Database::open(temp.path());
        REQUIRE(result.is_ok());
        auto& db = result.value();

        // Verify tree exists
        grovedb::Path root{};
        auto get_result = db.get(root, grovedb::ByteSpan("test"));
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());
    }
}

// =============================================================================
// Element Tests
// =============================================================================

TEST_CASE("Element types", "[element]") {
    SECTION("Item creation") {
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("hello"));
        REQUIRE(item.valid());
        REQUIRE(item.is_item());
        REQUIRE(item.type() == grovedb::ElementType::Item);
    }

    SECTION("Tree creation") {
        auto tree = grovedb::Element::new_tree();
        REQUIRE(tree.valid());
        REQUIRE(tree.is_tree());
        REQUIRE(tree.type() == grovedb::ElementType::Tree);
    }

    SECTION("SumItem creation") {
        auto sum_item = grovedb::Element::new_sum_item(42);
        REQUIRE(sum_item.valid());
        REQUIRE(sum_item.type() == grovedb::ElementType::SumItem);
    }

    SECTION("SumTree creation") {
        auto sum_tree = grovedb::Element::new_sum_tree();
        REQUIRE(sum_tree.valid());
        REQUIRE(sum_tree.type() == grovedb::ElementType::SumTree);
    }

    SECTION("Element with flags") {
        auto flags = std::vector<uint8_t>{0x01, 0x02, 0x03};
        auto item = grovedb::Element::new_item(
            grovedb::ByteSpan("data"),
            grovedb::ByteSpan(flags)
        );
        REQUIRE(item.valid());
        REQUIRE(item.is_item());
        // Flags are stored in the element and retrievable after storage
    }

    SECTION("BigSumTree creation") {
        auto big_sum_tree = grovedb::Element::new_big_sum_tree();
        REQUIRE(big_sum_tree.valid());
        REQUIRE(big_sum_tree.is_big_sum_tree());
        REQUIRE(big_sum_tree.type() == grovedb::ElementType::BigSumTree);
    }

    SECTION("CountTree creation") {
        auto count_tree = grovedb::Element::new_count_tree();
        REQUIRE(count_tree.valid());
        REQUIRE(count_tree.is_count_tree());
        REQUIRE(count_tree.type() == grovedb::ElementType::CountTree);
    }

    SECTION("CountSumTree creation") {
        auto count_sum_tree = grovedb::Element::new_count_sum_tree();
        REQUIRE(count_sum_tree.valid());
        REQUIRE(count_sum_tree.is_count_sum_tree());
        REQUIRE(count_sum_tree.type() == grovedb::ElementType::CountSumTree);
    }
}

// =============================================================================
// CRUD Operations Tests
// =============================================================================

TEST_CASE("Insert and Get operations", "[crud]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    SECTION("Insert and get item") {
        // Create tree first
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "users", tree).is_ok());

        // Insert item in tree
        grovedb::Path users{"users"};
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("Alice"));
        REQUIRE(db.insert(users, "alice", item).is_ok());

        // Get item
        auto result = db.get(users, "alice");
        REQUIRE(result.is_ok());
        REQUIRE(result->has_value());

        auto& elem = result->value();
        REQUIRE(elem.is_item());

        auto value = elem.item_value();
        REQUIRE(value.to_string_view() == "Alice");
    }

    SECTION("Insert and get sum item") {
        // Create sum tree
        auto sum_tree = grovedb::Element::new_sum_tree();
        REQUIRE(db.insert(root, "scores", sum_tree).is_ok());

        // Insert sum item
        grovedb::Path scores{"scores"};
        auto sum_item = grovedb::Element::new_sum_item(100);
        REQUIRE(db.insert(scores, "player1", sum_item).is_ok());

        // Get sum item
        auto result = db.get(scores, "player1");
        REQUIRE(result.is_ok());
        REQUIRE(result->has_value());

        auto& elem = result->value();
        REQUIRE(elem.type() == grovedb::ElementType::SumItem);
        REQUIRE(elem.sum_value() == 100);
    }

    SECTION("Get non-existent key returns empty optional") {
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "data", tree).is_ok());

        grovedb::Path data{"data"};
        auto result = db.get(data, "nonexistent");
        // The C++ wrapper returns success with std::nullopt for "not found"
        REQUIRE(result.is_ok());
        REQUIRE(!result->has_value());
    }
}

TEST_CASE("Delete operation", "[crud]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Setup: create tree and item
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "data", tree).is_ok());

    grovedb::Path data{"data"};
    auto item = grovedb::Element::new_item(grovedb::ByteSpan("value"));
    REQUIRE(db.insert(data, "key", item).is_ok());

    // Verify exists
    auto get_result = db.get(data, "key");
    REQUIRE(get_result.is_ok());
    REQUIRE(get_result->has_value());

    // Delete
    auto delete_result = db.delete_element(data, "key");
    REQUIRE(delete_result.is_ok());

    // Verify deleted (returns empty optional, not error)
    auto result = db.get(data, "key");
    REQUIRE(result.is_ok());
    REQUIRE(!result->has_value());
}

// =============================================================================
// Transaction Tests
// =============================================================================

TEST_CASE("Transaction operations", "[transaction]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Setup tree
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "tx_test", tree).is_ok());
    grovedb::Path tx_path{"tx_test"};

    SECTION("Transaction commit persists changes") {
        {
            auto tx_result = db.start_transaction();
            REQUIRE(tx_result.is_ok());
            auto& tx = tx_result.value();

            auto item = grovedb::Element::new_item(grovedb::ByteSpan("committed"));
            REQUIRE(db.insert(tx_path, "commit_key", item, {}, &tx).is_ok());

            REQUIRE(tx.commit().is_ok());
        }

        // Verify persisted
        auto result = db.get(tx_path, "commit_key");
        REQUIRE(result.is_ok());
        REQUIRE(result->has_value());
    }

    SECTION("Transaction rollback discards changes") {
        {
            auto tx_result = db.start_transaction();
            REQUIRE(tx_result.is_ok());
            auto& tx = tx_result.value();

            auto item = grovedb::Element::new_item(grovedb::ByteSpan("rolled_back"));
            REQUIRE(db.insert(tx_path, "rollback_key", item, {}, &tx).is_ok());

            REQUIRE(tx.rollback().is_ok());
        }

        // Verify not persisted (returns empty optional)
        auto result = db.get(tx_path, "rollback_key");
        REQUIRE(result.is_ok());
        REQUIRE(!result->has_value());
    }

    SECTION("Transaction abort on destruction") {
        {
            auto tx_result = db.start_transaction();
            REQUIRE(tx_result.is_ok());
            auto& tx = tx_result.value();

            auto item = grovedb::Element::new_item(grovedb::ByteSpan("aborted"));
            REQUIRE(db.insert(tx_path, "abort_key", item, {}, &tx).is_ok());

            // Let transaction destruct without commit
        }

        // Verify not persisted (returns empty optional)
        auto result = db.get(tx_path, "abort_key");
        REQUIRE(result.is_ok());
        REQUIRE(!result->has_value());
    }
}

// =============================================================================
// Root Hash Tests
// =============================================================================

TEST_CASE("Root hash", "[hash]") {
    TempDb temp;
    auto& db = temp.db();

    SECTION("Empty database has consistent hash") {
        auto hash_result = db.root_hash();
        REQUIRE(hash_result.is_ok());
        // Empty db should have some hash (could be zeros or computed empty hash)
    }

    SECTION("Hash changes after insert") {
        auto hash1_result = db.root_hash();
        REQUIRE(hash1_result.is_ok());
        auto hash1 = hash1_result.value();

        // Insert something
        grovedb::Path root{};
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "tree1", tree).is_ok());

        auto hash2_result = db.root_hash();
        REQUIRE(hash2_result.is_ok());
        auto hash2 = hash2_result.value();

        // Hashes should differ
        REQUIRE(hash1.bytes != hash2.bytes);
    }
}

// =============================================================================
// Path Tests
// =============================================================================

TEST_CASE("Path operations", "[path]") {
    SECTION("Empty path") {
        grovedb::Path root{};
        REQUIRE(root.empty());
        REQUIRE(root.size() == 0);
    }

    SECTION("Path from initializer list") {
        grovedb::Path path{"users", "active"};
        REQUIRE(!path.empty());
        REQUIRE(path.size() == 2);
    }

    SECTION("Child path") {
        grovedb::Path parent{"users"};
        auto child = parent.child(grovedb::ByteSpan("alice"));
        REQUIRE(child.size() == 2);
    }
}

// =============================================================================
// Query Tests
// =============================================================================

TEST_CASE("Query operations", "[query]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Setup: create tree with items
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "items", tree).is_ok());

    grovedb::Path items{"items"};
    for (int i = 0; i < 5; i++) {
        auto key = "key" + std::to_string(i);
        auto value = "value" + std::to_string(i);
        auto item = grovedb::Element::new_item(grovedb::ByteSpan(value));
        REQUIRE(db.insert(items, grovedb::ByteSpan(key), item).is_ok());
    }

    SECTION("Query with range full") {
        grovedb::PathQueryBuilder query(items);
        query.add_range_full();

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 5);
    }

    SECTION("Query with limit") {
        grovedb::PathQueryBuilder query(items);
        query.add_range_full()
             .set_limit(3);

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 3);
    }

    SECTION("Query single key") {
        grovedb::PathQueryBuilder query(items);
        query.add_key("key2");

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 1);
    }
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_CASE("Error handling", "[error]") {
    TempDb temp;
    auto& db = temp.db();

    SECTION("Get from non-existent path returns error") {
        grovedb::Path bad_path{"nonexistent", "path"};
        auto result = db.get(bad_path, "key");
        REQUIRE(result.is_error());
        // Error should indicate path not found
    }

    SECTION("Insert into non-existent tree fails") {
        grovedb::Path bad_path{"nonexistent"};
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("value"));
        auto result = db.insert(bad_path, "key", item);
        REQUIRE(result.is_error());
    }
}

// =============================================================================
// ByteSpan Tests
// =============================================================================

TEST_CASE("ByteSpan operations", "[bytespan]") {
    SECTION("From string literal") {
        grovedb::ByteSpan span("hello");
        REQUIRE(span.size() == 5);
        REQUIRE(span.to_string_view() == "hello");
    }

    SECTION("From string_view") {
        std::string_view sv = "world";
        grovedb::ByteSpan span(sv);
        REQUIRE(span.size() == 5);
    }

    SECTION("From vector") {
        std::vector<uint8_t> vec = {1, 2, 3, 4, 5};
        grovedb::ByteSpan span(vec);
        REQUIRE(span.size() == 5);
    }

    SECTION("To vector") {
        grovedb::ByteSpan span("test");
        auto vec = span.to_vector();
        REQUIRE(vec.size() == 4);
    }

    SECTION("Empty span") {
        grovedb::ByteSpan span;
        REQUIRE(span.empty());
        REQUIRE(span.size() == 0);
    }
}

// =============================================================================
// Flush Tests
// =============================================================================

TEST_CASE("Flush operation", "[database]") {
    TempDb temp;
    auto& db = temp.db();

    SECTION("Flush succeeds on open database") {
        auto result = db.flush();
        REQUIRE(result.is_ok());
    }

    SECTION("Flush after insert") {
        grovedb::Path root{};
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "data", tree).is_ok());

        auto result = db.flush();
        REQUIRE(result.is_ok());
    }
}

// =============================================================================
// Batch Operations Tests
// =============================================================================

TEST_CASE("Batch operations", "[batch]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    SECTION("BatchBuilder creation and validity") {
        grovedb::BatchBuilder batch;
        REQUIRE(batch.valid());
        REQUIRE(batch.empty());
        REQUIRE(batch.size() == 0);
    }

    SECTION("Element creation and destruction without batch") {
        // Test that element creation and destruction works without batch
        auto tree = grovedb::Element::new_tree();
        REQUIRE(tree.valid());
        REQUIRE(tree.is_tree());
        // tree goes out of scope and gets freed here
    }

    SECTION("Element survives after regular insert") {
        // Test that element can be freed after being used in a regular insert
        auto tree = grovedb::Element::new_tree();
        void* tree_ptr = (void*)tree.get();

        // Use element in regular insert (borrowing semantics)
        auto insert_result = db.insert(root, "test_tree", tree);
        REQUIRE(insert_result.is_ok());
        REQUIRE(tree.get() == tree_ptr);  // Pointer should not change

        // Now free the tree element - this should work
        tree.reset();
        REQUIRE(tree.get() == nullptr);
    }

    SECTION("Batch insert single tree") {
        grovedb::BatchBuilder batch;

        auto tree = grovedb::Element::new_tree();
        auto add_result = batch.add_insert(root, "batch_tree", tree);
        REQUIRE(add_result.is_ok());
        REQUIRE(batch.size() == 1);

        // Free tree BEFORE applying batch - the batch should have copied the data
        tree.reset();
        REQUIRE(tree.get() == nullptr);

        // Now apply the batch - should still work because batch has a copy of element data
        auto apply_result = db.apply_batch(batch);
        REQUIRE(apply_result.is_ok());

        // Verify tree was created
        auto get_result = db.get(root, "batch_tree");
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());
        REQUIRE(get_result->value().is_tree());
    }

    SECTION("Batch insert multiple elements") {
        grovedb::BatchBuilder batch;

        // Create tree and items in batch
        auto tree = grovedb::Element::new_tree();
        REQUIRE(batch.add_insert(root, "users", std::move(tree)).is_ok());

        grovedb::Path users{"users"};
        auto item1 = grovedb::Element::new_item(grovedb::ByteSpan("Alice"));
        REQUIRE(batch.add_insert(users, "alice", std::move(item1)).is_ok());

        auto item2 = grovedb::Element::new_item(grovedb::ByteSpan("Bob"));
        REQUIRE(batch.add_insert(users, "bob", std::move(item2)).is_ok());

        REQUIRE(batch.size() == 3);

        // Apply batch
        auto apply_result = db.apply_batch(batch);
        REQUIRE(apply_result.is_ok());

        // Verify all elements were created
        auto alice = db.get(users, "alice");
        REQUIRE(alice.is_ok());
        REQUIRE(alice->has_value());
        REQUIRE(alice->value().item_value().to_string_view() == "Alice");

        auto bob = db.get(users, "bob");
        REQUIRE(bob.is_ok());
        REQUIRE(bob->has_value());
        REQUIRE(bob->value().item_value().to_string_view() == "Bob");
    }

    SECTION("Batch delete") {
        // Setup: create tree and item
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "data", tree).is_ok());

        grovedb::Path data{"data"};
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("value"));
        REQUIRE(db.insert(data, "key", item).is_ok());

        // Verify exists
        auto get_result = db.get(data, "key");
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());

        // Delete via batch
        grovedb::BatchBuilder batch;
        REQUIRE(batch.add_delete(data, "key").is_ok());
        REQUIRE(batch.size() == 1);

        auto apply_result = db.apply_batch(batch);
        REQUIRE(apply_result.is_ok());

        // Verify deleted (returns empty optional)
        auto result = db.get(data, "key");
        REQUIRE(result.is_ok());
        REQUIRE(!result->has_value());
    }

    SECTION("Batch with transaction") {
        // Setup tree outside transaction
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "tx_batch", tree).is_ok());

        grovedb::Path tx_path{"tx_batch"};

        // Create batch in transaction
        auto tx_result = db.start_transaction();
        REQUIRE(tx_result.is_ok());
        auto& tx = tx_result.value();

        grovedb::BatchBuilder batch;
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("tx_value"));
        REQUIRE(batch.add_insert(tx_path, "tx_key", std::move(item)).is_ok());

        auto apply_result = db.apply_batch(batch, &tx);
        REQUIRE(apply_result.is_ok());

        // Commit transaction
        REQUIRE(tx.commit().is_ok());

        // Verify persisted
        auto get_result = db.get(tx_path, "tx_key");
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());
    }

    SECTION("Batch insert_only inserts element") {
        // Note: InsertOnly in GroveDB means "insert assuming key doesn't exist"
        // (an optimization hint), NOT "only insert if key doesn't exist".
        // It will overwrite if key exists. Use InsertOrReplace for explicit semantics.
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "data", tree).is_ok());

        grovedb::Path data{"data"};

        // insert_only on non-existent key
        grovedb::BatchBuilder batch;
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("value"));
        REQUIRE(batch.add_insert_only(data, "new_key", std::move(item)).is_ok());

        auto apply_result = db.apply_batch(batch);
        REQUIRE(apply_result.is_ok());

        // Verify element was inserted
        auto get_result = db.get(data, "new_key");
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());
        REQUIRE(get_result->value().item_value().to_string_view() == "value");
    }

    SECTION("Batch replace updates existing key") {
        // Setup: create tree and item
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "data", tree).is_ok());

        grovedb::Path data{"data"};
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("original"));
        REQUIRE(db.insert(data, "existing", item).is_ok());

        // Replace existing key
        grovedb::BatchBuilder batch;
        auto new_item = grovedb::Element::new_item(grovedb::ByteSpan("replaced"));
        REQUIRE(batch.add_replace(data, "existing", std::move(new_item)).is_ok());

        auto apply_result = db.apply_batch(batch);
        REQUIRE(apply_result.is_ok());

        // Verify value was replaced
        auto get_result = db.get(data, "existing");
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());
        REQUIRE(get_result->value().item_value().to_string_view() == "replaced");
    }

    SECTION("Batch move semantics") {
        grovedb::BatchBuilder batch1;
        auto tree = grovedb::Element::new_tree();
        REQUIRE(batch1.add_insert(root, "move_test", std::move(tree)).is_ok());
        REQUIRE(batch1.size() == 1);

        // Move batch
        grovedb::BatchBuilder batch2 = std::move(batch1);
        REQUIRE(batch2.valid());
        REQUIRE(batch2.size() == 1);
        REQUIRE(!batch1.valid());  // NOLINT - testing moved-from state

        // Apply moved batch
        auto apply_result = db.apply_batch(batch2);
        REQUIRE(apply_result.is_ok());
    }

    SECTION("Empty batch apply succeeds") {
        grovedb::BatchBuilder batch;
        REQUIRE(batch.empty());

        auto apply_result = db.apply_batch(batch);
        REQUIRE(apply_result.is_ok());
    }
}

// =============================================================================
// Proof Operations Tests
// =============================================================================

TEST_CASE("Proof operations", "[proof]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Setup: create tree with items
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "data", tree).is_ok());

    grovedb::Path data{"data"};
    auto item = grovedb::Element::new_item(grovedb::ByteSpan("test_value"));
    REQUIRE(db.insert(data, "key1", item).is_ok());

    SECTION("Generate and verify single-key proof") {
        // Generate proof
        auto proof_result = grovedb::prove_key(db, data, grovedb::ByteSpan("key1"));
        REQUIRE(proof_result.is_ok());
        REQUIRE(!proof_result->empty());

        // Verify proof
        auto verify_result = grovedb::verify_key(proof_result.value(), data, grovedb::ByteSpan("key1"));
        REQUIRE(verify_result.is_ok());

        // Check we got results
        REQUIRE(verify_result->results.size() == 1);
        REQUIRE(verify_result->results[0].has_element());
        auto elem_type = verify_result->results[0].element_type();
        REQUIRE(elem_type.has_value());
        REQUIRE(elem_type.value() == grovedb::ElementType::Item);
        REQUIRE(verify_result->results[0].item_value().to_string_view() == "test_value");
    }

    SECTION("Proof root hash matches database root hash") {
        // Generate proof
        auto proof_result = grovedb::prove_key(db, data, grovedb::ByteSpan("key1"));
        REQUIRE(proof_result.is_ok());

        // Verify proof
        auto verify_result = grovedb::verify_key(proof_result.value(), data, grovedb::ByteSpan("key1"));
        REQUIRE(verify_result.is_ok());

        // Get database root hash
        auto db_hash_result = db.root_hash();
        REQUIRE(db_hash_result.is_ok());

        // Compare hashes
        REQUIRE(verify_result->root_hash.bytes == db_hash_result->bytes);
    }

    SECTION("Proof with path query") {
        // Add more items
        for (int i = 2; i <= 5; i++) {
            auto key = "key" + std::to_string(i);
            auto value = "value" + std::to_string(i);
            auto elem = grovedb::Element::new_item(grovedb::ByteSpan(value));
            REQUIRE(db.insert(data, grovedb::ByteSpan(key), elem).is_ok());
        }

        // Build query for all keys
        grovedb::PathQueryBuilder query(data);
        query.add_range_full();

        // Generate proof
        auto proof_result = grovedb::prove_query(db, query);
        REQUIRE(proof_result.is_ok());
        REQUIRE(!proof_result->empty());

        // Verify proof (need new query because prove_query consumes it)
        grovedb::PathQueryBuilder verify_query(data);
        verify_query.add_range_full();

        auto verify_result = grovedb::verify_query(proof_result.value(), verify_query);
        REQUIRE(verify_result.is_ok());
        REQUIRE(verify_result->results.size() == 5);
    }
}

// =============================================================================
// Reference Element Tests
// =============================================================================

TEST_CASE("Reference elements", "[element][reference]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    SECTION("Create absolute path reference") {
        // Create target tree
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "target", tree).is_ok());

        // Create item in target tree
        grovedb::Path target{"target"};
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("original_value"));
        REQUIRE(db.insert(target, "item", item).is_ok());

        // Create reference to target/item
        grovedb::Path ref_path{"target", "item"};
        auto ref_result = grovedb::Element::new_reference(ref_path, 0);
        REQUIRE(ref_result.is_ok());
        REQUIRE(ref_result->valid());
        REQUIRE(ref_result->is_reference());

        // Create refs tree and insert reference
        auto refs_tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "refs", refs_tree).is_ok());

        grovedb::Path refs{"refs"};
        REQUIRE(db.insert(refs, "myref", std::move(ref_result.value())).is_ok());

        // Getting the reference should resolve to the target's value
        auto get_result = db.get(refs, "myref");
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());
        REQUIRE(get_result->value().is_item());
        REQUIRE(get_result->value().item_value().to_string_view() == "original_value");
    }

    SECTION("Reference with flags") {
        // Create target
        auto tree = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "target2", tree).is_ok());

        grovedb::Path target{"target2"};
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("value"));
        REQUIRE(db.insert(target, "key", item).is_ok());

        // Create reference with flags
        grovedb::Path ref_path{"target2", "key"};
        std::vector<uint8_t> flags = {0xAB, 0xCD};
        auto ref_result = grovedb::Element::new_reference(ref_path, 0, grovedb::ByteSpan(flags));
        REQUIRE(ref_result.is_ok());
        REQUIRE(ref_result->valid());
    }

    SECTION("Reference with max hops") {
        // Create nested trees for hop testing
        auto tree1 = grovedb::Element::new_tree();
        REQUIRE(db.insert(root, "tree1", tree1).is_ok());

        grovedb::Path tree1_path{"tree1"};
        auto item = grovedb::Element::new_item(grovedb::ByteSpan("deep_value"));
        REQUIRE(db.insert(tree1_path, "item", item).is_ok());

        // Create reference with max_hops = 1
        grovedb::Path ref_path{"tree1", "item"};
        auto ref_result = grovedb::Element::new_reference(ref_path, 1);
        REQUIRE(ref_result.is_ok());
    }
}

// =============================================================================
// Query Ordering Tests
// =============================================================================

TEST_CASE("Query result ordering", "[query][ordering]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Create tree for items
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "sorted", tree).is_ok());

    grovedb::Path sorted{"sorted"};

    SECTION("Results are returned in key order") {
        // Insert items in random order
        std::vector<std::string> keys = {"cherry", "apple", "banana", "date", "elderberry"};
        for (const auto& key : keys) {
            auto item = grovedb::Element::new_item(grovedb::ByteSpan("value_" + key));
            REQUIRE(db.insert(sorted, grovedb::ByteSpan(key), item).is_ok());
        }

        // Query all
        grovedb::PathQueryBuilder query(sorted);
        query.add_range_full();

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 5);

        // Verify results are in sorted order
        auto& results = *result;
        REQUIRE(results[0].key_span().to_string_view() == "apple");
        REQUIRE(results[1].key_span().to_string_view() == "banana");
        REQUIRE(results[2].key_span().to_string_view() == "cherry");
        REQUIRE(results[3].key_span().to_string_view() == "date");
        REQUIRE(results[4].key_span().to_string_view() == "elderberry");
    }

    SECTION("Numeric keys sort lexicographically") {
        // Insert numeric keys (as strings, they sort lexicographically)
        for (int i = 0; i < 10; i++) {
            auto key = std::to_string(i);
            auto item = grovedb::Element::new_item(grovedb::ByteSpan(key));
            REQUIRE(db.insert(sorted, grovedb::ByteSpan(key), item).is_ok());
        }

        grovedb::PathQueryBuilder query(sorted);
        query.add_range_full();

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 10);

        // Lexicographic order: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
        auto& results = *result;
        REQUIRE(results[0].key_span().to_string_view() == "0");
        REQUIRE(results[9].key_span().to_string_view() == "9");
    }

    SECTION("Range query returns keys in order") {
        // Insert keys a through z
        for (char c = 'a'; c <= 'f'; c++) {
            std::string key(1, c);
            auto item = grovedb::Element::new_item(grovedb::ByteSpan(key));
            REQUIRE(db.insert(sorted, grovedb::ByteSpan(key), item).is_ok());
        }

        // Query range from b to e
        grovedb::PathQueryBuilder query(sorted);
        std::vector<uint8_t> start = {'b'};
        std::vector<uint8_t> end = {'e'};
        query.add_range_inclusive(grovedb::ByteSpan(start), grovedb::ByteSpan(end));

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 4);  // b, c, d, e

        auto& results = *result;
        REQUIRE(results[0].key_span().to_string_view() == "b");
        REQUIRE(results[1].key_span().to_string_view() == "c");
        REQUIRE(results[2].key_span().to_string_view() == "d");
        REQUIRE(results[3].key_span().to_string_view() == "e");
    }
}

// =============================================================================
// Concurrent Transaction Tests
// =============================================================================

TEST_CASE("Concurrent transactions", "[transaction][concurrent]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Setup tree
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "concurrent", tree).is_ok());
    grovedb::Path concurrent{"concurrent"};

    SECTION("Two transactions see their own writes") {
        auto tx1_result = db.start_transaction();
        auto tx2_result = db.start_transaction();
        REQUIRE(tx1_result.is_ok());
        REQUIRE(tx2_result.is_ok());
        auto& tx1 = tx1_result.value();
        auto& tx2 = tx2_result.value();

        // Insert in tx1
        auto item1 = grovedb::Element::new_item(grovedb::ByteSpan("tx1_value"));
        REQUIRE(db.insert(concurrent, "tx1_key", item1, {}, &tx1).is_ok());

        // Insert in tx2
        auto item2 = grovedb::Element::new_item(grovedb::ByteSpan("tx2_value"));
        REQUIRE(db.insert(concurrent, "tx2_key", item2, {}, &tx2).is_ok());

        // tx1 should see its own write but not tx2's
        auto get1_own = db.get(concurrent, "tx1_key", &tx1);
        REQUIRE(get1_own.is_ok());
        REQUIRE(get1_own->has_value());
        REQUIRE(get1_own->value().item_value().to_string_view() == "tx1_value");

        // tx2 should see its own write but not tx1's
        auto get2_own = db.get(concurrent, "tx2_key", &tx2);
        REQUIRE(get2_own.is_ok());
        REQUIRE(get2_own->has_value());
        REQUIRE(get2_own->value().item_value().to_string_view() == "tx2_value");

        // Clean up
        REQUIRE(tx1.rollback().is_ok());
        REQUIRE(tx2.rollback().is_ok());
    }

    SECTION("Committed data visible to new transactions") {
        // Insert and commit in first transaction
        {
            auto tx_result = db.start_transaction();
            REQUIRE(tx_result.is_ok());
            auto& tx = tx_result.value();

            auto item = grovedb::Element::new_item(grovedb::ByteSpan("committed_value"));
            REQUIRE(db.insert(concurrent, "committed_key", item, {}, &tx).is_ok());

            REQUIRE(tx.commit().is_ok());
        }

        // New transaction should see committed data
        auto tx_result = db.start_transaction();
        REQUIRE(tx_result.is_ok());
        auto& tx = tx_result.value();

        auto get_result = db.get(concurrent, "committed_key", &tx);
        REQUIRE(get_result.is_ok());
        REQUIRE(get_result->has_value());
        REQUIRE(get_result->value().item_value().to_string_view() == "committed_value");

        REQUIRE(tx.rollback().is_ok());
    }

    SECTION("Rolled back data not visible") {
        // Insert and rollback
        {
            auto tx_result = db.start_transaction();
            REQUIRE(tx_result.is_ok());
            auto& tx = tx_result.value();

            auto item = grovedb::Element::new_item(grovedb::ByteSpan("rolled_back_value"));
            REQUIRE(db.insert(concurrent, "rolled_back_key", item, {}, &tx).is_ok());

            REQUIRE(tx.rollback().is_ok());
        }

        // Should not be visible without transaction
        auto get_result = db.get(concurrent, "rolled_back_key");
        REQUIRE(get_result.is_ok());
        REQUIRE(!get_result->has_value());
    }
}

// =============================================================================
// Query Value Verification Tests
// =============================================================================

TEST_CASE("Query results value verification", "[query][values]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Create tree
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "values", tree).is_ok());

    grovedb::Path values{"values"};

    SECTION("Query results contain correct values") {
        // Insert items with known values
        std::vector<std::pair<std::string, std::string>> data = {
            {"key1", "first_value"},
            {"key2", "second_value"},
            {"key3", "third_value"},
            {"key4", "fourth_value"},
            {"key5", "fifth_value"}
        };

        for (const auto& [key, value] : data) {
            auto item = grovedb::Element::new_item(grovedb::ByteSpan(value));
            REQUIRE(db.insert(values, grovedb::ByteSpan(key), item).is_ok());
        }

        // Query all
        grovedb::PathQueryBuilder query(values);
        query.add_range_full();

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 5);

        // Verify each result has correct value
        auto& results = *result;
        REQUIRE(results[0].item_value().to_string_view() == "first_value");
        REQUIRE(results[1].item_value().to_string_view() == "second_value");
        REQUIRE(results[2].item_value().to_string_view() == "third_value");
        REQUIRE(results[3].item_value().to_string_view() == "fourth_value");
        REQUIRE(results[4].item_value().to_string_view() == "fifth_value");
    }

    SECTION("SumTree query returns sum values") {
        // Create sum tree
        auto sum_tree = grovedb::Element::new_sum_tree();
        REQUIRE(db.insert(root, "sums", sum_tree).is_ok());

        grovedb::Path sums{"sums"};

        // Insert sum items
        std::vector<std::pair<std::string, int64_t>> sum_data = {
            {"player1", 100},
            {"player2", 250},
            {"player3", 50}
        };

        for (const auto& [key, value] : sum_data) {
            auto sum_item = grovedb::Element::new_sum_item(value);
            REQUIRE(db.insert(sums, grovedb::ByteSpan(key), sum_item).is_ok());
        }

        // Query all
        grovedb::PathQueryBuilder query(sums);
        query.add_range_full();

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 3);

        // Verify sum values (results are in key order: player1, player2, player3)
        auto& results = *result;
        REQUIRE(results[0].sum_value() == 100);
        REQUIRE(results[1].sum_value() == 250);
        REQUIRE(results[2].sum_value() == 50);
    }

    SECTION("Query with limit returns correct subset") {
        // Insert 10 items
        for (int i = 0; i < 10; i++) {
            auto key = "item" + std::to_string(i);
            auto value = "value" + std::to_string(i);
            auto item = grovedb::Element::new_item(grovedb::ByteSpan(value));
            REQUIRE(db.insert(values, grovedb::ByteSpan(key), item).is_ok());
        }

        // Query with limit 3
        grovedb::PathQueryBuilder query(values);
        query.add_range_full().set_limit(3);

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 3);

        // Should get first 3 items in key order
        auto& results = *result;
        REQUIRE(results[0].key_span().to_string_view() == "item0");
        REQUIRE(results[1].key_span().to_string_view() == "item1");
        REQUIRE(results[2].key_span().to_string_view() == "item2");
    }

    SECTION("Query with offset and limit") {
        // Insert 10 items
        for (int i = 0; i < 10; i++) {
            auto key = "data" + std::to_string(i);
            auto value = "val" + std::to_string(i);
            auto item = grovedb::Element::new_item(grovedb::ByteSpan(value));
            REQUIRE(db.insert(values, grovedb::ByteSpan(key), item).is_ok());
        }

        // Query with offset 2 and limit 3
        grovedb::PathQueryBuilder query(values);
        query.add_range_full().set_offset(2).set_limit(3);

        auto result = db.query(query);
        REQUIRE(result.is_ok());
        REQUIRE(result->size() == 3);

        // Should skip first 2 and get next 3
        auto& results = *result;
        REQUIRE(results[0].key_span().to_string_view() == "data2");
        REQUIRE(results[1].key_span().to_string_view() == "data3");
        REQUIRE(results[2].key_span().to_string_view() == "data4");
    }
}

// =============================================================================
// Transaction State Regression Tests
// =============================================================================

TEST_CASE("Transaction state after rollback", "[transaction][state][regression]") {
    TempDb temp;
    auto& db = temp.db();
    grovedb::Path root{};

    // Setup tree
    auto tree = grovedb::Element::new_tree();
    REQUIRE(db.insert(root, "tx_state_test", tree).is_ok());

    SECTION("Transaction state is RolledBack after rollback") {
        auto tx_result = db.start_transaction();
        REQUIRE(tx_result.is_ok());
        auto& tx = tx_result.value();

        // Verify initial state
        REQUIRE(tx.state() == grovedb::TransactionState::Active);
        REQUIRE(tx.is_active());
        REQUIRE(tx.valid());

        // Rollback
        auto rollback_result = tx.rollback();
        REQUIRE(rollback_result.is_ok());

        // Verify state after rollback - should be RolledBack, not Active
        REQUIRE(tx.state() == grovedb::TransactionState::RolledBack);
        REQUIRE(!tx.is_active());
        REQUIRE(!tx.valid());  // valid() checks both handle and Active state
    }

    SECTION("Cannot commit after rollback") {
        auto tx_result = db.start_transaction();
        REQUIRE(tx_result.is_ok());
        auto& tx = tx_result.value();

        // Rollback first
        REQUIRE(tx.rollback().is_ok());

        // Attempt to commit should fail (state is RolledBack, not Active)
        auto commit_result = tx.commit();
        REQUIRE(commit_result.is_error());
        REQUIRE(commit_result.error().code() == grovedb::ErrorCode::InvalidParameter);
    }

    SECTION("Cannot rollback twice") {
        auto tx_result = db.start_transaction();
        REQUIRE(tx_result.is_ok());
        auto& tx = tx_result.value();

        // First rollback succeeds
        REQUIRE(tx.rollback().is_ok());

        // Second rollback should fail (not active anymore)
        auto second_rollback = tx.rollback();
        REQUIRE(second_rollback.is_error());
        REQUIRE(second_rollback.error().code() == grovedb::ErrorCode::InvalidParameter);
    }
}

// ============================================================================
// FFIPath type safety tests (internal helper)
// ============================================================================

#include <grovedb/detail/ffi_helpers.hpp>
#include <type_traits>

TEST_CASE("FFIPath type safety", "[ffi_helpers]") {
    SECTION("FFIPath is not movable") {
        // Regression test: FFIPath must not be movable because path_ contains
        // pointers into buffers_, and moving would leave dangling pointers.
        STATIC_REQUIRE_FALSE(std::is_move_constructible_v<grovedb::detail::FFIPath>);
        STATIC_REQUIRE_FALSE(std::is_move_assignable_v<grovedb::detail::FFIPath>);
    }

    SECTION("FFIPath is not copyable") {
        STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<grovedb::detail::FFIPath>);
        STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<grovedb::detail::FFIPath>);
    }
}
