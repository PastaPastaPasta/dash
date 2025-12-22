// Basic usage example for grovedb-cpp

#include <grovedb/grovedb.hpp>
#include <iostream>
#include <iomanip>

using namespace grovedb;

// Helper to print a hash
void print_hash(const Hash& hash) {
    std::cout << std::hex << std::setfill('0');
    for (auto b : hash.bytes) {
        std::cout << std::setw(2) << static_cast<int>(b);
    }
    std::cout << std::dec << std::endl;
}

int main() {
    std::cout << "GroveDB C++ Wrapper Example\n";
    std::cout << "===========================\n\n";

    // Open (or create) a database
    auto db_result = Database::open("/tmp/grovedb_example");
    if (!db_result) {
        std::cerr << "Failed to open database: " << db_result.error().message() << "\n";
        return 1;
    }
    auto& db = db_result.value();
    std::cout << "Database opened successfully\n";

    // Get initial root hash
    auto hash_result = db.root_hash();
    if (hash_result) {
        std::cout << "Initial root hash: ";
        print_hash(hash_result.value());
    }

    // Create a tree structure
    std::cout << "\nCreating tree structure...\n";

    // Insert a tree at root level
    Path root_path{};
    auto tree = Element::new_tree();
    auto r = db.insert(root_path, ByteSpan("users"), tree);
    if (!r) {
        std::cerr << "Failed to insert tree: " << r.error().message() << "\n";
        return 1;
    }

    // Insert some items in the users tree
    Path users_path{"users"};
    db.insert(users_path, ByteSpan("alice"), Element::new_item(ByteSpan("Alice Smith")));
    db.insert(users_path, ByteSpan("bob"), Element::new_item(ByteSpan("Bob Jones")));
    db.insert(users_path, ByteSpan("charlie"), Element::new_item(ByteSpan("Charlie Brown")));

    std::cout << "Inserted 3 users\n";

    // Read back a user
    auto get_result = db.get(users_path, ByteSpan("alice"));
    if (get_result && get_result->has_value()) {
        auto& elem = get_result->value();
        std::cout << "Retrieved alice: " << elem.item_value().to_string_view() << "\n";
    }

    // Create a sum tree for tracking scores
    std::cout << "\nCreating sum tree for scores...\n";
    auto sum_tree = Element::new_sum_tree();
    db.insert(root_path, ByteSpan("scores"), sum_tree);

    // Insert sum items
    Path scores_path{"scores"};
    db.insert(scores_path, ByteSpan("alice"), Element::new_sum_item(100));
    db.insert(scores_path, ByteSpan("bob"), Element::new_sum_item(85));
    db.insert(scores_path, ByteSpan("charlie"), Element::new_sum_item(92));

    // Query all scores
    std::cout << "\nQuerying all scores:\n";
    PathQueryBuilder query(scores_path);
    query.add_range_full();
    auto query_result = db.query(query);
    if (query_result) {
        for (const auto& item : query_result.value()) {
            auto key = item.key_span();
            auto score = item.sum_value();
            std::cout << "  " << key.to_string_view() << ": " << score << "\n";
        }
    }

    // Using transactions
    std::cout << "\nUsing transactions...\n";
    {
        auto tx_result = db.start_transaction();
        if (tx_result) {
            auto& tx = tx_result.value();

            // Make changes within transaction
            db.insert(users_path, ByteSpan("dave"), Element::new_item(ByteSpan("Dave Wilson")), {}, &tx);

            // Read within transaction (sees uncommitted changes)
            auto dave = db.get(users_path, ByteSpan("dave"), &tx);
            if (dave && dave->has_value()) {
                std::cout << "Within transaction - dave exists\n";
            }

            // Commit the transaction
            auto commit = tx.commit();
            if (commit) {
                std::cout << "Transaction committed successfully\n";
            }
        }
    }

    // Verify dave exists after commit
    auto dave_check = db.get(users_path, ByteSpan("dave"));
    if (dave_check && dave_check->has_value()) {
        std::cout << "After commit - dave: " << dave_check->value().item_value().to_string_view() << "\n";
    }

    // Delete an element
    std::cout << "\nDeleting bob...\n";
    auto del_result = db.delete_element(users_path, ByteSpan("bob"));
    if (del_result) {
        std::cout << "Bob deleted successfully\n";
    }

    // Verify deletion
    auto bob_check = db.get(users_path, ByteSpan("bob"));
    if (bob_check && !bob_check->has_value()) {
        std::cout << "Confirmed: bob no longer exists\n";
    }

    // Final root hash
    hash_result = db.root_hash();
    if (hash_result) {
        std::cout << "\nFinal root hash: ";
        print_hash(hash_result.value());
    }

    // Flush to disk
    db.flush();
    std::cout << "\nDatabase flushed and closed\n";

    return 0;
}
