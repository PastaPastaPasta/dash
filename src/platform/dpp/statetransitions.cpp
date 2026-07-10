// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/statetransitions.h>

#include <platform/dpp/bincode.h>
#include <platform/dpp/document.h>
#include <platform/params.h>

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <tinyformat.h>

#include <algorithm>
#include <utility>

/**
 * DPP state transition construction, byte-exact with dashpay/platform tag
 * v4.0.0 (Platform protocol version 12), validated against rs-dpp-generated
 * vectors in src/test/data/platform/dpp_st_vectors.json.
 *
 * Wire layout (bincode standard()+big_endian(), see platform/dpp/bincode.h):
 *  - StateTransition (rs-dpp/src/state_transition/mod.rs) is an unversioned
 *    enum; variant indexes: DataContractCreate=0, DataContractUpdate=1,
 *    Batch=2, IdentityCreate=3, ...
 *  - Batch document transitions serialize as BatchTransition::V1 with
 *    DocumentBaseTransition::V1 (token_payment_info = None): protocol
 *    versions 9..=12 all pin STATE_TRANSITION_SERIALIZATION_VERSIONS_V2
 *    (rs-platform-version/src/version/dpp_versions/
 *    dpp_state_transition_serialization_versions/v2.rs) whose
 *    batch_state_transition/document_base_state_transition
 *    default_current_version is 1.
 *  - Signing (rust-dashcore dash/src/signer.rs, rs-dpp state_transition
 *    sign_by_private_key): digest = double-SHA256 of the transition's
 *    signable bytes (the serialization with every
 *    #[platform_signable(exclude_from_sig_hash)] field omitted); the
 *    signature is the 65-byte compact recoverable ECDSA form whose header
 *    byte is 27 + recovery_id + 4 (compressed-pubkey convention).
 */
