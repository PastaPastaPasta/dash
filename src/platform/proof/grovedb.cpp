// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/proof/grovedb.h>

#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <limits>

namespace platform::grove {

namespace {

//! grovedb/src/operations/proof/mod.rs MAX_PROOF_DEPTH: limit on layer
//! nesting depth and on the number of children per layer.
constexpr size_t MAX_PROOF_DEPTH{128};
//! grovedb decodes envelopes with a 256 MiB bincode limit
//! (grovedb/src/operations/proof/mod.rs decode_grovedb_proof_canonical).
constexpr size_t MAX_PROOF_BYTES{256 * 1024 * 1024};
//! Sanity cap on decoded path segment counts (no grovedb equivalent; real
//! reference paths are a handful of segments deep).
constexpr size_t MAX_PATH_SEGMENTS{1024};

} // namespace

bool ReferencePath::Resolve(const std::vector<Bytes>& current_path, const Bytes& key,
                            std::vector<Bytes>& out, std::string& error) const
{
    // grovedb-element/src/reference_path/mod.rs path_from_reference_path_type
    out.clear();
    switch (type) {
    case Type::ABSOLUTE_PATH:
        out = path;
        return true;
    case Type::UPSTREAM_ROOT_HEIGHT: {
        if (height > current_path.size()) {
            error = "reference stored path cannot satisfy reference constraints";
            return false;
        }
        out.assign(current_path.begin(), current_path.begin() + height);
        out.insert(out.end(), path.begin(), path.end());
        return true;
    }
    case Type::UPSTREAM_ROOT_HEIGHT_WITH_PARENT_PATH_ADDITION: {
        if (height > current_path.size() || current_path.empty()) {
            error = "reference stored path cannot satisfy reference constraints";
            return false;
        }
        out.assign(current_path.begin(), current_path.begin() + height);
        out.insert(out.end(), path.begin(), path.end());
        out.push_back(current_path.back());
        return true;
    }
    case Type::UPSTREAM_FROM_ELEMENT_HEIGHT: {
        if (height > current_path.size()) {
            error = "reference stored path cannot satisfy reference constraints";
            return false;
        }
        out.assign(current_path.begin(), current_path.end() - height);
        out.insert(out.end(), path.begin(), path.end());
        return true;
    }
    case Type::COUSIN: {
        if (current_path.empty() || path.size() != 1) {
            error = "reference stored path cannot satisfy reference constraints";
            return false;
        }
        out.assign(current_path.begin(), current_path.end() - 1);
        out.push_back(path[0]);
        out.push_back(key);
        return true;
    }
    case Type::REMOVED_COUSIN: {
        if (current_path.empty()) {
            error = "reference stored path cannot satisfy reference constraints";
            return false;
        }
        out.assign(current_path.begin(), current_path.end() - 1);
        out.insert(out.end(), path.begin(), path.end());
        out.push_back(key);
        return true;
    }
    case Type::SIBLING: {
        if (path.size() != 1) {
            error = "reference stored path cannot satisfy reference constraints";
            return false;
        }
        out = current_path;
        out.push_back(path[0]);
        return true;
    }
    }
    error = "unknown reference path type";
    return false;
}

namespace {

bool ReadPathSegments(BytesReader& reader, std::vector<Bytes>& out)
{
    uint64_t count;
    if (!reader.ReadBincodeVarint(count)) return false;
    if (count > MAX_PATH_SEGMENTS) return reader.SetError(strprintf("path has too many segments (%u)", count));
    out.clear();
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        Bytes segment;
        if (!reader.ReadBincodeByteVec(segment, MAX_PROOF_BYTES)) return false;
        out.push_back(std::move(segment));
    }
    return true;
}

//! grovedb-element/src/reference_path/mod.rs ReferencePathType, bincode
//! standard()+big_endian(): varint discriminant in declaration order.
bool DecodeReferencePath(BytesReader& reader, ReferencePath& out)
{
    uint64_t discriminant;
    if (!reader.ReadBincodeVarint(discriminant)) return false;
    out = ReferencePath{};
    switch (discriminant) {
    case 0:
        out.type = ReferencePath::Type::ABSOLUTE_PATH;
        return ReadPathSegments(reader, out.path);
    case 1:
    case 2:
    case 3:
        out.type = discriminant == 1 ? ReferencePath::Type::UPSTREAM_ROOT_HEIGHT
                   : discriminant == 2
                       ? ReferencePath::Type::UPSTREAM_ROOT_HEIGHT_WITH_PARENT_PATH_ADDITION
                       : ReferencePath::Type::UPSTREAM_FROM_ELEMENT_HEIGHT;
        return reader.ReadU8(out.height) && ReadPathSegments(reader, out.path);
    case 4:
    case 6: {
        out.type = discriminant == 4 ? ReferencePath::Type::COUSIN : ReferencePath::Type::SIBLING;
        Bytes key;
        if (!reader.ReadBincodeByteVec(key, MAX_PROOF_BYTES)) return false;
        out.path.push_back(std::move(key));
        return true;
    }
    case 5:
        out.type = ReferencePath::Type::REMOVED_COUSIN;
        return ReadPathSegments(reader, out.path);
    default:
        return reader.SetError(strprintf("unknown reference path type discriminant %u", discriminant));
    }
}

bool ReadOptionalByteVec(BytesReader& reader, std::optional<Bytes>& out)
{
    bool has_value;
    if (!reader.ReadBincodeOptionTag(has_value)) return false;
    if (!has_value) {
        out.reset();
        return true;
    }
    Bytes value;
    if (!reader.ReadBincodeByteVec(value, MAX_PROOF_BYTES)) return false;
    out = std::move(value);
    return true;
}

const char* UnsupportedElementName(uint64_t discriminant)
{
    switch (discriminant) {
    case 5: return "BigSumTree";
    case 6: return "CountTree";
    case 7: return "CountSumTree";
    case 8: return "ProvableCountTree";
    case 9: return "ItemWithSumItem";
    case 10: return "ProvableCountSumTree";
    case 11: return "CommitmentTree";
    case 12: return "MmrTree";
    case 13: return "BulkAppendTree";
    case 14: return "DenseAppendOnlyFixedSizeTree";
    case 15: return "NonCounted";
    case 16: return "NotSummed";
    case 17: return "NotCountedOrSummed";
    case 18: return "ReferenceWithSumItem";
    case 19: return "ProvableSumTree";
    case 20: return "ProvableCountProvableSumTree";
    default: return nullptr;
    }
}

} // namespace

