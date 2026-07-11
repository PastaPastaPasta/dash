// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/drive/queries.h>

#include <platform/dpp/identity.h>
#include <platform/proof/merk.h>

#include <tinyformat.h>

#include <algorithm>
#include <limits>

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

// Contested-resource vote tree constants
// (rs-drive src/drive/votes/paths.rs).
constexpr uint8_t ROOT_VOTES = 112;
constexpr uint8_t CONTESTED_RESOURCE_TREE_KEY = 'c';
constexpr uint8_t ACTIVE_POLLS_TREE_KEY = 'p';
constexpr uint8_t CONTESTED_DOCUMENT_INDEXES_TREE_KEY = 1;
constexpr uint8_t VOTING_STORAGE_TREE_KEY = 1;
//! [0;32] = stored info item, [0;31]+1 = abstain sum tree, [0;31]+2 = lock
//! sum tree.
Bytes SpecialVoteKey(uint8_t last)
{
    Bytes key(32, 0);
    key.back() = last;
    return key;
}

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
    if (!platform::grove::VerifyQuery(proof, query, options, out, error)) return false;
    // Every Drive query the GUI issues goes through here. Require the strict
    // V1 GroveDBProof envelope: current Platform (protocol >= 4) only emits
    // V1, and the lenient V0 format does not bind the serialized element
    // bytes of a non-empty tree returned without a subquery, so accepting it
    // would let a malicious evonode (or on-path attacker, TLS being
    // unauthenticated by design) downgrade the proof and forge those bytes
    // while preserving the quorum-signed root hash.
    if (out.envelope_version < 1) {
        error = "rejected lenient V0 GroveDB proof envelope (strict V1 required)";
        return false;
    }
    return true;
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
    Bytes range_end{start};
    ++range_end.back();
    query.query.items.push_back(QueryItem::Range(std::move(start), std::move(range_end)));
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

