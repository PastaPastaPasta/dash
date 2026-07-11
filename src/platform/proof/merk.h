// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_PROOF_MERK_H
#define BITCOIN_PLATFORM_PROOF_MERK_H

#include <platform/serialize.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//! Merk (Merkle AVL tree) proof verification, a pure C++ port of the "verify"
//! feature slice of dashpay/grovedb tag v5.0.0:
//! - merk/src/tree/hash.rs               (blake3 node hashing)
//! - grovedb-query/src/proofs/mod.rs     (Op / Node variants)
//! - grovedb-query/src/proofs/encoding.rs (op stream decoding)
//! - grovedb-query/src/proofs/tree_feature_type.rs
//! - merk/src/proofs/tree.rs             (op replay stack machine)
//! - merk/src/proofs/query/verify.rs     (query matching + absence proofs)
namespace platform::merk {

using Hash256 = std::array<uint8_t, 32>;

//! merk/src/tree/hash.rs NULL_HASH: 32 zero bytes.
inline constexpr Hash256 NULL_HASH{};

//! blake3(varint(len(value)) || value); varint is LEB128
//! (merk/src/tree/hash.rs value_hash).
Hash256 ValueHash(Span<const uint8_t> value);
//! blake3(varint(len(key)) || key || value_hash(value))
//! (merk/src/tree/hash.rs kv_hash).
Hash256 KvHash(Span<const uint8_t> key, Span<const uint8_t> value);
//! blake3(varint(len(key)) || key || value_hash)
//! (merk/src/tree/hash.rs kv_digest_to_kv_hash).
Hash256 KvDigestToKvHash(Span<const uint8_t> key, const Hash256& value_hash);
//! blake3(kv || left || right) (merk/src/tree/hash.rs node_hash).
Hash256 NodeHash(const Hash256& kv, const Hash256& left, const Hash256& right);
//! blake3(a || b) (merk/src/tree/hash.rs combine_hash).
Hash256 CombineHash(const Hash256& a, const Hash256& b);
//! blake3(kv || left || right || sum_be8)
//! (merk/src/tree/hash.rs node_hash_with_sum; only used by ProvableSumTree
//! proofs, which this verifier rejects, but kept for completeness).
Hash256 NodeHashWithSum(const Hash256& kv, const Hash256& left, const Hash256& right, int64_t sum);

//! TreeFeatureType (grovedb-query/src/proofs/tree_feature_type.rs).
//! Only BasicMerkNode and SummedMerkNode are supported; the count / big-sum /
//! provable variants are rejected at decode time.
struct TreeFeature {
    bool summed{false};
    int64_t sum{0};
};

//! Node variants (grovedb-query/src/proofs/mod.rs enum Node). Only the
//! variants emitted for plain trees and (non-provable) sum trees are
//! supported; the count / MMR / dense / commitment / provable-aggregate op
//! families are rejected with explicit errors.
enum class NodeVariant : uint8_t {
    HASH,
    KV_HASH,
    KV,
    KV_VALUE_HASH,
    KV_DIGEST,
    KV_REF_VALUE_HASH,
    KV_VALUE_HASH_FEATURE_TYPE,
    KV_VALUE_HASH_FEATURE_TYPE_WITH_CHILD_HASH,
};

struct Node {
    NodeVariant variant{NodeVariant::HASH};
    Bytes key;
    Bytes value;
    Hash256 hash{};       //!< HASH / KV_HASH payload
    Hash256 value_hash{}; //!< KV_VALUE_HASH / KV_DIGEST / KV_REF_VALUE_HASH / feature type variants
    TreeFeature feature{};
    Hash256 child_hash{}; //!< KV_VALUE_HASH_FEATURE_TYPE_WITH_CHILD_HASH only