namespace platform::st {

namespace {

using dpp::Bytes;
using dpp::DocumentData;
using dpp::Value;
using dpp::Writer;

//! StateTransition enum variant indexes.
constexpr uint64_t ST_BATCH{2};
constexpr uint64_t ST_IDENTITY_CREATE{3};
//! BatchTransition::V1 (BatchedTransition vector; protocol >= 9).
constexpr uint64_t BATCH_V1{1};
//! BatchedTransition::Document / ::Token variant indexes.
constexpr uint64_t BATCHED_DOCUMENT{0};
//! DocumentTransition variant indexes.
constexpr uint64_t DOC_CREATE{0};
constexpr uint64_t DOC_REPLACE{1};
//! DocumentBaseTransition::V1 (adds Option<TokenPaymentInfo>).
constexpr uint64_t DOC_BASE_V1{1};
//! AssetLockProof variant indexes.
constexpr uint64_t PROOF_INSTANT{0};
constexpr uint64_t PROOF_CHAIN{1};

constexpr size_t COMPACT_SIG_SIZE{65};
constexpr size_t COMPRESSED_PUBKEY_SIZE{33};

//! Vote-resolution fund attached to contested DPNS domain creates:
//! rs-platform-version .../fee/vote_resolution_fund_fees/v1.rs
//! contested_document_vote_resolution_fund_required_amount (0.2 DASH in
//! credits), keyed by the contested unique index of the DPNS domain type.
constexpr uint64_t CONTESTED_DOCUMENT_FUND_CREDITS{20000000000};
const std::string CONTESTED_INDEX_NAME{"parentNameAndLabel"};

uint256 DoubleSha(Span<const uint8_t> data)
{
    uint256 out;
    CHash256().Write(data).Finalize(out);
    return out;
}

uint256 SingleSha(Span<const uint8_t> data)
{
    uint256 out;
    CSHA256().Write(data.data(), data.size()).Finalize(out.begin());
    return out;
}

std::array<uint8_t, 32> ToArray(const uint256& hash)
{
    std::array<uint8_t, 32> out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

//! Deterministic document entropy for DashPay documents:
//! DSHA256(owner_id || document_type_name || identity_contract_nonce LE64).
//! rs-dpp leaves entropy to the caller (the Rust SDK draws it at random);
//! deriving it from the (identity, contract nonce) pair keeps rebuilds of
//! the same transition byte-identical, so a GUI retry cannot register a
//! second document. DPNS documents use flow-specific entropy instead
//! (preorder: salted domain hash; domain: preorder salt).
std::array<uint8_t, 32> DeriveEntropy(const Identifier& owner,
                                      const std::string& document_type_name,
                                      uint64_t identity_contract_nonce)
{
    CHash256 hasher;
    hasher.Write(owner);
    hasher.Write(MakeUCharSpan(document_type_name));
    uint8_t nonce_le[8];
    WriteLE64(nonce_le, identity_contract_nonce);
    hasher.Write(nonce_le);
    uint256 hash;
    hasher.Finalize(hash);
    return ToArray(hash);
}

//! One document create/replace inside a Batch transition.
struct DocumentSpec {
    bool is_replace{false};
    Identifier document_id{};
    uint64_t identity_contract_nonce{0};
    std::string document_type_name;
    Identifier data_contract_id{};
    std::array<uint8_t, 32> entropy{}; //!< create only
    uint64_t revision{0};              //!< replace only
    DocumentData data;
    //! (index name, credits); create only.
    std::optional<std::pair<std::string, uint64_t>> prefunded_voting_balance;
};

//! Serializes a single-document BatchTransition::V1. With signature ==
//! nullptr the signable form is produced (signature_public_key_id and
//! signature are #[platform_signable(exclude_from_sig_hash)] in
//! BatchTransitionV1).
Bytes EncodeDocumentBatch(const Identifier& owner_id,
                          const DocumentSpec& spec,
                          uint32_t signature_public_key_id,
                          const Bytes* signature)
{
    Writer writer;
    writer.WriteVarint(ST_BATCH);
    writer.WriteVarint(BATCH_V1);
    writer.WriteBytes(owner_id);
    writer.WriteVarint(1); // transitions: Vec<BatchedTransition>
    writer.WriteVarint(BATCHED_DOCUMENT);
    writer.WriteVarint(spec.is_replace ? DOC_REPLACE : DOC_CREATE);
    writer.WriteVarint(0); // DocumentCreateTransition::V0 / DocumentReplaceTransition::V0
    // DocumentBaseTransition::V1
    writer.WriteVarint(DOC_BASE_V1);
    writer.WriteBytes(spec.document_id);
    writer.WriteVarint(spec.identity_contract_nonce);
    writer.WriteString(spec.document_type_name);
    writer.WriteBytes(spec.data_contract_id);
    writer.WriteOptionTag(false); // token_payment_info
    if (spec.is_replace) {
        writer.WriteVarint(spec.revision);
        EncodeDocumentData(writer, spec.data);
    } else {
        writer.WriteBytes(spec.entropy);
        EncodeDocumentData(writer, spec.data);
        writer.WriteOptionTag(spec.prefunded_voting_balance.has_value());
        if (spec.prefunded_voting_balance) {
            writer.WriteString(spec.prefunded_voting_balance->first);
            writer.WriteVarint(spec.prefunded_voting_balance->second);
        }
    }
    writer.WriteVarint(0); // user_fee_increase
    if (signature != nullptr) {
        writer.WriteVarint(signature_public_key_id);
        writer.WriteByteVec(*signature);
    }
    return writer.Take();
}

Result<BuiltTransition> SignDocumentBatch(const Identifier& owner_id,
                                          const DocumentSpec& spec,
                                          uint32_t signature_public_key_id,
                                          const Signer& signer)
{
    if (!signer) return {std::nullopt, "no signer provided"};
    const Bytes signable{EncodeDocumentBatch(owner_id, spec, signature_public_key_id, nullptr)};
    const uint256 digest{DoubleSha(signable)};
    Bytes signature;
    if (!signer(digest, signature)) {
        return {std::nullopt, "signing failed (wallet locked or key unavailable)"};
    }
    if (signature.size() != COMPACT_SIG_SIZE) {
        return {std::nullopt, strprintf("unexpected signature size %d (want %d)", signature.size(), COMPACT_SIG_SIZE)};
    }
    BuiltTransition built;
    built.bytes = EncodeDocumentBatch(owner_id, spec, signature_public_key_id, &signature);
    built.hash = SingleSha(built.bytes);
    return {std::move(built), ""};
}

//! Serializes an IdentityCreateTransition (::V0). With signatures == nullptr
//! the signable form is produced: per-key signatures, the outer signature
//! and the identity id are all excluded from the signable bytes
//! (rs-dpp .../identity_create_transition/v0/mod.rs, .../public_key_in_creation/v0/mod.rs).
Bytes EncodeIdentityCreate(const std::vector<NewIdentityKey>& keys,
                           const std::variant<InstantAssetLockProof, ChainAssetLockProof>& proof,
                           const std::vector<Bytes>* key_signatures,
                           const Bytes* signature,
                           const Identifier* identity_id)
{
    Writer writer;
    writer.WriteVarint(ST_IDENTITY_CREATE);
    writer.WriteVarint(0); // IdentityCreateTransition::V0
    writer.WriteVarint(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        const NewIdentityKey& key{keys[i]};
        // IdentityPublicKeyInCreation::V0; field order differs from
        // IdentityPublicKeyV0 (key_type before purpose).
        writer.WriteVarint(0);
        writer.WriteVarint(key.id);
        writer.WriteVarint(0); // KeyType::ECDSA_SECP256K1
        writer.WriteVarint(static_cast<uint64_t>(key.purpose));
        writer.WriteVarint(static_cast<uint64_t>(key.security_level));
        writer.WriteOptionTag(false); // contract_bounds
        writer.WriteBool(false);      // read_only
        writer.WriteByteVec(key.pubkey);
        if (key_signatures != nullptr) writer.WriteByteVec((*key_signatures)[i]);
    }
    if (const auto* instant = std::get_if<InstantAssetLockProof>(&proof)) {
        // AssetLockProof::Instant serializes through serde as
        // RawInstantLockProof { instant_lock, transaction, output_index }
        // with the lock and transaction as raw consensus bytes
        // (rs-dpp .../asset_lock_proof/instant/instant_asset_lock_proof.rs).
        writer.WriteVarint(PROOF_INSTANT);
        writer.WriteByteVec(instant->instant_lock);
        writer.WriteByteVec(instant->transaction);
        writer.WriteVarint(instant->output_index);
    } else {
        const auto& chain = std::get<ChainAssetLockProof>(proof);
        // AssetLockProof::Chain serializes through serde as
        // { core_chain_locked_height, out_point: { txid: bytes, vout } }.
        writer.WriteVarint(PROOF_CHAIN);
        writer.WriteVarint(chain.core_chain_locked_height);
        writer.WriteByteVec(Span{chain.out_point}.first(32));
        writer.WriteVarint(ReadLE32(chain.out_point.data() + 32));
    }
    writer.WriteVarint(0); // user_fee_increase
    if (signature != nullptr) {
        writer.WriteByteVec(*signature);
        writer.WriteBytes(*identity_id);
    }
    return writer.Take();
}

} // namespace

Identifier IdentityIdFromOutpoint(const std::array<uint8_t, 36>& out_point)
{
    return ToArray(DoubleSha(out_point));
}

Result<BuiltTransition> BuildIdentityCreate(
    const std::variant<InstantAssetLockProof, ChainAssetLockProof>& proof,
    const std::vector<NewIdentityKey>& keys,
    const Signer& asset_lock_signer)
{
    if (keys.empty()) return {std::nullopt, "no identity keys provided"};
    if (!asset_lock_signer) return {std::nullopt, "no asset lock signer provided"};

    // rs-dpp registers Identity::public_keys() (a BTreeMap keyed by id), so
    // the keys serialize in ascending id order with no duplicates.
    std::vector<NewIdentityKey> sorted_keys{keys};
    std::sort(sorted_keys.begin(), sorted_keys.end(),
              [](const NewIdentityKey& a, const NewIdentityKey& b) { return a.id < b.id; });
    for (size_t i = 0; i < sorted_keys.size(); ++i) {
        const NewIdentityKey& key{sorted_keys[i]};
        if (i > 0 && key.id == sorted_keys[i - 1].id) {
            return {std::nullopt, strprintf("duplicate identity key id %d", key.id)};
        }
        if (key.pubkey.size() != COMPRESSED_PUBKEY_SIZE) {
            return {std::nullopt, strprintf("identity key %d: unexpected public key size %d", key.id, key.pubkey.size())};
        }
        if (!key.signer) return {std::nullopt, strprintf("identity key %d: no signer", key.id)};
    }

    // Derive the identity id from the asset lock outpoint.
    Identifier identity_id;
    if (const auto* instant = std::get_if<InstantAssetLockProof>(&proof)) {
        // The txid of a Dash special transaction is the double-SHA256 of its
        // full serialization (including the extra payload), so the asset
        // lock transaction does not need to be re-parsed here.
        const uint256 txid{DoubleSha(instant->transaction)};
        std::array<uint8_t, 36> out_point;
        std::copy(txid.begin(), txid.end(), out_point.begin());
        WriteLE32(out_point.data() + 32, instant->output_index);
        identity_id = IdentityIdFromOutpoint(out_point);
    } else {
        identity_id = IdentityIdFromOutpoint(std::get<ChainAssetLockProof>(proof).out_point);
    }

    // Every registered key proves ownership by signing the same digest as
    // the asset-lock key: the double-SHA256 of the signable bytes (rs-dpp
    // IdentityCreateTransitionMethodsV0::try_from_identity_with_signer_and_private_key).
    const Bytes signable{EncodeIdentityCreate(sorted_keys, proof, nullptr, nullptr, nullptr)};
    const uint256 digest{DoubleSha(signable)};

    std::vector<Bytes> key_signatures;
    key_signatures.reserve(sorted_keys.size());
    for (const NewIdentityKey& key : sorted_keys) {
        Bytes sig;
        if (!key.signer(digest, sig)) {
            return {std::nullopt, strprintf("identity key %d: signing failed", key.id)};
        }
        if (sig.size() != COMPACT_SIG_SIZE) {
            return {std::nullopt, strprintf("identity key %d: unexpected signature size %d", key.id, sig.size())};
        }
        key_signatures.push_back(std::move(sig));
    }
    Bytes asset_lock_signature;
    if (!asset_lock_signer(digest, asset_lock_signature)) {
        return {std::nullopt, "asset lock key: signing failed"};
    }
    if (asset_lock_signature.size() != COMPACT_SIG_SIZE) {
        return {std::nullopt, strprintf("asset lock key: unexpected signature size %d", asset_lock_signature.size())};
    }

    BuiltTransition built;
    built.bytes = EncodeIdentityCreate(sorted_keys, proof, &key_signatures, &asset_lock_signature, &identity_id);
    built.hash = SingleSha(built.bytes);
    return {std::move(built), ""};
}

Result<BuiltTransition> BuildDpnsPreorder(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const std::array<uint8_t, 32>& salted_domain_hash,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    DocumentSpec spec;
    spec.identity_contract_nonce = identity_contract_nonce;
    spec.document_type_name = "preorder";
    spec.data_contract_id = DPNS_CONTRACT_ID;
    // The preorder document has no natural id source; the salted domain
    // hash is already blinded and unique per (name, salt), so it doubles as
    // the document entropy.
    spec.entropy = salted_domain_hash;
    spec.document_id = dpp::GenerateDocumentId(spec.data_contract_id, identity,
                                               spec.document_type_name, spec.entropy);
    spec.data.emplace("saltedDomainHash", Value::MakeBytes32(salted_domain_hash));
    return SignDocumentBatch(identity, spec, signature_public_key_id, signer);
}

Result<BuiltTransition> BuildDpnsDomain(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const std::string& label,
    const std::string& normalized_label,
    const std::string& parent_domain,
    const std::array<uint8_t, 32>& preorder_salt,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    if (label.empty() || normalized_label.empty() || parent_domain.empty()) {
        return {std::nullopt, "empty DPNS label or parent domain"};
    }
    if (NormalizeLabel(label) != normalized_label) {
        return {std::nullopt, "normalized label does not match label"};
    }
    DocumentSpec spec;
    spec.identity_contract_nonce = identity_contract_nonce;
    spec.document_type_name = "domain";
    spec.data_contract_id = DPNS_CONTRACT_ID;
    // The preorder salt is drawn fresh per registration attempt, making it a
    // suitable deterministic document entropy for the paired domain create.
    spec.entropy = preorder_salt;
    spec.document_id = dpp::GenerateDocumentId(spec.data_contract_id, identity,
                                               spec.document_type_name, spec.entropy);
    spec.data.emplace("label", Value::Text(label));
    spec.data.emplace("normalizedLabel", Value::Text(normalized_label));
    spec.data.emplace("parentDomainName", Value::Text(parent_domain));
    spec.data.emplace("normalizedParentDomainName", Value::Text(NormalizeLabel(parent_domain)));
    spec.data.emplace("preorderSalt", Value::MakeBytes32(preorder_salt));
    spec.data.emplace("records", Value::MakeMap({{Value::Text("identity"), Value::Id(identity)}}));
    spec.data.emplace("subdomainRules", Value::MakeMap({{Value::Text("allowSubdomains"), Value::Boolean(false)}}));
    // Contested names must prefund the masternode vote resolution; rs-dpp
    // fills this from the contested unique index of the DPNS domain type
    // (DocumentTypeV0Methods::prefunded_voting_balance_for_document_v0).
    if (IsContestedLabel(normalized_label)) {
        spec.prefunded_voting_balance = {CONTESTED_INDEX_NAME, CONTESTED_DOCUMENT_FUND_CREDITS};
    }
    return SignDocumentBatch(identity, spec, signature_public_key_id, signer);
}

Result<BuiltTransition> BuildProfile(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const Profile& profile,
    uint64_t revision,
    const std::optional<Identifier>& existing_document_id,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    if (!profile.avatar_hash.empty() && profile.avatar_hash.size() != 32) {
        return {std::nullopt, "avatar hash must be 32 bytes"};
    }
    if (!profile.avatar_fingerprint.empty() && profile.avatar_fingerprint.size() != 8) {
        return {std::nullopt, "avatar fingerprint must be 8 bytes"};
    }
    DocumentSpec spec;
    spec.identity_contract_nonce = identity_contract_nonce;
    spec.document_type_name = "profile";
    spec.data_contract_id = DASHPAY_CONTRACT_ID;
    if (existing_document_id) {
        if (revision < 2) return {std::nullopt, "profile replace requires revision > 1"};
        spec.is_replace = true;
        spec.document_id = *existing_document_id;
        spec.revision = revision;
    } else {
        if (revision != 1) return {std::nullopt, "profile create requires revision 1"};
        spec.entropy = DeriveEntropy(identity, spec.document_type_name, identity_contract_nonce);
        spec.document_id = dpp::GenerateDocumentId(spec.data_contract_id, identity,
                                                   spec.document_type_name, spec.entropy);
    }
    // All profile fields are optional in the DashPay contract; $createdAt /
    // $updatedAt are assigned by the chain from block time and never appear
    // in the transition.
    if (!profile.display_name.empty()) spec.data.emplace("displayName", Value::Text(profile.display_name));
    if (!profile.public_message.empty()) spec.data.emplace("publicMessage", Value::Text(profile.public_message));
    if (!profile.avatar_url.empty()) spec.data.emplace("avatarUrl", Value::Text(profile.avatar_url));
    if (!profile.avatar_hash.empty()) {
        std::array<uint8_t, 32> hash;
        std::copy(profile.avatar_hash.begin(), profile.avatar_hash.end(), hash.begin());
        spec.data.emplace("avatarHash", Value::MakeBytes32(hash));
    }
    if (!profile.avatar_fingerprint.empty()) {
        spec.data.emplace("avatarFingerprint", Value::MakeBytes(profile.avatar_fingerprint));
    }
    return SignDocumentBatch(identity, spec, signature_public_key_id, signer);
}

Result<BuiltTransition> BuildContactRequest(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const ContactRequest& request,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    if (request.encrypted_public_key.size() != 96) {
        return {std::nullopt, "encrypted public key must be 96 bytes"};
    }
    if (!request.encrypted_account_label.empty() &&
        (request.encrypted_account_label.size() < 48 || request.encrypted_account_label.size() > 80)) {
        return {std::nullopt, "encrypted account label must be 48-80 bytes"};
    }
    DocumentSpec spec;
    spec.identity_contract_nonce = identity_contract_nonce;
    spec.document_type_name = "contactRequest";
    spec.data_contract_id = DASHPAY_CONTRACT_ID;
    spec.entropy = DeriveEntropy(identity, spec.document_type_name, identity_contract_nonce);
    spec.document_id = dpp::GenerateDocumentId(spec.data_contract_id, identity,
                                               spec.document_type_name, spec.entropy);
    // DashPay contract v1: $createdAt and $createdAtCoreBlockHeight are
    // chain-assigned system fields, so ContactRequest::created_at and
    // ::core_height_created_at do not serialize into the transition
    // (packages/dashpay-contract/schema/v1/dashpay.schema.json).
    spec.data.emplace("toUserId", Value::Id(request.to_user_id));
    spec.data.emplace("encryptedPublicKey", Value::MakeBytes(request.encrypted_public_key));
    spec.data.emplace("senderKeyIndex", Value::U32(request.sender_key_index));
    spec.data.emplace("recipientKeyIndex", Value::U32(request.recipient_key_index));
    spec.data.emplace("accountReference", Value::U32(request.account_reference));
    if (!request.encrypted_account_label.empty()) {
        spec.data.emplace("encryptedAccountLabel", Value::MakeBytes(request.encrypted_account_label));
    }
    return SignDocumentBatch(identity, spec, signature_public_key_id, signer);
}

std::array<uint8_t, 32> SaltedDomainHash(
    const std::array<uint8_t, 32>& salt,
    const std::string& normalized_label,
    const std::string& parent_domain)
{
    // DSHA256(salt || normalized_label || "." || parent_domain), per the
    // Rust SDK DPNS registration flow
    // (packages/rs-sdk/src/platform/dpns_usernames/mod.rs register_dpns_name).
    const std::string full_name{normalized_label + "." + parent_domain};
    CHash256 hasher;
    hasher.Write(salt);
    hasher.Write(MakeUCharSpan(full_name));
    uint256 hash;
    hasher.Finalize(hash);
    return ToArray(hash);
}

std::string NormalizeLabel(const std::string& label)
{
    // Port of rs-dpp/src/util/strings.rs convert_to_homograph_safe_chars:
    // lower-case, then o->0 and l/i->1. DPNS labels are ASCII-only by
    // contract pattern, so ASCII lower-casing matches Rust's to_lowercase().
    std::string normalized;
    normalized.reserve(label.size());
    for (char c : label) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        switch (c) {
        case 'o': normalized.push_back('0'); break;
        case 'l':
        case 'i': normalized.push_back('1'); break;
        default: normalized.push_back(c);
        }
    }
    return normalized;
}

bool IsContestedLabel(const std::string& normalized_label)
{
    // DPNS domain contested index rule: the parentNameAndLabel index is
    // contested when normalizedLabel matches ^[a-zA-Z01-]{3,19}$
    // (packages/dpns-contract/schema/v1/dpns-contract-documents.json,
    // indices[0].contested.fieldMatches). Note digits 2-9 make a name
    // non-contested and normalized labels are already lower-case.
    if (normalized_label.size() < 3 || normalized_label.size() > 19) return false;
    for (char c : normalized_label) {
        const bool allowed{(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == '0' || c == '1' || c == '-'};
        if (!allowed) return false;
    }
    return true;
}

} // namespace platform::st
