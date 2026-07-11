// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! Tests for the pure C++ GroveDB proof verifier (src/platform/proof/)
//! against JSON vectors generated from the Rust dashpay/grovedb v5.0.0
//! implementation by the platform-test-vectors crate
//! (github.com/PastaPastaPasta/dash-platform-test-vectors).

#include <test/data/platform/element_vectors.json.h>
#include <test/data/platform/grovedb_proof_vectors.json.h>
#include <test/data/platform/merk_proof_vectors.json.h>

#include <platform/proof/grovedb.h>
#include <platform/proof/merk.h>
#include <platform/serialize.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using platform::Bytes;
using platform::grove::Element;
using platform::grove::GroveQuery;
using platform::grove::PathQuery;
using platform::merk::Hash256;
using platform::merk::QueryItem;

BOOST_FIXTURE_TEST_SUITE(platform_proof_tests, BasicTestingSetup)

namespace {

UniValue ReadJsonObject(const unsigned char* data, size_t size)
{
    UniValue v;
    BOOST_REQUIRE(v.read(std::string(data, data + size)));
    BOOST_REQUIRE(v.isObject());
    return v;
}

Bytes FromHex(const std::string& hex)
{
    const auto parsed{TryParseHex<uint8_t>(hex)};
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), "invalid hex in test vector: " + hex);
    return *parsed;
}