    bool IsKeyBearing() const;
};

//! Proof operators (grovedb-query/src/proofs/mod.rs enum Op).
enum class OpType : uint8_t {
    PUSH,
    PUSH_INVERTED,
    PARENT,
    CHILD,
    PARENT_INVERTED,
    CHILD_INVERTED,
};

struct Op {
    OpType type{OpType::PUSH};
    Node node;
};

//! Decodes one proof operator from the reader
//! (grovedb-query/src/proofs/encoding.rs impl Decode for Op). Key length is
//! a u8; value lengths are fixed-width big-endian u16 (small ops) or u32
//! (large ops). Returns false with reader error set on failure.
bool DecodeOp(BytesReader& reader, Op& op);

//! Query item shapes needed by the platform GUI
//! (grovedb-query/src/query_item/mod.rs enum QueryItem subset).
struct QueryItem {
    enum class Type : uint8_t {
        KEY,
        RANGE,           //!< start..end (end exclusive)
        RANGE_INCLUSIVE, //!< start..=end
        RANGE_FULL,
    };
    Type type{Type::KEY};
    Bytes key;   //!< KEY
    Bytes start; //!< RANGE / RANGE_INCLUSIVE
    Bytes end;   //!< RANGE / RANGE_INCLUSIVE

    static QueryItem Key(Bytes key);
    static QueryItem Range(Bytes start, Bytes end);
    static QueryItem RangeInclusive(Bytes start, Bytes end);
    static QueryItem RangeFull();

    //! (bound, is_exclusive); nullopt bound means unbounded.
    std::pair<std::optional<Span<const Bytes::value_type>>, bool> LowerBound() const;
    //! (bound, is_inclusive); nullopt bound means unbounded.
    std::pair<std::optional<Span<const Bytes::value_type>>, bool> UpperBound() const;
    bool LowerUnbounded() const { return type == Type::RANGE_FULL; }
    bool UpperUnbounded() const { return type == Type::RANGE_FULL; }
    bool Contains(Span<const uint8_t> key) const;
};

//! One proven result entry (merk/src/proofs/query/verify.rs
//! ProvedKeyOptionalValue).
struct ProvedKeyValue {
    Bytes key;
    Bytes value;
    Hash256 value_hash{};
    //! True only for KVValueHashFeatureTypeWithChildHash nodes where
    //! combine_hash(H(value), child_hash) == value_hash was checked.
    bool child_hash_verified{false};
};

struct VerifyResult {
    Hash256 root_hash{};
    std::vector<ProvedKeyValue> result_set;
    std::optional<uint16_t> limit_left;
};

//! Strict proof version constant (merk/src/proofs/query/verify.rs
//! PROOF_VERSION_LATEST). Version 0 permits item elements in KVValueHash
//! nodes (legacy V0 envelopes); version 1 rejects them.
inline constexpr uint16_t PROOF_VERSION_LATEST{1};

//! Executes and verifies a merk proof against a query, mirroring
//! merk/src/proofs/query/verify.rs execute_proof. `items` must be sorted
//! ascending and non-overlapping (grovedb's Query::insert_item invariant);
//! this is validated and violations are errors.
//!
//! On success `out.root_hash` is the reconstructed merk root hash and
//! `out.result_set` contains exactly the entries that answer the query.
//! Absent keys are proven absent via boundary nodes; a proof that omits data
//! required by the query fails verification (security critical: a node must
//! not be able to silently drop matching results).
bool ExecuteProof(Span<const uint8_t> proof, const std::vector<QueryItem>& items,
                  std::optional<uint16_t> limit, bool left_to_right, uint16_t proof_version,
                  VerifyResult& out, std::string& error);

//! ExecuteProof + comparison of the reconstructed root hash against
//! expected_hash (merk/src/proofs/query/verify.rs verify_proof).
bool VerifyProof(Span<const uint8_t> proof, const std::vector<QueryItem>& items,
                 std::optional<uint16_t> limit, bool left_to_right, const Hash256& expected_hash,
                 VerifyResult& out, std::string& error);

} // namespace platform::merk

#endif // BITCOIN_PLATFORM_PROOF_MERK_H
