// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/drive/queries.h>

#include <platform/dpp/identity.h>
#include <platform/proof/merk.h>

#include <algorithm>

namespace platform::drive {
namespace {

using platform::grove::Element;
using platform::grove::GroveQuery;
using platform::grove::GroveVerifyResult;
using platform::grove::PathQuery;
using platform::grove::ProvedPathKeyValue;
using platform::grove::VerifyOptions;
using platform::merk::QueryItem;

// Drive tree layout constants (see queries.h).
constexpr uint8_t ROOT_IDENTITIES = 32;
constexpr uint8_t ROOT_BALANCES = 96;
constexpr uint8_t ROOT_UNIQUE_PKH_TO_IDENTITIES = 24;
constexpr uint8_t ROOT_DATA_CONTRACT_DOCUMENTS = 64;
constexpr uint8_t ID_TREE_REVISION = 192;
constexpr uint8_t ID_TREE_NONCE = 64;
constexpr uint8_t ID_TREE_KEYS = 128;

bool RunQuery(Span<const uint8_t> proof, const PathQuery& query, GroveVerifyResult& out,
              std::string& error);

Bytes ByteSeg(uint8_t b) { return Bytes{b}; }

Bytes IdBytes(const Identifier& id) { return Bytes(id.begin(), id.end()); }

std::vector<Bytes> IdentityPath(const Identifier& id) { return {ByteSeg(ROOT_IDENTITIES), IdBytes(id)}; }

PathQuery SingleKeyQuery(std::vector<Bytes> path, Bytes key)
{
    PathQuery pq;
    pq.path = std::move(path);
    pq.query.items.push_back(QueryItem::Key(std::move(key)));
    pq.query.left_to_right = true;
    pq.limit = 1;
    return pq;
}

PathQuery RangeFullQuery(std::vector<Bytes> path)
{
    PathQuery pq;
    pq.path = std::move(path);
    pq.query.items.push_back(QueryItem::RangeFull());
    pq.query.left_to_right = true;
    return pq;
}

Bytes StringBytes(const std::string& value) { return Bytes(value.begin(), value.end()); }

GroveQuery::SubqueryBranch PathBranch(std::vector<Bytes> path, std::shared_ptr<GroveQuery> query = {})
{
    GroveQuery::SubqueryBranch branch;
    branch.subquery_path = std::move(path);
    branch.subquery = std::move(query);
    return branch;
}

std::shared_ptr<GroveQuery> FullRangeQuery()
{
    auto query = std::make_shared<GroveQuery>();
    query->items.push_back(QueryItem::RangeFull());
    return query;
}

std::shared_ptr<GroveQuery> FullRangeQueryWithDocumentIds()
{
    auto query = FullRangeQuery();
    query->default_subquery_branch = PathBranch({ByteSeg(0)}, FullRangeQuery());
    return query;
}

std::vector<Bytes> DocumentTypePath(const Identifier& contract, const std::string& type)
{
    return {ByteSeg(ROOT_DATA_CONTRACT_DOCUMENTS), IdBytes(contract), ByteSeg(1), StringBytes(type)};
}

PathQuery UniqueIndexQuery(const Identifier& contract, const std::string& type,
                           const std::string& field, Bytes key, uint16_t limit)
{
    PathQuery query;
    query.path = DocumentTypePath(contract, type);
    query.path.push_back(StringBytes(field));
    query.query.items.push_back(QueryItem::Key(std::move(key)));
    query.query.default_subquery_branch = PathBranch({ByteSeg(0)});
    query.limit = limit;
    return query;
}

bool ExtractDocuments(Span<const uint8_t> proof, const PathQuery& query,
                      std::vector<Bytes>& documents_out, Hash256& root_out,
                      std::string& error)
{
    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;
    documents_out.clear();
    for (const ProvedPathKeyValue& item : result.results) {
        Element element;
        if (!platform::grove::DecodeElement(item.value, element, error)) return false;
        if (element.type != Element::Type::ITEM) {
            error = "document query returned a non-item element";
            return false;
        }
        documents_out.push_back(std::move(element.item_value));
    }
    return true;
}

bool PathsEqual(const std::vector<Bytes>& a, const std::vector<Bytes>& b) { return a == b; }

bool RunQuery(Span<const uint8_t> proof, const PathQuery& query, GroveVerifyResult& out,
              std::string& error)
{
    VerifyOptions options; // defaults: raw results, include empty trees
    return platform::grove::VerifyQuery(proof, query, options, out, error);
}

//! Reads an 8-byte big-endian item value (revision / nonce).
bool DecodeU64BEItem(const Element& element, uint64_t& out, std::string& error)
{
    if (element.type != Element::Type::ITEM) {
        error = "expected an item element";
        return false;
    }
    if (element.item_value.size() != 8) {
        error = "expected an 8-byte value";
        return false;
    }
    uint64_t v = 0;
    for (uint8_t byte : element.item_value) v = (v << 8) | byte;
    out = v;
    return true;
}

} // namespace

bool VerifyIdentityBalance(Span<const uint8_t> proof, const Identifier& identity_id,
                           std::optional<uint64_t>& balance_out, Hash256& root_out,
                           std::string& error)
{
    const PathQuery query{SingleKeyQuery({ByteSeg(ROOT_BALANCES)}, IdBytes(identity_id))};
    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;

    if (result.results.empty()) {
        balance_out = std::nullopt;
        return true;
    }
    if (result.results.size() != 1) {
        error = "expected at most one balance result";
        return false;
    }
    const ProvedPathKeyValue& r{result.results.front()};
    if (!PathsEqual(r.path, {ByteSeg(ROOT_BALANCES)}) || r.key != IdBytes(identity_id)) {
        error = "balance result was not for the requested identity";
        return false;
    }
    Element element;
    if (!platform::grove::DecodeElement(Span<const uint8_t>(r.value), element, error)) return false;
    if (element.type != Element::Type::SUM_ITEM) {
        error = "balance element is not a sum item";
        return false;
    }
    if (element.sum_value < 0) {
        error = "balance can't be negative";
        return false;
    }
    balance_out = static_cast<uint64_t>(element.sum_value);
    return true;
}

bool VerifyIdentityRevision(Span<const uint8_t> proof, const Identifier& identity_id,
                            std::optional<uint64_t>& revision_out, Hash256& root_out,
                            std::string& error)
{
    const PathQuery query{SingleKeyQuery(IdentityPath(identity_id), ByteSeg(ID_TREE_REVISION))};
    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;

    if (result.results.empty()) {
        revision_out = std::nullopt;
        return true;
    }
    const ProvedPathKeyValue& r{result.results.front()};
    if (!PathsEqual(r.path, IdentityPath(identity_id)) || r.key != ByteSeg(ID_TREE_REVISION)) {
        error = "revision result was not for the requested identity";
        return false;
    }
    Element element;
    if (!platform::grove::DecodeElement(Span<const uint8_t>(r.value), element, error)) return false;
    uint64_t value = 0;
    if (!DecodeU64BEItem(element, value, error)) return false;
    revision_out = value;
    return true;
}

bool VerifyIdentityNonce(Span<const uint8_t> proof, const Identifier& identity_id,
                         std::optional<uint64_t>& nonce_out, Hash256& root_out, std::string& error)
{
    const PathQuery query{SingleKeyQuery(IdentityPath(identity_id), ByteSeg(ID_TREE_NONCE))};
    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;

    if (result.results.empty()) {
        nonce_out = std::nullopt;
        return true;
    }
    const ProvedPathKeyValue& r{result.results.front()};
    if (!PathsEqual(r.path, IdentityPath(identity_id)) || r.key != ByteSeg(ID_TREE_NONCE)) {
        error = "nonce result was not for the requested identity";
        return false;
    }
    Element element;
    if (!platform::grove::DecodeElement(Span<const uint8_t>(r.value), element, error)) return false;
    uint64_t value = 0;
    if (!DecodeU64BEItem(element, value, error)) return false;
    nonce_out = value;
    return true;
}

bool VerifyIdentityContractNonce(Span<const uint8_t> proof, const Identifier& identity_id,
                                 const Identifier& contract_id, std::optional<uint64_t>& nonce_out,
                                 Hash256& root_out, std::string& error)
{
    std::vector<Bytes> path{ByteSeg(ROOT_IDENTITIES), IdBytes(identity_id), ByteSeg(32), IdBytes(contract_id)};
    const PathQuery query{SingleKeyQuery(path, ByteSeg(0))};
    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;
    if (result.results.empty()) { nonce_out = std::nullopt; return true; }
    const ProvedPathKeyValue& r{result.results.front()};
    if (!PathsEqual(r.path, path) || r.key != ByteSeg(0)) {
        error = "contract nonce result was not for the requested identity and contract";
        return false;
    }
    Element element;
    if (!platform::grove::DecodeElement(Span<const uint8_t>(r.value), element, error)) return false;
    uint64_t value{0};
    if (!DecodeU64BEItem(element, value, error)) return false;
    nonce_out = value;
    return true;
}

bool VerifyIdentityKeys(Span<const uint8_t> proof, const Identifier& identity_id,
                        std::optional<std::vector<IdentityPublicKey>>& keys_out, Hash256& root_out,
                        std::string& error)
{
    const std::vector<Bytes> keys_path{ByteSeg(ROOT_IDENTITIES), IdBytes(identity_id),
                                       ByteSeg(ID_TREE_KEYS)};
    const PathQuery query{RangeFullQuery(keys_path)};
    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;

    std::vector<IdentityPublicKey> keys;
    for (const ProvedPathKeyValue& r : result.results) {
        if (!PathsEqual(r.path, keys_path)) {
            error = "identity key result on an unexpected path";
            return false;
        }
        Element element;
        if (!platform::grove::DecodeElement(Span<const uint8_t>(r.value), element, error)) return false;
        if (element.type != Element::Type::ITEM) {
            error = "identity key leaf is not an item";
            return false;
        }
        std::optional<IdentityPublicKey> key{
            platform::dpp::DecodeIdentityPublicKey(Span<const uint8_t>(element.item_value), error)};
        if (!key.has_value()) return false;
        keys.push_back(std::move(*key));
    }
    keys_out = std::move(keys);
    return true;
}

bool VerifyIdentityIdByPublicKeyHash(Span<const uint8_t> proof,
                                     const std::array<uint8_t, 20>& public_key_hash,
                                     std::optional<Identifier>& identity_out, Hash256& root_out,
                                     std::string& error)
{
    const Bytes hash_key(public_key_hash.begin(), public_key_hash.end());
    const PathQuery query{SingleKeyQuery({ByteSeg(ROOT_UNIQUE_PKH_TO_IDENTITIES)}, hash_key)};
    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;

    if (result.results.empty()) {
        identity_out = std::nullopt; // proven absent
        return true;
    }
    const ProvedPathKeyValue& r{result.results.front()};
    if (!PathsEqual(r.path, {ByteSeg(ROOT_UNIQUE_PKH_TO_IDENTITIES)}) || r.key != hash_key) {
        error = "public key hash result on an unexpected path";
        return false;
    }
    Element element;
    if (!platform::grove::DecodeElement(Span<const uint8_t>(r.value), element, error)) return false;
    if (element.type != Element::Type::ITEM || element.item_value.size() != 32) {
        error = "public key hash leaf is not a 32-byte identity id";
        return false;
    }
    Identifier id{};
    std::copy(element.item_value.begin(), element.item_value.end(), id.begin());
    identity_out = id;
    return true;
}

bool VerifyFullIdentity(Span<const uint8_t> balance_proof, Span<const uint8_t> revision_proof,
                        Span<const uint8_t> keys_proof, const Identifier& identity_id,
                        std::optional<Identity>& identity_out, Hash256& root_out, std::string& error)
{
    std::optional<uint64_t> balance;
    Hash256 balance_root{};
    if (!VerifyIdentityBalance(balance_proof, identity_id, balance, balance_root, error)) return false;

    std::optional<uint64_t> revision;
    Hash256 revision_root{};
    if (!VerifyIdentityRevision(revision_proof, identity_id, revision, revision_root, error)) return false;

    std::optional<std::vector<IdentityPublicKey>> keys;
    Hash256 keys_root{};
    if (!VerifyIdentityKeys(keys_proof, identity_id, keys, keys_root, error)) return false;

    // All three proofs must commit to the same platform state root.
    if (balance_root != revision_root || balance_root != keys_root) {
        error = "identity sub-proofs commit to different state roots";
        return false;
    }
    root_out = balance_root;

    const bool has_keys{keys.has_value() && !keys->empty()};
    if (!balance.has_value() && !revision.has_value() && !has_keys) {
        identity_out = std::nullopt; // fully absent
        return true;
    }
    if (!balance.has_value() || !revision.has_value() || !has_keys) {
        error = "identity proof is incomplete";
        return false;
    }

    Identity identity;
    identity.id = identity_id;
    identity.balance = *balance;
    identity.revision = *revision;
    identity.public_keys = std::move(*keys);
    identity_out = std::move(identity);
    return true;
}

bool VerifyDpnsNameExact(Span<const uint8_t> proof, const std::string& normalized_label,
                         std::vector<Bytes>& documents_out, Hash256& root_out, std::string& error)
{
    PathQuery query;
    query.path = DocumentTypePath(DPNS_CONTRACT_ID, "domain");
    query.path.push_back(StringBytes("normalizedParentDomainName"));
    query.path.push_back(StringBytes("dash"));
    query.path.push_back(StringBytes("normalizedLabel"));
    query.query.items.push_back(QueryItem::Key(StringBytes(normalized_label)));
    query.query.default_subquery_branch = PathBranch({ByteSeg(0)});
    query.limit = 1;
    return ExtractDocuments(proof, query, documents_out, root_out, error);
}

bool VerifyDpnsNamePrefix(Span<const uint8_t> proof, const std::string& normalized_prefix,
                          uint16_t limit, std::vector<Bytes>& documents_out, Hash256& root_out,
                          std::string& error)
{
    if (normalized_prefix.empty() || static_cast<uint8_t>(normalized_prefix.back()) == 0xff) {
        error = "invalid DPNS prefix";
        return false;
    }
    PathQuery query;
    query.path = DocumentTypePath(DPNS_CONTRACT_ID, "domain");
    query.path.push_back(StringBytes("normalizedParentDomainName"));
    query.path.push_back(StringBytes("dash"));
    query.path.push_back(StringBytes("normalizedLabel"));
    Bytes start{StringBytes(normalized_prefix)};
    Bytes end{start};
    ++end.back();
    query.query.items.push_back(QueryItem::Range(std::move(start), std::move(end)));
    query.query.default_subquery_branch = PathBranch({ByteSeg(0)});
    query.limit = limit;
    return ExtractDocuments(proof, query, documents_out, root_out, error);
}

bool VerifyDpnsNamesByIdentity(Span<const uint8_t> proof, const Identifier& identity_id,
                               uint16_t limit, std::vector<Bytes>& documents_out,
                               Hash256& root_out, std::string& error)
{
    PathQuery query;
    query.path = DocumentTypePath(DPNS_CONTRACT_ID, "domain");
    query.path.push_back(StringBytes("records.identity"));
    query.query.items.push_back(QueryItem::Key(IdBytes(identity_id)));
    query.query.default_subquery_branch = PathBranch({ByteSeg(0)}, FullRangeQuery());
    query.limit = limit;
    return ExtractDocuments(proof, query, documents_out, root_out, error);
}

bool VerifyDashPayProfileByOwner(Span<const uint8_t> proof, const Identifier& owner_id,
                                 std::vector<Bytes>& documents_out, Hash256& root_out,
                                 std::string& error)
{
    return ExtractDocuments(proof,
                            UniqueIndexQuery(DASHPAY_CONTRACT_ID, "profile", "$ownerId",
                                             IdBytes(owner_id), 1),
                            documents_out, root_out, error);
}

bool VerifyDashPayContactRequests(Span<const uint8_t> proof, const Identifier& identity_id,
                                  bool to_identity, uint16_t limit,
                                  std::vector<Bytes>& documents_out, Hash256& root_out,
                                  std::string& error)
{
    PathQuery query;
    query.path = DocumentTypePath(DASHPAY_CONTRACT_ID, "contactRequest");
    query.path.push_back(StringBytes(to_identity ? "toUserId" : "$ownerId"));
    query.query.items.push_back(QueryItem::Key(IdBytes(identity_id)));
    query.query.default_subquery_branch =
        PathBranch({StringBytes("$createdAt")}, FullRangeQueryWithDocumentIds());
    query.limit = limit;
    return ExtractDocuments(proof, query, documents_out, root_out, error);
}

} // namespace platform::drive