namespace {

//! Decoded subset of dpp ContestedDocumentVotePollStoredInfo (bincode
//! standard()+big_endian(), rs-dpp
//! src/voting/vote_info_storage/contested_document_vote_poll_stored_info/):
//! the last finalized vote event and the poll status.
struct StoredVoteInfo {
    enum class Status : uint8_t { NOT_STARTED, AWARDED, LOCKED, STARTED };
    struct VoteChoice {
        enum class Kind : uint8_t { TOWARDS_IDENTITY, ABSTAIN, LOCK };
        Kind kind{Kind::ABSTAIN};
        Identifier identity{}; //!< TOWARDS_IDENTITY only
        uint32_t votes{0};     //!< sum of the voters' strengths
    };
    std::vector<VoteChoice> last_choices; //!< empty when never finalized
    uint64_t last_finalization_time_ms{0};
    Status status{Status::NOT_STARTED};
    Identifier awarded_to{}; //!< AWARDED only
};

bool ReadIdentifier(BytesReader& reader, Identifier& out)
{
    return reader.ReadInto(Span<uint8_t>{out.data(), out.size()});
}

//! BlockInfo { time_ms: u64, height: u64, core_height: u32, epoch: u16 };
//! only the block time survives into `time_ms_out`.
bool ReadBlockInfo(BytesReader& reader, uint64_t& time_ms_out)
{
    uint64_t height, core_height, epoch;
    return reader.ReadBincodeVarint(time_ms_out) && reader.ReadBincodeVarint(height) &&
           reader.ReadBincodeVarint(core_height) && reader.ReadBincodeVarint(epoch);
}

//! FinalizedResourceVoteChoicesWithVoterInfo { resource_vote_choice:
//! ResourceVoteChoice, voters: Vec<(Identifier, u8)> }. The per-voter list is
//! collapsed to the summed strength.
bool ReadVoteChoice(BytesReader& reader, StoredVoteInfo::VoteChoice& out)
{
    uint64_t kind;
    if (!reader.ReadBincodeVarint(kind)) return false;
    switch (kind) {
    case 0:
        out.kind = StoredVoteInfo::VoteChoice::Kind::TOWARDS_IDENTITY;
        if (!ReadIdentifier(reader, out.identity)) return false;
        break;
    case 1:
        out.kind = StoredVoteInfo::VoteChoice::Kind::ABSTAIN;
        break;
    case 2:
        out.kind = StoredVoteInfo::VoteChoice::Kind::LOCK;
        break;
    default:
        return reader.SetError(strprintf("unknown resource vote choice discriminant %u", kind));
    }
    uint64_t voter_count;
    if (!reader.ReadBincodeVarint(voter_count)) return false;
    if (voter_count > reader.Remaining()) return reader.SetError("voter count exceeds remaining data");
    uint64_t votes = 0;
    for (uint64_t i = 0; i < voter_count; ++i) {
        Identifier voter;
        uint8_t strength;
        if (!ReadIdentifier(reader, voter) || !reader.ReadU8(strength)) return false;
        votes += strength;
    }
    if (votes > std::numeric_limits<uint32_t>::max()) return reader.SetError("vote tally overflows u32");
    out.votes = static_cast<uint32_t>(votes);
    return true;
}

bool DecodeStoredVoteInfo(Span<const uint8_t> bytes, StoredVoteInfo& out, std::string& error)
{
    BytesReader reader{bytes};
    out = StoredVoteInfo{};
    uint64_t version;
    if (!reader.ReadBincodeVarint(version)) {
        error = reader.GetError();
        return false;
    }
    if (version != 0) {
        error = strprintf("unknown stored info version %u", version);
        return false;
    }
    uint64_t event_count{0};
    bool ok{reader.ReadBincodeVarint(event_count)};
    if (ok && event_count > reader.Remaining()) ok = reader.SetError("event count exceeds remaining data");
    for (uint64_t i = 0; ok && i < event_count; ++i) {
        uint64_t choice_count{0};
        ok = reader.ReadBincodeVarint(choice_count);
        if (ok && choice_count > reader.Remaining()) ok = reader.SetError("choice count exceeds remaining data");
        std::vector<StoredVoteInfo::VoteChoice> choices;
        for (uint64_t j = 0; ok && j < choice_count; ++j) {
            StoredVoteInfo::VoteChoice choice;
            ok = ReadVoteChoice(reader, choice);
            if (ok) choices.push_back(std::move(choice));
        }
        uint64_t start_time, finalization_time;
        ok = ok && ReadBlockInfo(reader, start_time) && ReadBlockInfo(reader, finalization_time);
        // ContestedDocumentVotePollWinnerInfo { NoWinner, WonByIdentity, Locked }
        uint64_t winner_kind{0};
        Identifier winner{};
        ok = ok && reader.ReadBincodeVarint(winner_kind);
        if (ok && winner_kind == 1) ok = ReadIdentifier(reader, winner);
        if (ok && winner_kind > 2) ok = reader.SetError(strprintf("unknown winner discriminant %u", winner_kind));
        if (ok) {
            // Only the last event matters for the outcome.
            out.last_choices = std::move(choices);
            out.last_finalization_time_ms = finalization_time;
        }
    }
    // ContestedDocumentVotePollStatus { NotStarted, Awarded(id), Locked, Started(BlockInfo) }
    uint64_t status{0};
    if (ok) ok = reader.ReadBincodeVarint(status);
    if (ok) {
        switch (status) {
        case 0:
            out.status = StoredVoteInfo::Status::NOT_STARTED;
            break;
        case 1:
            out.status = StoredVoteInfo::Status::AWARDED;
            ok = ReadIdentifier(reader, out.awarded_to);
            break;
        case 2:
            out.status = StoredVoteInfo::Status::LOCKED;
            break;
        case 3: {
            out.status = StoredVoteInfo::Status::STARTED;
            uint64_t start_time;
            ok = ReadBlockInfo(reader, start_time);
            break;
        }
        default:
            ok = reader.SetError(strprintf("unknown vote poll status discriminant %u", status));
        }
    }
    // Trailing locked_count (u16); consumed so the "all bytes read" check
    // below is meaningful.
    if (ok) {
        uint64_t locked_count{0};
        ok = reader.ReadBincodeVarint(locked_count);
    }
    if (!ok || reader.HasError()) {
        error = strprintf("unable to decode contested vote stored info: %s", reader.GetError());
        return false;
    }
    if (!reader.IsEof()) {
        error = "contested vote stored info has trailing bytes";
        return false;
    }
    return true;
}

} // namespace