bool DecodeElement(Span<const uint8_t> bytes, Element& out, std::string& error)
{
    // grovedb-element/src/element/serialize.rs Element::deserialize with the
    // bincode standard()+big_endian()+no_limit configuration. Enum
    // discriminants and collection lengths are bincode varints; Option is a
    // 0/1 tag byte; integer payloads are zigzag varints.
    BytesReader reader{bytes};
    out = Element{};
    uint64_t discriminant;
    bool ok{reader.ReadBincodeVarint(discriminant)};
    if (ok) {
        switch (discriminant) {
        case 0:
            out.type = Element::Type::ITEM;
            ok = reader.ReadBincodeByteVec(out.item_value, MAX_PROOF_BYTES) &&
                 ReadOptionalByteVec(reader, out.flags);
            break;
        case 1: {
            out.type = Element::Type::REFERENCE;
            ok = DecodeReferencePath(reader, out.reference);
            if (ok) {
                bool has_hops;
                ok = reader.ReadBincodeOptionTag(has_hops);
                if (ok && has_hops) {
                    uint8_t hops;
                    ok = reader.ReadU8(hops);
                    out.max_hops = hops;
                }
            }
            ok = ok && ReadOptionalByteVec(reader, out.flags);
            break;
        }
        case 2: {
            out.type = Element::Type::TREE;
            ok = ReadOptionalByteVec(reader, out.root_key) && ReadOptionalByteVec(reader, out.flags);
            break;
        }
        case 3:
            out.type = Element::Type::SUM_ITEM;
            ok = reader.ReadBincodeVarintSigned(out.sum_value) && ReadOptionalByteVec(reader, out.flags);
            break;
        case 4:
            out.type = Element::Type::SUM_TREE;
            ok = ReadOptionalByteVec(reader, out.root_key) &&
                 reader.ReadBincodeVarintSigned(out.sum_value) && ReadOptionalByteVec(reader, out.flags);
            break;
        default: {
            const char* name{UnsupportedElementName(discriminant)};
            if (name != nullptr) {
                error = strprintf("unsupported element type %s", name);
            } else {
                error = strprintf("unknown element discriminant %u", discriminant);
            }
            return false;
        }
        }
    }
    if (!ok || reader.HasError()) {
        error = strprintf("unable to deserialize element: %s", reader.GetError());
        return false;
    }
    if (!reader.IsEof()) {
        error = strprintf("element deserialization did not consume all bytes: %u left", reader.Remaining());
        return false;
    }
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// GroveDBProof envelope decoding
// (grovedb/src/operations/proof/mod.rs, bincode standard()+big_endian())
// ---------------------------------------------------------------------------

struct LayerProofNode {
    Bytes merk_proof;
    std::map<Bytes, LayerProofNode> lower_layers;
};

struct EnvelopeProof {
    //! 0 = GroveDBProofV0 (MerkOnlyLayerProof + embedded ProveOptions,
    //! lenient merk proof version), 1 = GroveDBProofV1 (LayerProof with
    //! ProofBytes, strict merk proof version, default ProveOptions).
    uint16_t version{1};
    //! ProveOptions::decrease_limit_on_empty_sub_query_result. Embedded in
    //! the proof for V0 (prover controlled); the default (true) for V1.
    bool decrease_limit_on_empty_sub_query_result{true};
    LayerProofNode root_layer;
};

bool DecodeLayerProof(BytesReader& reader, bool v1, size_t depth, LayerProofNode& out)
{
    // Mirrors MerkOnlyLayerProof::decode_with_depth (V0) and
    // LayerProof::decode_with_depth (V1).
    if (depth > MAX_PROOF_DEPTH) {
        return reader.SetError("proof layer nesting depth exceeded maximum");
    }
    if (v1) {
        // ProofBytes enum: only the Merk variant is verifiable here.
        uint64_t proof_type;
        if (!reader.ReadBincodeVarint(proof_type)) return false;
        switch (proof_type) {
        case 0:
            break; // Merk
        case 1:
            return reader.SetError("unsupported proof bytes type MMR");
        case 2:
            return reader.SetError("unsupported proof bytes type BulkAppendTree");
        case 3:
            return reader.SetError("unsupported proof bytes type DenseTree");
        case 4:
            return reader.SetError("unsupported proof bytes type CommitmentTree");
        default:
            return reader.SetError(strprintf("unknown proof bytes discriminant %u", proof_type));
        }
    }
    if (!reader.ReadBincodeByteVec(out.merk_proof, MAX_PROOF_BYTES)) return false;
    uint64_t child_count;
    if (!reader.ReadBincodeVarint(child_count)) return false;
    if (child_count > MAX_PROOF_DEPTH) return reader.SetError("proof layer has too many children");
    for (uint64_t i = 0; i < child_count; ++i) {
        Bytes key;
        if (!reader.ReadBincodeByteVec(key, MAX_PROOF_BYTES)) return false;
        LayerProofNode child;
        if (!DecodeLayerProof(reader, v1, depth + 1, child)) return false;
        out.lower_layers[std::move(key)] = std::move(child);
    }
    return true;
}

bool DecodeEnvelope(Span<const uint8_t> proof, EnvelopeProof& out, std::string& error)
{
    // grovedb/src/operations/proof/mod.rs decode_grovedb_proof_canonical:
    // bincode standard()+big_endian(), trailing bytes rejected.
    BytesReader reader{proof};
    uint64_t version;
    if (!reader.ReadBincodeVarint(version)) {
        error = reader.GetError();
        return false;
    }
    out = EnvelopeProof{};
    switch (version) {
    case 0: {
        out.version = 0;
        if (!DecodeLayerProof(reader, /*v1=*/false, 0, out.root_layer)) {
            error = reader.GetError();
            return false;
        }
        // Embedded ProveOptions { decrease_limit_on_empty_sub_query_result:
        // bool }. Prover controlled; see the V0 security note upstream.
        uint8_t flag;
        if (!reader.ReadU8(flag) || flag > 1) {
            error = reader.HasError() ? reader.GetError() : "invalid ProveOptions bool encoding";
            return false;
        }
        out.decrease_limit_on_empty_sub_query_result = flag == 1;
        break;
    }
    case 1: {
        out.version = 1;
        // V1 does not embed ProveOptions; the verifier uses the default.
        if (!DecodeLayerProof(reader, /*v1=*/true, 0, out.root_layer)) {
            error = reader.GetError();
            return false;
        }
        break;
    }
    default:
        error = strprintf("unknown GroveDBProof version discriminant %u", version);
        return false;
    }
    if (!reader.IsEof()) {
        error = strprintf("proof has %u trailing bytes after the encoded envelope", reader.Remaining());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PathQuery::query_items_at_path (grovedb/src/query/mod.rs)
// ---------------------------------------------------------------------------

//! Mirror of grovedb/src/query/mod.rs SinglePathSubquery, resolved for one
//! specific path.
struct LevelQuery {
    std::vector<merk::QueryItem> items;
    bool left_to_right{true};
    enum class SubqueryKind : uint8_t { NONE, ALWAYS, CONDITIONAL } subquery_kind{SubqueryKind::NONE};
    const std::vector<GroveQuery::ConditionalBranch>* conditional{nullptr};
    std::optional<Bytes> in_path;

    //! SinglePathSubquery::has_subquery_or_matching_in_path_on_key.
    bool HasSubqueryOrMatchingInPathOnKey(const Bytes& key) const
    {
        switch (subquery_kind) {
        case SubqueryKind::ALWAYS:
            return true;
        case SubqueryKind::CONDITIONAL:
            if (conditional != nullptr) {
                for (const auto& branch : *conditional) {
                    if (branch.item.Contains(key)) return true;
                }
            }
            break;
        case SubqueryKind::NONE:
            break;
        }
        return in_path.has_value() && *in_path == key;
    }
};

LevelQuery LevelFromQuery(const GroveQuery& query)
{
    // SinglePathSubquery::from_query
    LevelQuery out;
    out.items = query.items;
    out.left_to_right = query.left_to_right;
    if (!query.default_subquery_branch.Empty()) {
        out.subquery_kind = LevelQuery::SubqueryKind::ALWAYS;
    } else if (!query.conditional_subquery_branches.empty()) {
        out.subquery_kind = LevelQuery::SubqueryKind::CONDITIONAL;
        out.conditional = &query.conditional_subquery_branches;
    }
    return out;
}

LevelQuery LevelFromKeyInPath(const Bytes& key, bool subquery_is_last_path_item,
                              bool subquery_has_inner_subquery)
{
    // SinglePathSubquery::from_key_when_in_path
    LevelQuery out;
    out.items.push_back(merk::QueryItem::Key(key));
    if (!(subquery_is_last_path_item && !subquery_has_inner_subquery)) {
        out.in_path = key;
    }
    return out;
}

using PathSpan = Span<const Bytes>;

bool PathsMatchPrefix(PathSpan path, const std::vector<Bytes>& prefix_source, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (path[i] != prefix_source[i]) return false;
    }
    return true;
}

//! recursive_query_items in PathQuery::query_items_at_path. `path` is the
//! path relative to the layer this query applies to. Returns false when the
//! query has nothing to say about this path (Rust None).
bool RecursiveQueryItems(const GroveQuery& query, PathSpan path, LevelQuery& out)
{
    if (path.empty()) {
        out = LevelFromQuery(query);
        return true;
    }

    const Bytes& key{path.front()};
    const PathSpan rest{path.subspan(1)};

    const auto resolve_branch = [&](const GroveQuery::SubqueryBranch& branch) -> bool {
        // Shared logic between conditional and default subquery branches.
        if (branch.subquery_path.has_value()) {
            const std::vector<Bytes>& subquery_path{*branch.subquery_path};
            if (rest.size() <= subquery_path.size()) {
                if (PathsMatchPrefix(rest, subquery_path, rest.size())) {
                    if (rest.size() == subquery_path.size()) {
                        if (branch.subquery) {
                            out = LevelFromQuery(*branch.subquery);
                            return true;
                        }
                        return false;
                    }
                    const bool last_path_item{path.size() == subquery_path.size()};
                    out = LevelFromKeyInPath(subquery_path[rest.size()], last_path_item,
                                             branch.subquery != nullptr);
                    return true;
                }
            } else if (PathsMatchPrefix(rest, subquery_path, subquery_path.size()) && branch.subquery) {
                return RecursiveQueryItems(*branch.subquery, rest.subspan(subquery_path.size()), out);
            }
        } else if (branch.subquery) {
            return RecursiveQueryItems(*branch.subquery, rest, out);
        }
        return false;
    };

    for (const auto& conditional : query.conditional_subquery_branches) {
        if (conditional.item.Contains(key)) {
            // Once a conditional item matches the key the outcome is final.
            return resolve_branch(conditional.branch);
        }
    }

    return resolve_branch(query.default_subquery_branch);
}

//! PathQuery::query_items_at_path. `path` is absolute. Returns false when
//! the path is not covered by the query.
bool QueryItemsAtPath(const PathQuery& query, PathSpan path, LevelQuery& out)
{
    const size_t query_path_len{query.path.size()};
    if (path.size() < query_path_len) {
        if (!PathsMatchPrefix(path, query.path, path.size())) return false;
        out = LevelFromKeyInPath(query.path[path.size()], /*subquery_is_last_path_item=*/false,
                                 /*subquery_has_inner_subquery=*/true);
        return true;
    }
    if (!PathsMatchPrefix(path, query.path, query_path_len)) return false;
    if (path.size() == query_path_len) {
        out = LevelFromQuery(query.query);
        return true;
    }
    return RecursiveQueryItems(query.query, path.subspan(query_path_len), out);
}

// ---------------------------------------------------------------------------
// Recursive layer verification
// (grovedb/src/operations/proof/verify.rs verify_layer_proof{,_v1})
// ---------------------------------------------------------------------------

struct VerifyContext {
    const PathQuery& query;
    const VerifyOptions& options;
    const EnvelopeProof& envelope;
    std::optional<uint16_t> limit_left;
    std::vector<ProvedPathKeyValue> results;
};

bool VerifyLayer(VerifyContext& ctx, const LayerProofNode& layer, std::vector<Bytes>& current_path,
                 size_t depth, Hash256& out_root_hash, std::string& error)
{
    if (depth > MAX_PROOF_DEPTH) {
        error = "proof verification exceeded maximum depth limit";
        return false;
    }

    LevelQuery level;
    if (!QueryItemsAtPath(ctx.query, PathSpan{current_path}, level)) {
        error = "proof layer path is not covered by the path query";
        return false;
    }

    // V0 envelopes verify with the lenient merk proof version (0); V1 with
    // the strict version (1).
    merk::VerifyResult merk_result;
    if (!merk::ExecuteProof(layer.merk_proof, level.items, ctx.limit_left, level.left_to_right,
                            ctx.envelope.version, merk_result, error)) {
        return false;
    }

    std::vector<Bytes> verified_keys;

    if (merk_result.result_set.empty()) {
        if (ctx.envelope.decrease_limit_on_empty_sub_query_result && ctx.limit_left.has_value() &&
            *ctx.limit_left > 0) {
            --*ctx.limit_left;
        }
    } else {
        for (const merk::ProvedKeyValue& proved : merk_result.result_set) {
            const Bytes& key{proved.key};
            const Bytes& value_bytes{proved.value};
            Element element;
            if (!DecodeElement(value_bytes, element, error)) return false;
            verified_keys.push_back(key);

            const auto lower_it{layer.lower_layers.find(key)};
            if (lower_it != layer.lower_layers.end()) {
                if (!element.IsTree() || element.IsEmptyTree()) {
                    error = "proof has lower layer for a non-tree element";
                    return false;
                }
                current_path.push_back(key);
                LevelQuery deeper;
                if (!QueryItemsAtPath(ctx.query, PathSpan{current_path}, deeper)) {
                    // The query targets the tree element itself, not its
                    // contents.
                    ctx.results.push_back(
                        ProvedPathKeyValue{current_path, key, value_bytes, proved.value_hash});
                    if (ctx.limit_left.has_value() && *ctx.limit_left > 0) --*ctx.limit_left;
                    current_path.pop_back();
                    if (ctx.limit_left == std::optional<uint16_t>{0}) break;
                } else {
                    Hash256 lower_hash;
                    if (!VerifyLayer(ctx, lower_it->second, current_path, depth + 1, lower_hash, error)) {
                        return false;
                    }
                    current_path.pop_back();
                    // Subtree link check: the parent's committed value_hash
                    // must equal combine_hash(H(serialized element bytes),
                    // child layer root hash).
                    const Hash256 combined{merk::CombineHash(merk::ValueHash(value_bytes), lower_hash)};
                    if (proved.value_hash != combined) {
                        error = strprintf("mismatch in lower layer hash, expected %s, got %s",
                                          HexStr(proved.value_hash), HexStr(combined));
                        return false;
                    }
                    if (ctx.limit_left == std::optional<uint16_t>{0}) break;
                }
            } else if (element.IsItem() ||
                       (!level.HasSubqueryOrMatchingInPathOnKey(key) &&
                        (ctx.options.include_empty_trees_in_result ||
                         !(element.type == Element::Type::TREE && element.IsEmptyTree())))) {
                if (element.IsTree() && element.IsEmptyTree()) {
                    // Empty trees carry no lower layer; their committed
                    // value_hash must be combine_hash(H(value), NULL_HASH).
                    // Without this an attacker could swap tree types inside
                    // KVValueHash nodes.
                    const Hash256 expected{merk::CombineHash(merk::ValueHash(value_bytes), merk::NULL_HASH)};
                    if (proved.value_hash != expected) {
                        error = strprintf("empty tree value hash mismatch at key %s", HexStr(key));
                        return false;
                    }
                }
                // V1 strict rule: a non-empty merk tree without a subquery
                // must come from a KVValueHashFeatureTypeWithChildHash node
                // so the merk verifier confirmed combine_hash(H(value),
                // child_hash) == value_hash.
                if (ctx.envelope.version >= 1 && element.IsTree() && !element.IsEmptyTree() &&
                    !proved.child_hash_verified) {
                    error = strprintf("non-empty tree at key %s without subquery must use a "
                                      "child-hash-verified proof node",
                                      HexStr(key));
                    return false;
                }
                ctx.results.push_back(ProvedPathKeyValue{current_path, key, value_bytes, proved.value_hash});
                if (ctx.limit_left.has_value() && *ctx.limit_left > 0) --*ctx.limit_left;
                if (ctx.limit_left == std::optional<uint16_t>{0}) break;
            } else if (element.IsTree() && !element.IsEmptyTree() &&
                       level.HasSubqueryOrMatchingInPathOnKey(key)) {
                error = strprintf("proof is missing lower layer for non-empty tree at key %s", HexStr(key));
                return false;
            }
        }
    }

    if (ctx.options.verify_proof_succinctness) {
        for (const auto& [key, unused_child] : layer.lower_layers) {
            if (std::find(verified_keys.begin(), verified_keys.end(), key) == verified_keys.end()) {
                error = strprintf("proof contains extra lower layer for key %s not required by query",
                                  HexStr(key));
                return false;
            }
        }
    }

    out_root_hash = merk_result.root_hash;
    return true;
}

} // namespace

bool VerifyQuery(Span<const uint8_t> proof, const PathQuery& query, const VerifyOptions& options,
                 GroveVerifyResult& out, std::string& error)
{
    out = GroveVerifyResult{};
    EnvelopeProof envelope;
    if (!DecodeEnvelope(proof, envelope, error)) return false;

    VerifyContext ctx{query, options, envelope, query.limit, {}};
    std::vector<Bytes> current_path;
    Hash256 root_hash;
    if (!VerifyLayer(ctx, envelope.root_layer, current_path, 0, root_hash, error)) return false;

    out.root_hash = root_hash;
    out.results = std::move(ctx.results);
    out.envelope_version = envelope.version;
    return true;
}

bool VerifyQueryWithRootHash(Span<const uint8_t> proof, const PathQuery& query,
                             const VerifyOptions& options, const Hash256& expected_root_hash,
                             GroveVerifyResult& out, std::string& error)
{
    if (!VerifyQuery(proof, query, options, out, error)) return false;
    if (out.root_hash != expected_root_hash) {
        error = strprintf("proof did not match expected root hash: expected %s, got %s",
                          HexStr(expected_root_hash), HexStr(out.root_hash));
        return false;
    }
    return true;
}

} // namespace platform::grove
