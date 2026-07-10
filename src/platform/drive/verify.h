// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DRIVE_VERIFY_H
#define BITCOIN_PLATFORM_DRIVE_VERIFY_H

#include <platform/drive/queries.h>
#include <platform/drive/quorumsig.h>
#include <platform/serialize.h>
#include <platform/types.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

//! Top-level "verify a DAPI response" layer: verifies the GroveDB proof to a
//! root hash, then verifies the Tenderdash quorum threshold signature that
//! binds that root to a signed Platform block, and finally decodes the typed
//! result. This is the only entry point the GUI should call for proved data.
namespace platform::drive {

//! Resolves a quorum's basic-scheme (48-byte) BLS public key from its type and
//! hash. The client wires this to interfaces::Node::LLMQ::getPlatformQuorums.
//! Returns std::nullopt when the quorum is unknown (which fails verification).
using QuorumKeyLookup =
    std::function<std::optional<std::vector<uint8_t>>(uint8_t quorum_type, const Hash256& quorum_hash)>;

//! Verifies the quorum-signature binding of an already-computed state root.
//! Looks the quorum key up via `lookup`, then checks the threshold signature.
bool VerifyRootBinding(const Hash256& app_hash, const ProofEnvelope& envelope,
                       const BlockContext& ctx, const QuorumKeyLookup& lookup, std::string& error);

//! getIdentityBalance end to end: GroveDB proof -> root -> quorum sig -> value.
bool VerifyAndDecodeIdentityBalance(Span<const uint8_t> grovedb_proof, const ProofEnvelope& envelope,
                                    const BlockContext& ctx, const Identifier& identity_id,
                                    const QuorumKeyLookup& lookup,
                                    std::optional<uint64_t>& balance_out, std::string& error);

//! getIdentityNonce end to end.
bool VerifyAndDecodeIdentityNonce(Span<const uint8_t> grovedb_proof, const ProofEnvelope& envelope,
                                  const BlockContext& ctx, const Identifier& identity_id,
                                  const QuorumKeyLookup& lookup, std::optional<uint64_t>& nonce_out,
                                  std::string& error);

//! getIdentityByPublicKeyHash end to end (present -> id, absent -> nullopt).
bool VerifyAndDecodeIdentityIdByPublicKeyHash(Span<const uint8_t> grovedb_proof,
                                              const ProofEnvelope& envelope, const BlockContext& ctx,
                                              const std::array<uint8_t, 20>& public_key_hash,
                                              const QuorumKeyLookup& lookup,
                                              std::optional<Identifier>& identity_out,
                                              std::string& error);

//! getIdentity (full) end to end. The three sub-proofs share one signed root,
//! so a single envelope binds all of them.
bool VerifyAndDecodeFullIdentity(Span<const uint8_t> balance_proof,
                                 Span<const uint8_t> revision_proof, Span<const uint8_t> keys_proof,
                                 const ProofEnvelope& envelope, const BlockContext& ctx,
                                 const Identifier& identity_id, const QuorumKeyLookup& lookup,
                                 std::optional<Identity>& identity_out, std::string& error);

} // namespace platform::drive

#endif // BITCOIN_PLATFORM_DRIVE_VERIFY_H