bool VerifyContestedVoteState(Span<const uint8_t> proof, const Identifier& contract_id,
                              const std::string& document_type,
                              const std::vector<Bytes>& index_values, uint16_t count,
                              ContestedVoteState& out, Hash256& root_out, std::string& error)
{
    const Bytes stored_info_key{SpecialVoteKey(0)};
    const Bytes abstain_key{SpecialVoteKey(1)};
    const Bytes lock_key{SpecialVoteKey(2)};

    // Mirror of ContestedDocumentVotePollDriveQuery::construct_path_query for
    // result type VoteTally, allow_include_locked_and_abstaining_vote_tally =
    // true, no start_at (rs-drive src/query/vote_poll_vote_state_query.rs):
    // all keys of the contenders tree, tallies resolved through the [1]
    // voting sum tree, the stored-info item returned in place.
    PathQuery query;
    query.path = {ByteSeg(ROOT_VOTES), ByteSeg(CONTESTED_RESOURCE_TREE_KEY),
                  ByteSeg(ACTIVE_POLLS_TREE_KEY), IdBytes(contract_id), StringBytes(document_type),
                  ByteSeg(CONTESTED_DOCUMENT_INDEXES_TREE_KEY)};
    query.path.insert(query.path.end(), index_values.begin(), index_values.end());
    query.query.items.push_back(QueryItem::RangeFull());
    query.query.default_subquery_branch = PathBranch({ByteSeg(VOTING_STORAGE_TREE_KEY)});
    const auto conditional = [](Bytes key, GroveQuery::SubqueryBranch branch) {
        GroveQuery::ConditionalBranch out_branch;
        out_branch.item = QueryItem::Key(std::move(key));
        out_branch.branch = std::move(branch);
        return out_branch;
    };
    query.query.conditional_subquery_branches.push_back(
        conditional(lock_key, PathBranch({ByteSeg(VOTING_STORAGE_TREE_KEY)})));
    query.query.conditional_subquery_branches.push_back(
        conditional(abstain_key, PathBranch({ByteSeg(VOTING_STORAGE_TREE_KEY)})));
    query.query.conditional_subquery_branches.push_back(
        conditional(stored_info_key, GroveQuery::SubqueryBranch{}));
    query.limit = static_cast<uint16_t>(std::min<uint32_t>(uint32_t{count} + 3, 65535));

    GroveVerifyResult result;
    if (!RunQuery(proof, query, result, error)) return false;
    root_out = result.root_hash;

    out = ContestedVoteState{};
    out.contest_found = !result.results.empty();

    for (const ProvedPathKeyValue& r : result.results) {
        Element element;
        if (!platform::grove::DecodeElement(Span<const uint8_t>(r.value), element, error)) return false;
        if (r.path.size() < query.path.size() || r.path.empty()) {
            error = "contested vote result on an unexpected path";
            return false;
        }
        const Bytes& path_tail{r.path.back()};

        if (element.type == Element::Type::SUM_TREE) {
            // A tally: the enclosing tree's key names the contender (or the
            // abstain/lock buckets); the sum tree itself sits at key [1].
            if (r.path.size() != query.path.size() + 1 || r.key != ByteSeg(VOTING_STORAGE_TREE_KEY) ||
                !std::equal(query.path.begin(), query.path.end(), r.path.begin())) {
                error = "contested vote tally on an unexpected path";
                return false;
            }
            if (element.sum_value < 0 || element.sum_value > std::numeric_limits<uint32_t>::max()) {
                error = strprintf("vote tally out of range: %d", element.sum_value);
                return false;
            }
            const auto tally{static_cast<uint32_t>(element.sum_value)};
            if (path_tail == lock_key) {
                out.lock_votes = tally;
            } else if (path_tail == abstain_key) {
                out.abstain_votes = tally;
            } else if (path_tail.size() == 32) {
                Identifier id{};
                std::copy(path_tail.begin(), path_tail.end(), id.begin());
                out.contenders.emplace_back(id, tally);
            } else {
                error = "contested vote tally under a malformed contender key";
                return false;
            }
        } else if (element.type == Element::Type::ITEM && r.key == stored_info_key &&
                   r.path == query.path) {
            StoredVoteInfo info;
            if (!DecodeStoredVoteInfo(Span<const uint8_t>(element.item_value), info, error)) return false;
            if (info.status != StoredVoteInfo::Status::AWARDED &&
                info.status != StoredVoteInfo::Status::LOCKED) {
                continue; // active or reset poll: live tallies are authoritative
            }
            // Finished poll: the stored info replaces the live tallies
            // (rs-drive verify_vote_poll_vote_state_proof_v0).
            out.finished = true;
            out.finished_at_time_ms = info.last_finalization_time_ms;
            out.locked = info.status == StoredVoteInfo::Status::LOCKED;
            if (info.status == StoredVoteInfo::Status::AWARDED) out.winner = info.awarded_to;
            out.contenders.clear();
            uint32_t lock_total{0}, abstain_total{0};
            for (const StoredVoteInfo::VoteChoice& choice : info.last_choices) {
                switch (choice.kind) {
                case StoredVoteInfo::VoteChoice::Kind::TOWARDS_IDENTITY:
                    out.contenders.emplace_back(choice.identity, choice.votes);
                    break;
                case StoredVoteInfo::VoteChoice::Kind::ABSTAIN:
                    abstain_total += choice.votes;
                    break;
                case StoredVoteInfo::VoteChoice::Kind::LOCK:
                    lock_total += choice.votes;
                    break;
                }
            }
            out.abstain_votes = abstain_total;
            out.lock_votes = lock_total;
        } else {
            error = "unexpected element in contested vote state result";
            return false;
        }
    }
    return true;
}

} // namespace platform::drive
