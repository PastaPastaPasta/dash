// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/drive/verify.h>

namespace platform::drive {

bool VerifyRootBinding(const Hash256& app_hash, const ProofEnvelope& envelope,
                       const BlockContext& ctx, const QuorumKeyLookup& lookup, std::string& error)
{
    std::optional<std::vector<uint8_t>> quorum_key{lookup(envelope.quorum_type, envelope.quorum_hash)};
    if (!quorum_key.has_value()) {
        error = "quorum public key not available for the signing quorum";
        return false;
    }
    return VerifyQuorumSig(app_hash, envelope, ctx, *quorum_key, error);
}

bool VerifyAndDecodeIdentityBalance(Span<const uint8_t> grovedb_proof, const ProofEnvelope& envelope,
                                    const BlockContext& ctx, const Identifier& identity_id,
                                    const QuorumKeyLookup& lookup,
                                    std::optional<uint64_t>& balance_out, std::string& error)
{
    Hash256 root{};
    if (!VerifyIdentityBalance(grovedb_proof, identity_id, balance_out, root, error)) return false;
    return VerifyRootBinding(root, envelope, ctx, lookup, error);
}

bool VerifyAndDecodeIdentityNonce(Span<const uint8_t> grovedb_proof, const ProofEnvelope& envelope,
                                  const BlockContext& ctx, const Identifier& identity_id,
                                  const QuorumKeyLookup& lookup, std::optional<uint64_t>& nonce_out,
                                  std::string& error)
{
    Hash256 root{};
    if (!VerifyIdentityNonce(grovedb_proof, identity_id, nonce_out, root, error)) return false;
    return VerifyRootBinding(root, envelope, ctx, lookup, error);
}

bool VerifyAndDecodeIdentityIdByPublicKeyHash(Span<const uint8_t> grovedb_proof,
                                              const ProofEnvelope& envelope, const BlockContext& ctx,
                                              const std::array<uint8_t, 20>& public_key_hash,
                                              const QuorumKeyLookup& lookup,
                                              std::optional<Identifier>& identity_out,
                                              std::string& error)
{
    Hash256 root{};
    if (!VerifyIdentityIdByPublicKeyHash(grovedb_proof, public_key_hash, identity_out, root, error)) {
        return false;
    }
    return VerifyRootBinding(root, envelope, ctx, lookup, error);
}

bool VerifyAndDecodeFullIdentity(Span<const uint8_t> balance_proof,
                                 Span<const uint8_t> revision_proof, Span<const uint8_t> keys_proof,
                                 const ProofEnvelope& envelope, const BlockContext& ctx,
                                 const Identifier& identity_id, const QuorumKeyLookup& lookup,
                                 std::optional<Identity>& identity_out, std::string& error)
{
    Hash256 root{};
    if (!VerifyFullIdentity(balance_proof, revision_proof, keys_proof, identity_id, identity_out,
                            root, error)) {
        return false;
    }
    return VerifyRootBinding(root, envelope, ctx, lookup, error);
}

} // namespace platform::drive
