// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/drive/queries.h>

#include <rust/platform/lib.h>

#include <algorithm>

namespace platform::drive {
namespace {

rust::Slice<const uint8_t> ToSlice(Span<const uint8_t> data)
{
    return {data.data(), data.size()};
}

rust::Slice<const uint8_t> ToSlice(const Identifier& id) { return {id.data(), id.size()}; }

//! Copies a bridge-returned 32-byte root hash into a Hash256.
bool RootFromFfi(const rust::Vec<uint8_t>& root, Hash256& root_out, std::string& error)
{
    if (root.size() != root_out.size()) {
        error = "bridge returned a malformed root hash";
        return false;
    }
    std::copy(root.begin(), root.end(), root_out.begin());
    return true;
}

bool IdFromFfi(const rust::Vec<uint8_t>& bytes, Identifier& id_out, std::string& error)
{
    if (bytes.size() != id_out.size()) {
        error = "bridge returned a malformed identifier";
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), id_out.begin());
    return true;
}

IdentityPublicKey KeyFromFfi(const platform_ffi::FfiIdentityKey& key)
{
    IdentityPublicKey out;
    out.id = key.id;
    out.purpose = static_cast<IdentityPublicKey::Purpose>(key.purpose);
    out.security_level = static_cast<IdentityPublicKey::SecurityLevel>(key.security_level);
    out.type = static_cast<IdentityPublicKey::Type>(key.key_type);
    out.read_only = key.read_only;
    out.data.assign(key.data.begin(), key.data.end());
    out.disabled_at = key.has_disabled_at ? std::optional<uint64_t>{key.disabled_at} : std::nullopt;
    return out;
}

//! Runs one bridge call, converting the rust::Error it throws on a failed or
//! malformed proof into the bool + error-string convention used here.
template <typename Fn>
bool Bridged(std::string& error, const Fn& fn)
{
    try {
        return fn();
    } catch (const rust::Error& e) {
        error = e.what();
        return false;
    }
}

bool U64Query(platform_ffi::FfiU64Result (*verify)(rust::Slice<const uint8_t>,
                                                   rust::Slice<const uint8_t>),
              Span<const uint8_t> proof, const Identifier& identity_id,
              std::optional<uint64_t>& value_out, Hash256& root_out, std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiU64Result result{verify(ToSlice(proof), ToSlice(identity_id))};
        if (!RootFromFfi(result.root_hash, root_out, error)) return false;
        value_out = result.present ? std::optional<uint64_t>{result.value} : std::nullopt;
        return true;
    });
}

bool DocsFromFfi(const platform_ffi::FfiDocsResult& result, std::vector<Bytes>& documents_out,
                 Hash256& root_out, std::string& error)
{
    if (!RootFromFfi(result.root_hash, root_out, error)) return false;
    documents_out.clear();
    documents_out.reserve(result.documents.size());
    for (const platform_ffi::FfiBytes& doc : result.documents) {
        documents_out.emplace_back(doc.data.begin(), doc.data.end());
    }
    return true;
}

} // namespace

bool VerifyIdentityBalance(Span<const uint8_t> proof, const Identifier& identity_id,
                           std::optional<uint64_t>& balance_out, Hash256& root_out,
                           std::string& error)
{
    return U64Query(platform_ffi::verify_identity_balance, proof, identity_id, balance_out,
                    root_out, error);
}

bool VerifyIdentityRevision(Span<const uint8_t> proof, const Identifier& identity_id,
                            std::optional<uint64_t>& revision_out, Hash256& root_out,
                            std::string& error)
{
    return U64Query(platform_ffi::verify_identity_revision, proof, identity_id, revision_out,
                    root_out, error);
}

bool VerifyIdentityNonce(Span<const uint8_t> proof, const Identifier& identity_id,
                         std::optional<uint64_t>& nonce_out, Hash256& root_out, std::string& error)
{
    return U64Query(platform_ffi::verify_identity_nonce, proof, identity_id, nonce_out, root_out,
                    error);
}

bool VerifyIdentityContractNonce(Span<const uint8_t> proof, const Identifier& identity_id,
                                 const Identifier& contract_id, std::optional<uint64_t>& nonce_out,
                                 Hash256& root_out, std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiU64Result result{platform_ffi::verify_identity_contract_nonce(
            ToSlice(proof), ToSlice(identity_id), ToSlice(contract_id))};
        if (!RootFromFfi(result.root_hash, root_out, error)) return false;
        nonce_out = result.present ? std::optional<uint64_t>{result.value} : std::nullopt;
        return true;
    });
}

bool VerifyIdentityKeys(Span<const uint8_t> proof, const Identifier& identity_id,
                        std::optional<std::vector<IdentityPublicKey>>& keys_out, Hash256& root_out,
                        std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiKeysResult result{
            platform_ffi::verify_identity_keys(ToSlice(proof), ToSlice(identity_id))};
        if (!RootFromFfi(result.root_hash, root_out, error)) return false;
        if (!result.present) {
            keys_out = std::nullopt;
            return true;
        }
        std::vector<IdentityPublicKey> keys;
        keys.reserve(result.keys.size());
        for (const platform_ffi::FfiIdentityKey& key : result.keys) keys.push_back(KeyFromFfi(key));
        keys_out = std::move(keys);
        return true;
    });
}

