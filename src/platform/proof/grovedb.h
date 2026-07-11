// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_PROOF_GROVEDB_H
#define BITCOIN_PLATFORM_PROOF_GROVEDB_H

#include <platform/proof/merk.h>
#include <platform/serialize.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

//! Layered GroveDB proof verification, a pure C++ port of the "verify"
//! feature slice of dashpay/grovedb tag v5.0.0:
//! - grovedb/src/operations/proof/mod.rs    (GroveDBProof envelope, bincode
//!   standard()+big_endian() config, V0/V1 layer proofs)
//! - grovedb/src/operations/proof/verify.rs (recursive layer verification)
//! - grovedb-element/src/element/mod.rs + serialize.rs (Element decoding)
//! - grovedb-element/src/reference_path/mod.rs (ReferencePathType)
//! - grovedb/src/query/mod.rs               (PathQuery::query_items_at_path)
namespace platform::grove {

using merk::Hash256;

//! ReferencePathType (grovedb-element/src/reference_path/mod.rs). Variant
//! order matches the Rust enum declaration (bincode discriminants 0..6).
struct ReferencePath {
    enum class Type : uint8_t {
        ABSOLUTE_PATH = 0,
        UPSTREAM_ROOT_HEIGHT = 1,
        UPSTREAM_ROOT_HEIGHT_WITH_PARENT_PATH_ADDITION = 2,
        UPSTREAM_FROM_ELEMENT_HEIGHT = 3,
        COUSIN = 4,
        REMOVED_COUSIN = 5,
        SIBLING = 6,
    };
    Type type{Type::ABSOLUTE_PATH};
    //! First tuple field of the upstream variants.
    uint8_t height{0};
    //! Path segments; for COUSIN / SIBLING the single key lives in path[0].
    std::vector<Bytes> path;

    //! Resolves the reference to an absolute path, given the path of the
    //! tree that holds the reference and the reference's own key. Mirrors
    //! grovedb-element/src/reference_path/mod.rs
    //! path_from_reference_path_type. Returns false on invalid input (e.g.
    //! upstream height exceeding the current path length).
    bool Resolve(const std::vector<Bytes>& current_path, const Bytes& key,
                 std::vector<Bytes>& out, std::string& error) const;
};

//! GroveDB Element (grovedb-element/src/element/mod.rs). Only the variants
//! the platform GUI needs are decoded fully (Item, Reference, Tree, SumItem,
//! SumTree); the remaining variants are recognized and rejected with clear
//! errors. Bincode discriminants follow the Rust declaration order.
struct Element {
    enum class Type : uint8_t {
        ITEM = 0,
        REFERENCE = 1,
        TREE = 2,
        SUM_ITEM = 3,
        SUM_TREE = 4,
        // Recognized but unsupported: BigSumTree=5, CountTree=6,
        // CountSumTree=7, ProvableCountTree=8, ItemWithSumItem=9,
        // ProvableCountSumTree=10, CommitmentTree=11, MmrTree=12,
        // BulkAppendTree=13, DenseAppendOnlyFixedSizeTree=14,
        // NonCounted=15, NotSummed=16, NotCountedOrSummed=17,
        // ReferenceWithSumItem=18, ProvableSumTree=19,
        // ProvableCountProvableSumTree=20.
    };
    Type type{Type::ITEM};
    Bytes item_value;                //!< ITEM payload
    int64_t sum_value{0};            //!< SUM_ITEM value / SUM_TREE total
    std::optional<Bytes> root_key;   //!< TREE / SUM_TREE root key
    ReferencePath reference;         //!< REFERENCE target
    std::optional<uint8_t> max_hops; //!< REFERENCE MaxReferenceHop
    std::optional<Bytes> flags;      //!< element flags (all variants)

    bool IsTree() const { return type == Type::TREE || type == Type::SUM_TREE; }
    bool IsEmptyTree() const { return IsTree() && !root_key.has_value(); }
    bool IsItem() const { return type == Type::ITEM || type == Type::SUM_ITEM; }
};

//! Decodes a serialized Element (bincode standard()+big_endian()+no_limit,
//! grovedb-element/src/element/serialize.rs Element::deserialize). The whole
//! input must be consumed. Returns false with error set on failure or when
//! the element variant is not supported by this verifier.
bool DecodeElement(Span<const uint8_t> bytes, Element& out, std::string& error);

//! Query with optional default subquery branch
//! (grovedb-query/src/query.rs Query + subquery_branch.rs SubqueryBranch).
//! Conditional subquery branches are supported through `conditional`.
struct GroveQuery {
    struct SubqueryBranch {
        //! Optional path of intermediate keys leading to the subquery.
        std::optional<std::vector<Bytes>> subquery_path;
        std::shared_ptr<GroveQuery> subquery;

        bool Empty() const { return !subquery_path.has_value() && !subquery; }
    };
    struct ConditionalBranch {
        merk::QueryItem item;
        SubqueryBranch branch;
    };

    std::vector<merk::QueryItem> items;
    bool left_to_right{true};
    SubqueryBranch default_subquery_branch;
    std::vector<ConditionalBranch> conditional_subquery_branches;
};

//! PathQuery (grovedb/src/query/mod.rs PathQuery + SizedQuery). Offsets are
//! not supported by the proof system and are therefore not modeled.
struct PathQuery {
    std::vector<Bytes> path;
    GroveQuery query;
    std::optional<uint16_t> limit;
};

//! One verified result entry (grovedb/src/operations/proof/util.rs
//! ProvedPathKeyValue): the path of the subtree that holds the entry, the
//! key, and the serialized element bytes.
struct ProvedPathKeyValue {
    std::vector<Bytes> path;
    Bytes key;
    Bytes value;
    Hash256 value_hash{};
};

struct GroveVerifyResult {
    Hash256 root_hash{};
    std::vector<ProvedPathKeyValue> results;
    //! Decoded GroveDBProof envelope version (0 = lenient V0, 1 = strict V1).
    //! Network verification requires V1; V0 is retained only for the decoder
    //! unit tests (see platform_proof_tests).
    uint16_t envelope_version{1};
};

//! Verification options (merk/src/proofs/query/verify.rs VerifyOptions
//! subset; absence_proofs_for_non_existing_searched_keys is handled by the
//! caller since raw results already prove absences cryptographically).
struct VerifyOptions {
    //! Reject proofs with lower layers the query did not require.
    bool verify_proof_succinctness{false};
    //! Include empty tree elements in the result set.
    bool include_empty_trees_in_result{true};
};

//! Verifies a full layered GroveDB proof (V0 or V1 envelope) against a path
//! query, mirroring GroveDb::verify_query_raw /
//! grovedb/src/operations/proof/verify.rs. On success out.root_hash is the
//! GroveDB root hash the proof commits to and out.results contains the
//! proven (path, key, element bytes) entries in proof order. Any layer whose
//! parent link hash does not match, any incomplete query answer and any
//! malformed encoding is a hard error.
bool VerifyQuery(Span<const uint8_t> proof, const PathQuery& query, const VerifyOptions& options,
                 GroveVerifyResult& out, std::string& error);

//! VerifyQuery + comparison against an expected root hash.
bool VerifyQueryWithRootHash(Span<const uint8_t> proof, const PathQuery& query,
                             const VerifyOptions& options, const Hash256& expected_root_hash,
                             GroveVerifyResult& out, std::string& error);

} // namespace platform::grove

#endif // BITCOIN_PLATFORM_PROOF_GROVEDB_H