Hash256 HashFromHex(const std::string& hex)
{
    const Bytes bytes{FromHex(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    Hash256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

QueryItem ParseQueryItem(const UniValue& json)
{
    const std::string type{json["type"].get_str()};
    if (type == "key") return QueryItem::Key(FromHex(json["key"].get_str()));
    if (type == "range") return QueryItem::Range(FromHex(json["start"].get_str()), FromHex(json["end"].get_str()));
    if (type == "range_inclusive") {
        return QueryItem::RangeInclusive(FromHex(json["start"].get_str()), FromHex(json["end"].get_str()));
    }
    if (type == "range_full") return QueryItem::RangeFull();
    BOOST_REQUIRE_MESSAGE(false, "unknown query item type " + type);
    return QueryItem::RangeFull();
}

std::vector<QueryItem> ParseQueryItems(const UniValue& json)
{
    std::vector<QueryItem> items;
    for (size_t i = 0; i < json.size(); ++i) {
        items.push_back(ParseQueryItem(json[i]));
    }
    return items;
}

GroveQuery ParseGroveQuery(const UniValue& json)
{
    GroveQuery query;
    query.items = ParseQueryItems(json["items"]);
    query.left_to_right = json["left_to_right"].get_bool();
    const UniValue& subquery_path{json["default_subquery_path"]};
    if (!subquery_path.isNull()) {
        std::vector<Bytes> segments;
        for (size_t i = 0; i < subquery_path.size(); ++i) {
            segments.push_back(FromHex(subquery_path[i].get_str()));
        }
        query.default_subquery_branch.subquery_path = std::move(segments);
    }
    const UniValue& subquery{json["default_subquery"]};
    if (!subquery.isNull()) {
        query.default_subquery_branch.subquery = std::make_shared<GroveQuery>(ParseGroveQuery(subquery));
    }
    const UniValue& conditional{json["conditional_subquery_branches"]};
    if (!conditional.isNull()) {
        for (size_t i = 0; i < conditional.size(); ++i) {
            const UniValue& entry{conditional[i]};
            GroveQuery::ConditionalBranch branch;
            branch.item = ParseQueryItem(entry["item"]);
            const UniValue& branch_path{entry["subquery_path"]};
            if (!branch_path.isNull()) {
                std::vector<Bytes> segments;
                for (size_t j = 0; j < branch_path.size(); ++j) {
                    segments.push_back(FromHex(branch_path[j].get_str()));
                }
                branch.branch.subquery_path = std::move(segments);
            }
            const UniValue& branch_subquery{entry["subquery"]};
            if (!branch_subquery.isNull()) {
                branch.branch.subquery = std::make_shared<GroveQuery>(ParseGroveQuery(branch_subquery));
            }
            query.conditional_subquery_branches.push_back(std::move(branch));
        }
    }
    return query;
}

PathQuery ParsePathQuery(const UniValue& json)
{
    PathQuery query;
    const UniValue& path{json["path"]};
    for (size_t i = 0; i < path.size(); ++i) {
        query.path.push_back(FromHex(path[i].get_str()));
    }
    query.query = ParseGroveQuery(json["query"]);
    const UniValue& limit{json["limit"]};
    if (!limit.isNull()) query.limit = static_cast<uint16_t>(limit.getInt<int>());
    return query;
}

//! Outcome of verifying a (possibly corrupted) proof against a vector's
//! query and expected output.
enum class VectorOutcome {
    //! Verification returned an error, or the reconstructed root hash does
    //! not match the trusted one (a verifier bound to a trusted root hash
    //! rejects either way).
    DETECTED,
    //! Verification succeeded and root hash + results match the vector.
    MATCHES,
    //! Verification succeeded with the expected root hash but different
    //! results — for a corrupted proof this would be a forgery.
    DIFFERS,
};

//! Runs a merk vector and classifies the outcome.
VectorOutcome RunMerkVector(const UniValue& vec, const Bytes& proof, std::string& error)
{
    const std::vector<QueryItem> items{ParseQueryItems(vec["query"]["items"])};
    std::optional<uint16_t> limit;
    if (!vec["query"]["limit"].isNull()) limit = static_cast<uint16_t>(vec["query"]["limit"].getInt<int>());
    const bool left_to_right{vec["query"]["left_to_right"].get_bool()};
    const auto proof_version{static_cast<uint16_t>(vec["proof_version"].getInt<int>())};

    platform::merk::VerifyResult result;
    if (!platform::merk::ExecuteProof(proof, items, limit, left_to_right, proof_version, result, error)) {
        return VectorOutcome::DETECTED;
    }
    if (result.root_hash != HashFromHex(vec["expected_root_hash_hex"].get_str())) {
        error = "root hash mismatch";
        return VectorOutcome::DETECTED;
    }
    const UniValue& expected{vec["expected_results"]};
    if (result.result_set.size() != expected.size()) {
        error = "result count mismatch";
        return VectorOutcome::DIFFERS;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (result.result_set[i].key != FromHex(expected[i]["key_hex"].get_str()) ||
            result.result_set[i].value != FromHex(expected[i]["value_hex"].get_str())) {
            error = "result entry mismatch";
            return VectorOutcome::DIFFERS;
        }
    }
    return VectorOutcome::MATCHES;
}

//! Runs a grovedb vector and classifies the outcome.
VectorOutcome RunGroveVector(const UniValue& vec, const Bytes& proof, std::string& error)
{
    const PathQuery query{ParsePathQuery(vec["path_query"])};
    // Options matching Rust GroveDb::verify_query_raw, which produced the
    // expected results.
    const platform::grove::VerifyOptions options{
        .verify_proof_succinctness = false,
        .include_empty_trees_in_result = true,
    };
    platform::grove::GroveVerifyResult result;
    if (!platform::grove::VerifyQuery(proof, query, options, result, error)) return VectorOutcome::DETECTED;
    if (result.root_hash != HashFromHex(vec["expected_root_hash_hex"].get_str())) {
        error = "root hash mismatch";
        return VectorOutcome::DETECTED;
    }
    const UniValue& expected{vec["expected_results"]};
    if (result.results.size() != expected.size()) {
        error = "result count mismatch";
        return VectorOutcome::DIFFERS;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        const UniValue& entry{expected[i]};
        std::vector<Bytes> expected_path;
        for (size_t j = 0; j < entry["path"].size(); ++j) {
            expected_path.push_back(FromHex(entry["path"][j].get_str()));
        }
        if (result.results[i].path != expected_path ||
            result.results[i].key != FromHex(entry["key_hex"].get_str()) ||
            result.results[i].value != FromHex(entry["element_hex"].get_str())) {
            error = "result entry mismatch";
            return VectorOutcome::DIFFERS;
        }
    }
    return VectorOutcome::MATCHES;
}

} // namespace

BOOST_AUTO_TEST_CASE(element_vectors)
{
    const UniValue doc{ReadJsonObject(json_tests::element_vectors, sizeof(json_tests::element_vectors))};
    const UniValue& vectors{doc["vectors"]};
    BOOST_REQUIRE(vectors.size() > 0);
    for (size_t i = 0; i < vectors.size(); ++i) {
        const UniValue& vec{vectors[i]};
        const std::string name{vec["name"].get_str()};
        const Bytes serialized{FromHex(vec["serialized_hex"].get_str())};

        Element element;
        std::string error;
        BOOST_CHECK_MESSAGE(platform::grove::DecodeElement(serialized, element, error),
                            name + ": " + error);

        // Spot checks on semantics for known vectors.
        if (name == "item_no_flags") {
            BOOST_CHECK(element.type == Element::Type::ITEM);
            BOOST_CHECK(element.item_value == Bytes({'h', 'e', 'l', 'l', 'o'}));
            BOOST_CHECK(!element.flags.has_value());
        } else if (name == "item_with_flags") {
            BOOST_CHECK(element.type == Element::Type::ITEM);
            BOOST_CHECK(element.item_value == Bytes({1, 2, 3}));
            BOOST_CHECK((element.flags == std::optional<Bytes>{{7, 8}}));
        } else if (name == "item_large_value") {
            BOOST_CHECK(element.type == Element::Type::ITEM);
            BOOST_CHECK_EQUAL(element.item_value.size(), 300U);
        } else if (name == "sum_item_positive") {
            BOOST_CHECK(element.type == Element::Type::SUM_ITEM);
            BOOST_CHECK_EQUAL(element.sum_value, 1000);
        } else if (name == "sum_item_negative") {
            BOOST_CHECK(element.type == Element::Type::SUM_ITEM);
            BOOST_CHECK_EQUAL(element.sum_value, -42);
        } else if (name == "tree_empty_root_key") {
            BOOST_CHECK(element.type == Element::Type::TREE);
            BOOST_CHECK(!element.root_key.has_value());
        } else if (name == "tree_nonempty") {
            BOOST_CHECK(element.type == Element::Type::TREE);
            BOOST_REQUIRE(element.root_key.has_value());
            BOOST_CHECK_EQUAL(element.root_key->size(), 32U);
        } else if (name == "sum_tree_nonempty") {
            BOOST_CHECK(element.type == Element::Type::SUM_TREE);
            BOOST_CHECK(element.root_key.has_value());
            BOOST_CHECK_EQUAL(element.sum_value, 123);
        } else if (name == "reference_absolute") {
            BOOST_CHECK(element.type == Element::Type::REFERENCE);
            BOOST_CHECK(element.reference.type == platform::grove::ReferencePath::Type::ABSOLUTE_PATH);
            BOOST_CHECK_EQUAL(element.reference.path.size(), 3U);
        } else if (name == "reference_absolute_hops_flags") {
            BOOST_CHECK(element.type == Element::Type::REFERENCE);
            BOOST_CHECK(element.max_hops == std::optional<uint8_t>{3});
            BOOST_CHECK(element.flags == std::optional<Bytes>{{9}});
        } else if (name == "reference_upstream_root_height") {
            BOOST_CHECK(element.reference.type ==
                        platform::grove::ReferencePath::Type::UPSTREAM_ROOT_HEIGHT);
            BOOST_CHECK_EQUAL(element.reference.height, 1);
        } else if (name == "reference_sibling") {
            BOOST_CHECK(element.reference.type == platform::grove::ReferencePath::Type::SIBLING);
            BOOST_REQUIRE_EQUAL(element.reference.path.size(), 1U);
            BOOST_CHECK(element.reference.path[0] == Bytes({'s'}));
        }

        // Truncation must be rejected.
        if (serialized.size() > 1) {
            const Bytes truncated(serialized.begin(), serialized.end() - 1);
            Element ignored;
            BOOST_CHECK_MESSAGE(!platform::grove::DecodeElement(truncated, ignored, error),
                                name + ": truncated bytes must not decode");
        }
        // Trailing garbage must be rejected.
        Bytes extended{serialized};
        extended.push_back(0xff);
        Element ignored;
        BOOST_CHECK_MESSAGE(!platform::grove::DecodeElement(extended, ignored, error),
                            name + ": trailing bytes must not decode");
    }

    // Unknown / unsupported discriminants must fail with clear errors.
    for (const uint8_t discriminant : {uint8_t{5}, uint8_t{11}, uint8_t{15}, uint8_t{21}, uint8_t{255}}) {
        Element ignored;
        std::string error;
        BOOST_CHECK(!platform::grove::DecodeElement(Bytes{discriminant, 0x00}, ignored, error));
        BOOST_CHECK(!error.empty());
    }
}

BOOST_AUTO_TEST_CASE(merk_vectors)
{
    const UniValue doc{ReadJsonObject(json_tests::merk_proof_vectors, sizeof(json_tests::merk_proof_vectors))};
    const UniValue& vectors{doc["vectors"]};
    BOOST_REQUIRE(vectors.size() > 0);
    for (size_t i = 0; i < vectors.size(); ++i) {
        const UniValue& vec{vectors[i]};
        const std::string name{vec["name"].get_str()};
        const Bytes proof{FromHex(vec["proof_hex"].get_str())};

        std::string error;
        BOOST_CHECK_MESSAGE(RunMerkVector(vec, proof, error) == VectorOutcome::MATCHES,
                            name + ": " + error);

        // Negative: flipping any single byte of a merk proof must be
        // detected (decode error, root hash mismatch or result mismatch).
        for (size_t pos = 0; pos < proof.size(); ++pos) {
            Bytes corrupted{proof};
            corrupted[pos] ^= 0x01;
            std::string ignored_error;
            BOOST_CHECK_MESSAGE(RunMerkVector(vec, corrupted, ignored_error) == VectorOutcome::DETECTED,
                                name + ": corrupting byte " + std::to_string(pos) + " must fail");
        }

        // Negative: wrong expected root hash must fail.
        const std::vector<QueryItem> items{ParseQueryItems(vec["query"]["items"])};
        std::optional<uint16_t> limit;
        if (!vec["query"]["limit"].isNull()) limit = static_cast<uint16_t>(vec["query"]["limit"].getInt<int>());
        Hash256 wrong_root{HashFromHex(vec["expected_root_hash_hex"].get_str())};
        wrong_root[0] ^= 0x01;
        platform::merk::VerifyResult result;
        BOOST_CHECK_MESSAGE(!platform::merk::VerifyProof(proof, items, limit,
                                                         vec["query"]["left_to_right"].get_bool(),
                                                         wrong_root, result, error),
                            name + ": wrong expected root must fail");
    }
}

BOOST_AUTO_TEST_CASE(merk_query_completeness)
{
    // Security critical: a proof must not be able to silently omit results
    // that match the query. Take the proof that only covers key "i2" and
    // verify it against a query that also asks for "i4" (which exists in
    // the tree but is abridged in this proof): verification must fail.
    const UniValue doc{ReadJsonObject(json_tests::merk_proof_vectors, sizeof(json_tests::merk_proof_vectors))};
    const UniValue& vectors{doc["vectors"]};
    const UniValue* single_key{nullptr};
    for (size_t i = 0; i < vectors.size(); ++i) {
        if (vectors[i]["name"].get_str() == "items_single_key_present") single_key = &vectors[i];
    }
    BOOST_REQUIRE(single_key != nullptr);
    const Bytes proof{FromHex((*single_key)["proof_hex"].get_str())};
    const Hash256 expected_root{HashFromHex((*single_key)["expected_root_hash_hex"].get_str())};

    // The original query verifies.
    std::vector<QueryItem> items{QueryItem::Key(Bytes{'i', '2'})};
    platform::merk::VerifyResult result;
    std::string error;
    BOOST_CHECK_MESSAGE(
        platform::merk::VerifyProof(proof, items, std::nullopt, true, expected_root, result, error), error);
    BOOST_CHECK_EQUAL(result.result_set.size(), 1U);

    // Asking for an additional key the proof does not cover must fail even
    // though the proof itself is untampered.
    items.push_back(QueryItem::Key(Bytes{'i', '4'}));
    BOOST_CHECK(!platform::merk::VerifyProof(proof, items, std::nullopt, true, expected_root, result, error));

    // Same for a range that the proof cannot answer completely.
    items = {QueryItem::Range(Bytes{'i', '1'}, Bytes{'i', '5'})};
    BOOST_CHECK(!platform::merk::VerifyProof(proof, items, std::nullopt, true, expected_root, result, error));
}

BOOST_AUTO_TEST_CASE(merk_query_completeness_rtl)
{
    // The right-to-left mirror of merk_query_completeness: a proof generated
    // for a descending single-key query must not verify once the query also
    // asks for data the proof abridges.
    const UniValue doc{ReadJsonObject(json_tests::merk_proof_vectors, sizeof(json_tests::merk_proof_vectors))};
    const UniValue& vectors{doc["vectors"]};
    const UniValue* single_key{nullptr};
    for (size_t i = 0; i < vectors.size(); ++i) {
        if (vectors[i]["name"].get_str() == "items_single_key_present_rtl") single_key = &vectors[i];
    }
    BOOST_REQUIRE(single_key != nullptr);
    const Bytes proof{FromHex((*single_key)["proof_hex"].get_str())};
    const Hash256 expected_root{HashFromHex((*single_key)["expected_root_hash_hex"].get_str())};

    // The original descending query verifies.
    std::vector<QueryItem> items{QueryItem::Key(Bytes{'i', '2'})};
    platform::merk::VerifyResult result;
    std::string error;
    BOOST_CHECK_MESSAGE(
        platform::merk::VerifyProof(proof, items, std::nullopt, false, expected_root, result, error), error);
    BOOST_CHECK_EQUAL(result.result_set.size(), 1U);

    // Adding a key the proof does not cover must fail (items stay sorted
    // ascending; traversal order is what differs).
    items = {QueryItem::Key(Bytes{'i', '2'}), QueryItem::Key(Bytes{'i', '4'})};
    BOOST_CHECK(!platform::merk::VerifyProof(proof, items, std::nullopt, false, expected_root, result, error));

    // A range the proof cannot answer completely must fail as well.
    items = {QueryItem::Range(Bytes{'i', '1'}, Bytes{'i', '5'})};
    BOOST_CHECK(!platform::merk::VerifyProof(proof, items, std::nullopt, false, expected_root, result, error));
}

BOOST_AUTO_TEST_CASE(grovedb_vectors)
{
    const UniValue doc{
        ReadJsonObject(json_tests::grovedb_proof_vectors, sizeof(json_tests::grovedb_proof_vectors))};
    const UniValue& vectors{doc["vectors"]};
    BOOST_REQUIRE(vectors.size() > 0);
    for (size_t i = 0; i < vectors.size(); ++i) {
        const UniValue& vec{vectors[i]};
        const std::string name{vec["name"].get_str()};
        const Bytes proof{FromHex(vec["proof_hex"].get_str())};

        std::string error;
        BOOST_CHECK_MESSAGE(RunGroveVector(vec, proof, error) == VectorOutcome::MATCHES,
                            name + ": " + error);

        // Negative: flipping any single byte of the envelope must never
        // verify to a different root hash or result set. A few positions
        // are genuinely malleable in the Rust implementation as well (each
        // was confirmed against grovedb v5.0.0 verify_query_raw):
        // - the trailing ProveOptions bool of V0 envelopes (prover
        //   controlled by design, see the upstream security note) and a
        //   Push <-> PushInverted twin opcode pair (0x1c/0x1d) on a
        //   single-node layer may still verify to the *identical* output;
        // - lenient V0 proofs do not bind the serialized element bytes of a
        //   non-empty tree returned without descending into it (the very
        //   forgery vector the V1 strict mode closes with
        //   KVValueHashFeatureTypeWithChildHash nodes), so for
        //   root_tree_no_subquery_v0 some flips inside the element bytes
        //   verify with the same root hash but a different element.
        const bool is_v0{name.size() >= 3 && name.compare(name.size() - 3, 3, "_v0") == 0};
        const bool v0_tree_element_hole{name == "root_tree_no_subquery_v0"};
        for (size_t pos = 0; pos < proof.size(); ++pos) {
            Bytes corrupted{proof};
            corrupted[pos] ^= 0x01;
            std::string ignored_error;
            const VectorOutcome outcome{RunGroveVector(vec, corrupted, ignored_error)};
            if (outcome == VectorOutcome::DIFFERS) {
                BOOST_CHECK_MESSAGE(v0_tree_element_hole,
                                    name + ": corrupting byte " + std::to_string(pos) +
                                        " must not verify to different results");
            } else if (outcome == VectorOutcome::MATCHES) {
                const bool malleable{(is_v0 && pos == proof.size() - 1) || proof[pos] == 0x1c ||
                                     proof[pos] == 0x1d};
                BOOST_CHECK_MESSAGE(malleable, name + ": corrupting byte " + std::to_string(pos) +
                                                   " must fail");
            }
        }

        // Negative: wrong expected root hash must fail.
        const PathQuery query{ParsePathQuery(vec["path_query"])};
        Hash256 wrong_root{HashFromHex(vec["expected_root_hash_hex"].get_str())};
        wrong_root[31] ^= 0x80;
        platform::grove::GroveVerifyResult result;
        BOOST_CHECK_MESSAGE(
            !platform::grove::VerifyQueryWithRootHash(proof, query, platform::grove::VerifyOptions{},
                                                      wrong_root, result, error),
            name + ": wrong expected root must fail");
    }
}

BOOST_AUTO_TEST_CASE(grovedb_query_completeness)
{
    // A layered proof covering keys {i1, i3} must not verify for a query
    // that additionally asks for i4 (present in the tree, abridged in the
    // proof).
    const UniValue doc{
        ReadJsonObject(json_tests::grovedb_proof_vectors, sizeof(json_tests::grovedb_proof_vectors))};
    const UniValue& vectors{doc["vectors"]};
    const UniValue* two_level{nullptr};
    for (size_t i = 0; i < vectors.size(); ++i) {
        if (vectors[i]["name"].get_str() == "two_level_items_v1") two_level = &vectors[i];
    }
    BOOST_REQUIRE(two_level != nullptr);
    const Bytes proof{FromHex((*two_level)["proof_hex"].get_str())};

    PathQuery query{ParsePathQuery((*two_level)["path_query"])};
    query.query.items.push_back(QueryItem::Key(Bytes{'i', '4'}));
    std::sort(query.query.items.begin(), query.query.items.end(),
              [](const QueryItem& a, const QueryItem& b) { return a.key < b.key; });

    platform::grove::GroveVerifyResult result;
    std::string error;
    BOOST_CHECK(!platform::grove::VerifyQuery(proof, query, platform::grove::VerifyOptions{}, result, error));
}

BOOST_AUTO_TEST_CASE(grovedb_succinctness_option)
{
    // With the succinctness option enabled, exact-match vectors still verify
    // (their lower layers are all required by the query).
    const UniValue doc{
        ReadJsonObject(json_tests::grovedb_proof_vectors, sizeof(json_tests::grovedb_proof_vectors))};
    const UniValue& vectors{doc["vectors"]};
    for (size_t i = 0; i < vectors.size(); ++i) {
        const UniValue& vec{vectors[i]};
        const std::string name{vec["name"].get_str()};
        const Bytes proof{FromHex(vec["proof_hex"].get_str())};
        const PathQuery query{ParsePathQuery(vec["path_query"])};
        const platform::grove::VerifyOptions options{
            .verify_proof_succinctness = true,
            .include_empty_trees_in_result = true,
        };
        platform::grove::GroveVerifyResult result;
        std::string error;
        BOOST_CHECK_MESSAGE(platform::grove::VerifyQuery(proof, query, options, result, error),
                            name + ": " + error);
        BOOST_CHECK(result.root_hash == HashFromHex(vec["expected_root_hash_hex"].get_str()));
    }
}

BOOST_AUTO_TEST_CASE(reference_path_resolution)
{
    // grovedb-element/src/reference_path/mod.rs path_from_reference_path_type
    using platform::grove::ReferencePath;
    const std::vector<Bytes> current_path{Bytes{'a'}, Bytes{'b'}, Bytes{'c'}};
    const Bytes key{'k'};
    std::vector<Bytes> resolved;
    std::string error;

    ReferencePath absolute;
    absolute.type = ReferencePath::Type::ABSOLUTE_PATH;
    absolute.path = {Bytes{'x'}, Bytes{'y'}};
    BOOST_REQUIRE(absolute.Resolve(current_path, key, resolved, error));
    BOOST_CHECK(resolved == std::vector<Bytes>({Bytes{'x'}, Bytes{'y'}}));

    ReferencePath upstream_root;
    upstream_root.type = ReferencePath::Type::UPSTREAM_ROOT_HEIGHT;
    upstream_root.height = 2;
    upstream_root.path = {Bytes{'p'}, Bytes{'q'}};
    BOOST_REQUIRE(upstream_root.Resolve(current_path, key, resolved, error));
    BOOST_CHECK(resolved == std::vector<Bytes>({Bytes{'a'}, Bytes{'b'}, Bytes{'p'}, Bytes{'q'}}));

    ReferencePath parent_addition;
    parent_addition.type = ReferencePath::Type::UPSTREAM_ROOT_HEIGHT_WITH_PARENT_PATH_ADDITION;
    parent_addition.height = 2;
    parent_addition.path = {Bytes{'x'}, Bytes{'y'}};
    BOOST_REQUIRE(parent_addition.Resolve(current_path, key, resolved, error));
    BOOST_CHECK(resolved ==
                std::vector<Bytes>({Bytes{'a'}, Bytes{'b'}, Bytes{'x'}, Bytes{'y'}, Bytes{'c'}}));

    ReferencePath from_element;
    from_element.type = ReferencePath::Type::UPSTREAM_FROM_ELEMENT_HEIGHT;
    from_element.height = 1;
    from_element.path = {Bytes{'p'}, Bytes{'q'}};
    BOOST_REQUIRE(from_element.Resolve(current_path, key, resolved, error));
    BOOST_CHECK(resolved == std::vector<Bytes>({Bytes{'a'}, Bytes{'b'}, Bytes{'p'}, Bytes{'q'}}));

    ReferencePath cousin;
    cousin.type = ReferencePath::Type::COUSIN;
    cousin.path = {Bytes{'z'}};
    BOOST_REQUIRE(cousin.Resolve(current_path, key, resolved, error));
    BOOST_CHECK(resolved == std::vector<Bytes>({Bytes{'a'}, Bytes{'b'}, Bytes{'z'}, Bytes{'k'}}));

    ReferencePath removed_cousin;
    removed_cousin.type = ReferencePath::Type::REMOVED_COUSIN;
    removed_cousin.path = {Bytes{'m'}, Bytes{'n'}};
    BOOST_REQUIRE(removed_cousin.Resolve(current_path, key, resolved, error));
    BOOST_CHECK(resolved ==
                std::vector<Bytes>({Bytes{'a'}, Bytes{'b'}, Bytes{'m'}, Bytes{'n'}, Bytes{'k'}}));

    ReferencePath sibling;
    sibling.type = ReferencePath::Type::SIBLING;
    sibling.path = {Bytes{'s'}};
    BOOST_REQUIRE(sibling.Resolve(current_path, key, resolved, error));
    BOOST_CHECK(resolved == std::vector<Bytes>({Bytes{'a'}, Bytes{'b'}, Bytes{'c'}, Bytes{'s'}}));

    // Upstream height beyond the current path must fail.
    upstream_root.height = 4;
    BOOST_CHECK(!upstream_root.Resolve(current_path, key, resolved, error));
}

BOOST_AUTO_TEST_CASE(leb128_and_bincode_varints)
{
    // LEB128 write/read round trip (integer-encoding crate compatible).
    for (const uint64_t value : {uint64_t{0}, uint64_t{1}, uint64_t{127}, uint64_t{128},
                                 uint64_t{300}, uint64_t{1'000'000},
                                 std::numeric_limits<uint64_t>::max()}) {
        Bytes encoded;
        platform::WriteLEB128(encoded, value);
        platform::BytesReader reader{encoded};
        uint64_t decoded;
        BOOST_REQUIRE(reader.ReadLEB128(decoded));
        BOOST_CHECK_EQUAL(decoded, value);
        BOOST_CHECK(reader.IsEof());
    }

    // bincode-v2 big-endian varints: values below 251 are one byte; 251
    // introduces a big-endian u16 (spot values match the Rust encoder, cf.
    // SumItem(1000) serializing to 03fb07d000 with zigzag(1000) = 2000 =
    // 0x07d0).
    {
        const Bytes data{0xfa};
        platform::BytesReader reader{data};
        uint64_t v;
        BOOST_REQUIRE(reader.ReadBincodeVarint(v));
        BOOST_CHECK_EQUAL(v, 250U);
    }
    {
        const Bytes data{0xfb, 0x07, 0xd0};
        platform::BytesReader reader{data};
        int64_t v;
        BOOST_REQUIRE(reader.ReadBincodeVarintSigned(v));
        BOOST_CHECK_EQUAL(v, 1000);
    }
    {
        const Bytes data{0xfc, 0x00, 0x01, 0x00, 0x00};
        platform::BytesReader reader{data};
        uint64_t v;
        BOOST_REQUIRE(reader.ReadBincodeVarint(v));
        BOOST_CHECK_EQUAL(v, 65536U);
    }
    {
        // u128 marker is unsupported.
        const Bytes data{0xfe};
        platform::BytesReader reader{data};
        uint64_t v;
        BOOST_CHECK(!reader.ReadBincodeVarint(v));
    }
}

BOOST_AUTO_TEST_SUITE_END()