bool VerifyIdentityIdByPublicKeyHash(Span<const uint8_t> proof,
                                     const std::array<uint8_t, 20>& public_key_hash,
                                     std::optional<Identifier>& identity_out, Hash256& root_out,
                                     std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiIdResult result{platform_ffi::verify_identity_id_by_pubkey_hash(
            ToSlice(proof), rust::Slice<const uint8_t>{public_key_hash.data(), public_key_hash.size()})};
        if (!RootFromFfi(result.root_hash, root_out, error)) return false;
        if (!result.present) {
            identity_out = std::nullopt; // proven absent
            return true;
        }
        Identifier id{};
        if (!IdFromFfi(result.id, id, error)) return false;
        identity_out = id;
        return true;
    });
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
    return Bridged(error, [&] {
        return DocsFromFfi(platform_ffi::verify_dpns_name_exact(ToSlice(proof), normalized_label),
                           documents_out, root_out, error);
    });
}

bool VerifyDpnsNamePrefix(Span<const uint8_t> proof, const std::string& normalized_prefix,
                          uint16_t limit, std::vector<Bytes>& documents_out, Hash256& root_out,
                          std::string& error)
{
    return Bridged(error, [&] {
        return DocsFromFfi(
            platform_ffi::verify_dpns_name_prefix(ToSlice(proof), normalized_prefix, limit),
            documents_out, root_out, error);
    });
}

bool VerifyDpnsNamesByIdentity(Span<const uint8_t> proof, const Identifier& identity_id,
                               uint16_t limit, std::vector<Bytes>& documents_out,
                               Hash256& root_out, std::string& error)
{
    return Bridged(error, [&] {
        return DocsFromFfi(platform_ffi::verify_dpns_names_by_identity(ToSlice(proof),
                                                                       ToSlice(identity_id), limit),
                           documents_out, root_out, error);
    });
}

bool VerifyDashPayProfileByOwner(Span<const uint8_t> proof, const Identifier& owner_id,
                                 std::vector<Bytes>& documents_out, Hash256& root_out,
                                 std::string& error)
{
    return Bridged(error, [&] {
        return DocsFromFfi(
            platform_ffi::verify_dashpay_profile_by_owner(ToSlice(proof), ToSlice(owner_id)),
            documents_out, root_out, error);
    });
}

bool VerifyDashPayContactRequests(Span<const uint8_t> proof, const Identifier& identity_id,
                                  bool to_identity, uint16_t limit,
                                  std::vector<Bytes>& documents_out, Hash256& root_out,
                                  std::string& error)
{
    return Bridged(error, [&] {
        return DocsFromFfi(platform_ffi::verify_dashpay_contact_requests(
                               ToSlice(proof), ToSlice(identity_id), to_identity, limit),
                           documents_out, root_out, error);
    });
}

bool VerifyContestedVoteState(Span<const uint8_t> proof, const Identifier& contract_id,
                              const std::string& document_type,
                              const std::vector<Bytes>& index_values, uint16_t count,
                              ContestedVoteState& out, Hash256& root_out, std::string& error)
{
    return Bridged(error, [&] {
        // The tree-key-encoded index values of the contested DPNS index are
        // raw utf8 strings, matching the bridge's Vec<String> surface.
        rust::Vec<rust::String> values;
        values.reserve(index_values.size());
        for (const Bytes& value : index_values) {
            values.push_back(rust::String{reinterpret_cast<const char*>(value.data()), value.size()});
        }
        const platform_ffi::FfiContestedResult result{platform_ffi::verify_contested_vote_state(
            ToSlice(proof), ToSlice(contract_id), document_type, std::move(values), count)};
        if (!RootFromFfi(result.root_hash, root_out, error)) return false;

        out = ContestedVoteState{};
        out.contest_found = result.contest_found;
        out.contenders.reserve(result.contenders.size());
        for (const platform_ffi::FfiContender& contender : result.contenders) {
            Identifier id{};
            if (!IdFromFfi(contender.identity, id, error)) return false;
            out.contenders.emplace_back(id, contender.has_votes ? contender.votes : 0);
        }
        if (result.has_abstain) out.abstain_votes = result.abstain_votes;
        if (result.has_lock) out.lock_votes = result.lock_votes;
        out.finished = result.finished;
        out.locked = result.locked;
        if (result.has_winner) {
            Identifier winner{};
            if (!IdFromFfi(result.winner, winner, error)) return false;
            out.winner = winner;
        }
        out.finished_at_time_ms = result.finished_at_time_ms;
        return true;
    });
}

} // namespace platform::drive
