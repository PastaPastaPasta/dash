// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/proof/merk.h>

#include <crypto/blake3/blake3.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstring>

namespace platform::merk {

namespace {

//! merk/src/proofs/tree.rs MAX_PROOF_OPS.
constexpr size_t MAX_PROOF_OPS{50'000};
//! merk/src/proofs/tree.rs MAX_PROOF_STACK_DEPTH.
constexpr size_t MAX_PROOF_STACK_DEPTH{10'000};
//! merk/src/proofs/tree.rs MAX_PROOF_TREE_HEIGHT.
constexpr size_t MAX_PROOF_TREE_HEIGHT{92};
//! grovedb-query/src/proofs/encoding.rs MAX_VALUE_LEN (64 MiB).
constexpr uint32_t MAX_VALUE_LEN{64 * 1024 * 1024};
//! GroveDB enforces a 255 byte maximum key length, so u8 key lengths in the
//! proof encoding are exact (grovedb-query/src/proofs/encoding.rs).
constexpr size_t MAX_KEY_LEN{255};

class Blake3
{
public:
    Blake3() { blake3_hasher_init(&m_hasher); }
    Blake3& Update(Span<const uint8_t> data)
    {
        blake3_hasher_update(&m_hasher, data.data(), data.size());
        return *this;
    }
    Hash256 Finalize()
    {
        Hash256 out;
        blake3_hasher_finalize(&m_hasher, out.data(), out.size());
        return out;
    }

private:
    blake3_hasher m_hasher;
};

Bytes LEB128(uint64_t value)
{
    Bytes out;
    WriteLEB128(out, value);
    return out;
}

} // namespace

Hash256 ValueHash(Span<const uint8_t> value)
{
    // merk/src/tree/hash.rs value_hash
    return Blake3{}.Update(LEB128(value.size())).Update(value).Finalize();
}

Hash256 KvHash(Span<const uint8_t> key, Span<const uint8_t> value)
{
    // merk/src/tree/hash.rs kv_hash
    const Hash256 vh{ValueHash(value)};
    return Blake3{}.Update(LEB128(key.size())).Update(key).Update(vh).Finalize();
}

Hash256 KvDigestToKvHash(Span<const uint8_t> key, const Hash256& value_hash)
{
    // merk/src/tree/hash.rs kv_digest_to_kv_hash
    return Blake3{}.Update(LEB128(key.size())).Update(key).Update(value_hash).Finalize();
}

Hash256 NodeHash(const Hash256& kv, const Hash256& left, const Hash256& right)
{
    // merk/src/tree/hash.rs node_hash
    return Blake3{}.Update(kv).Update(left).Update(right).Finalize();
}

Hash256 CombineHash(const Hash256& a, const Hash256& b)
{
    // merk/src/tree/hash.rs combine_hash
    return Blake3{}.Update(a).Update(b).Finalize();
}

Hash256 NodeHashWithSum(const Hash256& kv, const Hash256& left, const Hash256& right, int64_t sum)
{
    // merk/src/tree/hash.rs node_hash_with_sum: i64 sum appended big-endian.
    std::array<uint8_t, 8> sum_be;
    const auto usum = static_cast<uint64_t>(sum);
    for (int i = 0; i < 8; ++i) {
        sum_be[i] = static_cast<uint8_t>(usum >> (8 * (7 - i)));
    }
    return Blake3{}.Update(kv).Update(left).Update(right).Update(sum_be).Finalize();
}

bool Node::IsKeyBearing() const
{
    switch (variant) {
    case NodeVariant::KV:
    case NodeVariant::KV_VALUE_HASH:
    case NodeVariant::KV_DIGEST:
    case NodeVariant::KV_REF_VALUE_HASH:
    case NodeVariant::KV_VALUE_HASH_FEATURE_TYPE:
    case NodeVariant::KV_VALUE_HASH_FEATURE_TYPE_WITH_CHILD_HASH:
        return true;
    case NodeVariant::HASH:
    case NodeVariant::KV_HASH:
        return false;
    }
    return false;
}

namespace {

//! Decodes a TreeFeatureType (grovedb-query/src/proofs/tree_feature_type.rs
//! impl Decode). Tag byte then, for SummedMerkNode, an LEB128 zigzag i64
//! (integer-encoding crate write_varint). Only Basic (0) and Summed (1) are
//! supported.
bool DecodeTreeFeature(BytesReader& reader, TreeFeature& out)
{
    uint8_t tag;
    if (!reader.ReadU8(tag)) return false;
    switch (tag) {
    case 0:
        out = TreeFeature{};
        return true;
    case 1:
        out.summed = true;
        return reader.ReadLEB128Signed(out.sum);
    case 2:
        return reader.SetError("unsupported tree feature type BigSummedMerkNode");
    case 3:
        return reader.SetError("unsupported tree feature type CountedMerkNode");
    case 4:
        return reader.SetError("unsupported tree feature type CountedSummedMerkNode");
    case 5:
        return reader.SetError("unsupported tree feature type ProvableCountedMerkNode");
    case 6:
        return reader.SetError("unsupported tree feature type ProvableCountedSummedMerkNode");
    case 7:
        return reader.SetError("unsupported tree feature type ProvableSummedMerkNode");
    case 8:
        return reader.SetError("unsupported tree feature type ProvableCountedAndProvableSummedMerkNode");
    default:
        return reader.SetError(strprintf("unknown tree feature type tag %u", tag));
    }
}

bool ReadKey(BytesReader& reader, Bytes& key)
{
    uint8_t key_len;
    if (!reader.ReadU8(key_len)) return false;
    static_assert(MAX_KEY_LEN == 255, "u8 key length covers the whole key range");
    return reader.ReadBytes(key_len, key);
}

//! large=false: u16 big-endian value length; large=true: u32 big-endian
//! value length that must not fit in the small encoding (canonical form) and
//! must respect MAX_VALUE_LEN.
bool ReadValue(BytesReader& reader, bool large, Bytes& value)
{
    if (!large) {
        uint16_t len;
        if (!reader.ReadU16BE(len)) return false;
        return reader.ReadBytes(len, value);
    }
    uint32_t len;
    if (!reader.ReadU32BE(len)) return false;
    if (len < 65536) return reader.SetError("non-canonical large value length in proof op");
    if (len > MAX_VALUE_LEN) return reader.SetError(strprintf("proof value length %u exceeds maximum %u", len, MAX_VALUE_LEN));
    return reader.ReadBytes(len, value);
}

bool ReadHash(BytesReader& reader, Hash256& out)
{
    return reader.ReadInto(Span<uint8_t>{out.data(), out.size()});
}

} // namespace

bool DecodeOp(BytesReader& reader, Op& op)
{
    // grovedb-query/src/proofs/encoding.rs impl Decode for Op.
    uint8_t opcode;
    if (!reader.ReadU8(opcode)) return false;

    op.node = Node{};
    const auto push = [&](OpType type, NodeVariant variant) {
        op.type = type;
        op.node.variant = variant;
    };

    switch (opcode) {
    case 0x01:
    case 0x08:
        push(opcode == 0x01 ? OpType::PUSH : OpType::PUSH_INVERTED, NodeVariant::HASH);
        return ReadHash(reader, op.node.hash);
    case 0x02:
    case 0x09:
        push(opcode == 0x02 ? OpType::PUSH : OpType::PUSH_INVERTED, NodeVariant::KV_HASH);
        return ReadHash(reader, op.node.hash);
    case 0x03:
    case 0x0a:
    case 0x20:
    case 0x28: {
        const bool large{opcode == 0x20 || opcode == 0x28};
        push((opcode == 0x03 || opcode == 0x20) ? OpType::PUSH : OpType::PUSH_INVERTED, NodeVariant::KV);
        return ReadKey(reader, op.node.key) && ReadValue(reader, large, op.node.value);
    }
    case 0x04:
    case 0x0b:
    case 0x21:
    case 0x29: {
        const bool large{opcode == 0x21 || opcode == 0x29};
        push((opcode == 0x04 || opcode == 0x21) ? OpType::PUSH : OpType::PUSH_INVERTED, NodeVariant::KV_VALUE_HASH);
        return ReadKey(reader, op.node.key) && ReadValue(reader, large, op.node.value) &&
               ReadHash(reader, op.node.value_hash);
    }
    case 0x05:
    case 0x0c:
        push(opcode == 0x05 ? OpType::PUSH : OpType::PUSH_INVERTED, NodeVariant::KV_DIGEST);
        return ReadKey(reader, op.node.key) && ReadHash(reader, op.node.value_hash);
    case 0x06:
    case 0x0d:
    case 0x22:
    case 0x2a: {
        const bool large{opcode == 0x22 || opcode == 0x2a};
        push((opcode == 0x06 || opcode == 0x22) ? OpType::PUSH : OpType::PUSH_INVERTED, NodeVariant::KV_REF_VALUE_HASH);
        return ReadKey(reader, op.node.key) && ReadValue(reader, large, op.node.value) &&
               ReadHash(reader, op.node.value_hash);
    }
    case 0x07:
    case 0x0e:
    case 0x23:
    case 0x2b: {
        const bool large{opcode == 0x23 || opcode == 0x2b};
        push((opcode == 0x07 || opcode == 0x23) ? OpType::PUSH : OpType::PUSH_INVERTED, NodeVariant::KV_VALUE_HASH_FEATURE_TYPE);
        return ReadKey(reader, op.node.key) && ReadValue(reader, large, op.node.value) &&
               ReadHash(reader, op.node.value_hash) && DecodeTreeFeature(reader, op.node.feature);
    }
    case 0x1c:
    case 0x1d:
    case 0x2e:
    case 0x2f: {
        const bool large{opcode == 0x2e || opcode == 0x2f};
        push((opcode == 0x1c || opcode == 0x2e) ? OpType::PUSH : OpType::PUSH_INVERTED,
             NodeVariant::KV_VALUE_HASH_FEATURE_TYPE_WITH_CHILD_HASH);
        return ReadKey(reader, op.node.key) && ReadValue(reader, large, op.node.value) &&
               ReadHash(reader, op.node.value_hash) && DecodeTreeFeature(reader, op.node.feature) &&
               ReadHash(reader, op.node.child_hash);
    }
    case 0x10:
        op.type = OpType::PARENT;
        return true;
    case 0x11:
        op.type = OpType::CHILD;
        return true;
    case 0x12:
        op.type = OpType::PARENT_INVERTED;
        return true;
    case 0x13:
        op.type = OpType::CHILD_INVERTED;
        return true;
    default:
        break;
    }

    // Recognized but unsupported op families; reject with explicit errors so
    // callers can tell "malformed" from "unsupported tree type".
    if ((opcode >= 0x14 && opcode <= 0x1b) || opcode == 0x1e || opcode == 0x1f ||
        opcode == 0x24 || opcode == 0x25 || opcode == 0x2c || opcode == 0x2d) {
        return reader.SetError(strprintf("unsupported count-tree proof op 0x%02x", opcode));
    }
    if (opcode >= 0x30 && opcode <= 0x3d) {
        return reader.SetError(strprintf("unsupported provable-sum-tree proof op 0x%02x", opcode));
    }
    if (opcode >= 0x40 && opcode <= 0x4d) {
        return reader.SetError(strprintf("unsupported provable-count-and-sum-tree proof op 0x%02x", opcode));
    }
    return reader.SetError(strprintf("unknown proof op 0x%02x", opcode));
}

QueryItem QueryItem::Key(Bytes key)
{
    QueryItem item;
    item.type = Type::KEY;
    item.key = std::move(key);
    return item;
}

QueryItem QueryItem::Range(Bytes start, Bytes end)
{
    QueryItem item;
    item.type = Type::RANGE;
    item.start = std::move(start);
    item.end = std::move(end);
    return item;
}

QueryItem QueryItem::RangeInclusive(Bytes start, Bytes end)
{
    QueryItem item;
    item.type = Type::RANGE_INCLUSIVE;
    item.start = std::move(start);
    item.end = std::move(end);
    return item;
}

QueryItem QueryItem::RangeFull()
{
    QueryItem item;
    item.type = Type::RANGE_FULL;
    return item;
}

std::pair<std::optional<Span<const uint8_t>>, bool> QueryItem::LowerBound() const
{
    // grovedb-query/src/query_item/mod.rs lower_bound
    switch (type) {
    case Type::KEY:
        return {Span<const uint8_t>{key}, false};
    case Type::RANGE:
    case Type::RANGE_INCLUSIVE:
        return {Span<const uint8_t>{start}, false};
    case Type::RANGE_FULL:
        return {std::nullopt, false};
    }
    return {std::nullopt, false};
}

std::pair<std::optional<Span<const uint8_t>>, bool> QueryItem::UpperBound() const
{
    // grovedb-query/src/query_item/mod.rs upper_bound
    switch (type) {
    case Type::KEY:
        return {Span<const uint8_t>{key}, true};
    case Type::RANGE:
        return {Span<const uint8_t>{end}, false};
    case Type::RANGE_INCLUSIVE:
        return {Span<const uint8_t>{end}, true};
    case Type::RANGE_FULL:
        return {std::nullopt, true};
    }
    return {std::nullopt, true};
}

namespace {

int CompareBytes(Span<const uint8_t> a, Span<const uint8_t> b)
{
    const size_t common{std::min(a.size(), b.size())};
    const int r{common == 0 ? 0 : std::memcmp(a.data(), b.data(), common)};
    if (r != 0) return r;
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

} // namespace

bool QueryItem::Contains(Span<const uint8_t> candidate) const
{
    // grovedb-query/src/query_item/mod.rs contains
    const auto [lower, lower_non_inclusive] = LowerBound();
    const auto [upper, upper_inclusive] = UpperBound();
    const bool lower_ok{LowerUnbounded() ||
                        (lower && CompareBytes(candidate, *lower) > 0) ||
                        (lower && CompareBytes(candidate, *lower) == 0 && !lower_non_inclusive)};
    const bool upper_ok{UpperUnbounded() ||
                        (upper && CompareBytes(candidate, *upper) < 0) ||
                        (upper && CompareBytes(candidate, *upper) == 0 && upper_inclusive)};
    return lower_ok && upper_ok;
}

namespace {

//! Peeks at the element type discriminant of serialized element bytes and
//! reports whether the type has a "simple" value hash (H(value) with no
//! child-hash combination), mirroring
//! grovedb-element/src/element_type.rs from_serialized_value +
//! has_simple_value_hash. Simple types: Item (0), SumItem (3),
//! ItemWithSumItem (9), including under a NonCounted (15) wrapper.
bool ElementHasSimpleValueHash(Span<const uint8_t> value, bool& simple, std::string& error)
{
    if (value.empty()) {
        error = "cannot get element type from empty value";
        return false;
    }
    uint8_t base{value[0]};
    if (base == 15 || base == 16 || base == 17) {
        // Wrapper discriminants (NonCounted / NotSummed / NotCountedOrSummed)
        if (value.size() < 2) {
            error = "element wrapper has no inner discriminant byte";
            return false;
        }
        const uint8_t inner{value[1]};
        if (base == 15) {
            if (!(inner <= 14 || inner == 18 || inner == 19 || inner == 20)) {
                error = strprintf("invalid NonCounted inner element discriminant %u", inner);
                return false;
            }
        } else {
            if (!(inner == 4 || inner == 5 || inner == 7 || inner == 10 || inner == 19 || inner == 20)) {
                error = strprintf("invalid NotSummed/NotCountedOrSummed inner element discriminant %u", inner);
                return false;
            }
        }
        base = inner;
    } else if (base > 20) {
        error = strprintf("unknown element discriminant %u", base);
        return false;
    }
    simple = base == 0 /* Item */ || base == 3 /* SumItem */ || base == 9 /* ItemWithSumItem */;
    return true;
}

//! A reconstructed proof tree node. Proofs are executed with the equivalent
//! of merk/src/proofs/tree.rs execute(ops, /*collapse=*/true, visit): child
//! subtrees are collapsed to their hash (and, matching the Rust
//! Tree::into_hash, their height resets to 1).
struct StackTree {
    Node node;
    bool has_left{false};
    bool has_right{false};
    Hash256 left_hash{};
    Hash256 right_hash{};
    size_t height{1};
    size_t left_child_height{0};
    size_t right_child_height{0};

    const Hash256& ChildHash(bool left) const
    {
        static constexpr Hash256 null_hash{};
        if (left) return has_left ? left_hash : null_hash;
        return has_right ? right_hash : null_hash;
    }

    //! merk/src/proofs/tree.rs Tree::hash for the supported node variants.
    Hash256 TreeHash() const
    {
        switch (node.variant) {
        case NodeVariant::HASH:
            return node.hash;
        case NodeVariant::KV_HASH:
            return NodeHash(node.hash, ChildHash(true), ChildHash(false));
        case NodeVariant::KV:
            return NodeHash(KvHash(node.key, node.value), ChildHash(true), ChildHash(false));
        case NodeVariant::KV_VALUE_HASH:
        case NodeVariant::KV_DIGEST:
        case NodeVariant::KV_VALUE_HASH_FEATURE_TYPE:
        case NodeVariant::KV_VALUE_HASH_FEATURE_TYPE_WITH_CHILD_HASH:
            // Basic and Summed feature types hash like plain nodes; the sum
            // is not part of the node hash for non-provable sum trees. The
            // child_hash of the WithChildHash variant is GroveDB-level
            // metadata and does not participate in the merk hash.
            return NodeHash(KvDigestToKvHash(node.key, node.value_hash), ChildHash(true), ChildHash(false));
        case NodeVariant::KV_REF_VALUE_HASH: {
            // value_hash field holds H(serialized reference element); the
            // node's kv hash commits to
            // combine_hash(value_hash, H(referenced value)).
            const Hash256 combined{CombineHash(node.value_hash, ValueHash(node.value))};
            return NodeHash(KvDigestToKvHash(node.key, combined), ChildHash(true), ChildHash(false));
        }
        }
        return NULL_HASH;
    }
};

//! Validates that query items are sorted ascending and non-overlapping (the
//! invariant grovedb's Query::insert_item maintains).
bool ValidateQueryItems(const std::vector<QueryItem>& items, std::string& error)
{
    for (size_t i = 1; i < items.size(); ++i) {
        const auto [prev_upper, prev_upper_inclusive] = items[i - 1].UpperBound();
        const auto [next_lower, next_lower_non_inclusive] = items[i].LowerBound();
        if (!prev_upper || !next_lower) {
            error = "unbounded query items cannot be combined with other items";
            return false;
        }
        const int cmp{CompareBytes(*prev_upper, *next_lower)};
        if (cmp > 0 || (cmp == 0 && prev_upper_inclusive && !next_lower_non_inclusive)) {
            error = "query items must be sorted ascending and non-overlapping";
            return false;
        }
    }
    return true;
}

} // namespace

bool ExecuteProof(Span<const uint8_t> proof, const std::vector<QueryItem>& items,
                  std::optional<uint16_t> limit, bool left_to_right, uint16_t proof_version,
                  VerifyResult& out, std::string& error)
{
    // Mirrors merk/src/proofs/query/verify.rs execute_proof combined with
    // merk/src/proofs/tree.rs execute (collapse = true).
    out = VerifyResult{};
    if (!ValidateQueryItems(items, error)) return false;

    BytesReader reader{proof};
    std::vector<StackTree> stack;
    stack.reserve(32);
    std::optional<Bytes> last_key;
    size_t op_count{0};

    // Query walk state (verify.rs execute_proof)
    size_t query_index{0}; // index into items in traversal direction
    const auto query_peek = [&]() -> const QueryItem* {
        if (query_index >= items.size()) return nullptr;
        return left_to_right ? &items[query_index] : &items[items.size() - 1 - query_index];
    };
    bool in_range{false};
    std::optional<uint16_t> current_limit{limit};
    // Last pushed node: nullopt until the first push; then whether it was a
    // key-bearing variant (KV / KVDigest / KVValueHash / KVRefValueHash /
    // feature-type variants) as opposed to Hash / KVHash.
    std::optional<bool> last_push_key_bearing;

    const auto execute_node = [&](const Bytes& key, const Bytes* value, const Hash256& value_hash,
                                  bool child_hash_verified) -> bool {
        while (const QueryItem* query_item = query_peek()) {
            const auto [lower_bound, start_non_inclusive] = query_item->LowerBound();
            const auto [upper_bound, end_inclusive] = query_item->UpperBound();

            // Terminate if we encounter a node before the current query item.
            const bool terminate = left_to_right
                                       ? (!query_item->LowerUnbounded() &&
                                          (CompareBytes(*lower_bound, key) > 0 ||
                                           (start_non_inclusive && CompareBytes(*lower_bound, key) == 0)))
                                       : (!query_item->UpperUnbounded() &&
                                          (CompareBytes(*upper_bound, key) < 0 ||
                                           (!end_inclusive && CompareBytes(*upper_bound, key) == 0)));
            if (terminate) break;

            if (!in_range) {
                // This is the first data we encounter for this query item:
                // the bound facing the traversal direction must be proven,
                // either by an exact match or by a preceding key-bearing
                // node (or this being the first node in the tree).
                const auto bound = left_to_right ? lower_bound : upper_bound;
                const bool exact_match{bound && CompareBytes(key, *bound) == 0};
                if (!exact_match && last_push_key_bearing.has_value() && !*last_push_key_bearing) {
                    error = left_to_right ? "cannot verify lower bound of queried range"
                                          : "cannot verify upper bound of queried range";
                    return false;
                }
            }

            if (left_to_right) {
                if (upper_bound && CompareBytes(key, *upper_bound) >= 0) {
                    // At or past the upper bound (or exact single-key match):
                    // advance to the next query item.
                    ++query_index;
                    in_range = false;
                } else {
                    in_range = true;
                }
            } else if (lower_bound && CompareBytes(key, *lower_bound) <= 0) {
                ++query_index;
                in_range = false;
            } else {
                in_range = true;
            }

            if (query_item->Contains(key)) {
                if (value == nullptr) {
                    error = "proof is missing data for query";
                    return false;
                }
                if (current_limit.has_value()) {
                    if (*current_limit == 0) {
                        error = "proof returns more data than the query limit";
                        return false;
                    }
                    --*current_limit;
                    if (*current_limit == 0) in_range = false;
                }
                out.result_set.push_back(ProvedKeyValue{key, *value, value_hash, child_hash_verified});
                break; // continue to next push
            }
            // otherwise: continue with the next query item for the same key
        }
        return true;
    };

    const auto visit_node = [&](const Node& node) -> bool {
        switch (node.variant) {
        case NodeVariant::KV:
            return execute_node(node.key, &node.value, ValueHash(node.value), false);
        case NodeVariant::KV_VALUE_HASH:
        case NodeVariant::KV_VALUE_HASH_FEATURE_TYPE:
        case NodeVariant::KV_VALUE_HASH_FEATURE_TYPE_WITH_CHILD_HASH: {
            // Strict (V1+) mode: these node types exist for elements whose
            // value_hash is a combine_hash (trees and references). Reject
            // item elements to prevent KV -> KVValueHash forgery.
            if (proof_version >= 1) {
                bool simple{false};
                std::string element_error;
                if (!ElementHasSimpleValueHash(node.value, simple, element_error)) {
                    error = strprintf("cannot determine element type in proof node: %s", element_error);
                    return false;
                }
                if (simple) {
                    error = "value-hash proof node must not contain an item element";
                    return false;
                }
            }
            if (node.variant == NodeVariant::KV_VALUE_HASH_FEATURE_TYPE_WITH_CHILD_HASH) {
                // Verify value integrity: combine_hash(H(value), child_hash)
                // must equal the node's value_hash
                // (merk/src/proofs/query/verify.rs).
                const Hash256 computed{CombineHash(ValueHash(node.value), node.child_hash)};
                if (computed != node.value_hash) {
                    error = "value/child hash mismatch in proof node";
                    return false;
                }
                return execute_node(node.key, &node.value, node.value_hash, true);
            }
            return execute_node(node.key, &node.value, node.value_hash, false);
        }
        case NodeVariant::KV_DIGEST:
            return execute_node(node.key, nullptr, node.value_hash, false);
        case NodeVariant::KV_REF_VALUE_HASH:
            return execute_node(node.key, &node.value, node.value_hash, false);
        case NodeVariant::HASH:
        case NodeVariant::KV_HASH:
            if (in_range) {
                error = "proof is missing data for query range (abridged node inside range)";
                return false;
            }
            return true;
        }
        error = "unhandled proof node variant";
        return false;
    };

    // merk/src/proofs/tree.rs execute: op replay stack machine.
    const auto attach = [&](StackTree& parent, bool left, const StackTree& child) -> bool {
        if ((left && parent.has_left) || (!left && parent.has_right)) {
            error = "proof op tried to attach to an already occupied child slot";
            return false;
        }
        // Collapse the child to its hash; matching Rust Tree::into_hash the
        // collapsed child's height is 1.
        const Hash256 child_hash{child.TreeHash()};
        constexpr size_t collapsed_child_height{1};
        const size_t new_height{std::max(parent.height, collapsed_child_height + 1)};
        if (new_height > MAX_PROOF_TREE_HEIGHT) {
            error = "proof tree exceeds maximum height";
            return false;
        }
        parent.height = new_height;
        if (left) {
            parent.has_left = true;
            parent.left_hash = child_hash;
            parent.left_child_height = collapsed_child_height;
        } else {
            parent.has_right = true;
            parent.right_hash = child_hash;
            parent.right_child_height = collapsed_child_height;
        }
        return true;
    };

    const auto pop = [&](StackTree& out_tree) -> bool {
        if (stack.empty()) {
            error = "proof op stack underflow";
            return false;
        }
        out_tree = std::move(stack.back());
        stack.pop_back();
        return true;
    };

    while (!reader.IsEof()) {
        if (++op_count > MAX_PROOF_OPS) {
            error = "proof exceeds maximum operation count";
            return false;
        }
        Op op;
        if (!DecodeOp(reader, op)) {
            error = reader.GetError();
            return false;
        }
        switch (op.type) {
        case OpType::PARENT:
        case OpType::CHILD_INVERTED: {
            StackTree parent, child;
            if (!pop(parent) || !pop(child)) return false;
            if (op.type == OpType::CHILD_INVERTED) std::swap(parent, child);
            if (!attach(parent, /*left=*/true, child)) return false;
            stack.push_back(std::move(parent));
            break;
        }
        case OpType::CHILD:
        case OpType::PARENT_INVERTED: {
            // CHILD pops the child from the top of the stack, then the
            // parent; PARENT_INVERTED pops the parent first. Both attach the
            // child on the right (grovedb-query/src/proofs/mod.rs).
            StackTree parent, child;
            if (op.type == OpType::CHILD) {
                if (!pop(child) || !pop(parent)) return false;
            } else {
                if (!pop(parent) || !pop(child)) return false;
            }
            if (!attach(parent, /*left=*/false, child)) return false;
            stack.push_back(std::move(parent));
            break;
        }
        case OpType::PUSH:
        case OpType::PUSH_INVERTED: {
            if (op.node.IsKeyBearing() && last_key.has_value()) {
                const int cmp{CompareBytes(op.node.key, *last_key)};
                if (op.type == OpType::PUSH && cmp <= 0) {
                    error = "incorrect key ordering in proof";
                    return false;
                }
                if (op.type == OpType::PUSH_INVERTED && cmp >= 0) {
                    error = "incorrect key ordering in inverted proof";
                    return false;
                }
            }
            if (op.node.IsKeyBearing()) last_key = op.node.key;
            if (!visit_node(op.node)) return false;
            last_push_key_bearing = op.node.IsKeyBearing();
            StackTree tree;
            tree.node = std::move(op.node);
            stack.push_back(std::move(tree));
            if (stack.size() > MAX_PROOF_STACK_DEPTH) {
                error = "proof exceeds maximum stack depth";
                return false;
            }
            break;
        }
        }
    }

    if (stack.size() != 1) {
        error = "expected proof to result in exactly one stack item";
        return false;
    }
    const StackTree root{std::move(stack.back())};
    stack.pop_back();

    // Root-level AVL balance check (merk/src/proofs/tree.rs execute).
    const size_t max_child{std::max(root.left_child_height, root.right_child_height)};
    const size_t min_child{std::min(root.left_child_height, root.right_child_height)};
    if (max_child - min_child > 1) {
        error = "expected proof to result in a valid avl tree";
        return false;
    }

    // Remaining query items must be proven absent against the tree edge.
    if (query_peek() != nullptr && !(current_limit.has_value() && *current_limit == 0)) {
        if (!last_push_key_bearing.has_value() || !*last_push_key_bearing) {
            error = "proof is missing data for query";
            return false;
        }
    }

    out.root_hash = root.TreeHash();
    out.limit_left = current_limit;
    return true;
}

bool VerifyProof(Span<const uint8_t> proof, const std::vector<QueryItem>& items,
                 std::optional<uint16_t> limit, bool left_to_right, const Hash256& expected_hash,
                 VerifyResult& out, std::string& error)
{
    if (!ExecuteProof(proof, items, limit, left_to_right, PROOF_VERSION_LATEST, out, error)) return false;
    if (out.root_hash != expected_hash) {
        error = strprintf("proof did not match expected hash: expected %s, got %s",
                          HexStr(expected_hash), HexStr(out.root_hash));
        return false;
    }
    return true;
}

} // namespace platform::merk
