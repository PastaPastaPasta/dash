// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DRIVE_QUERIES_H
#define BITCOIN_PLATFORM_DRIVE_QUERIES_H

#include <platform/proof/grovedb.h>
#include <platform/serialize.h>
#include <platform/types.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//! Per-query GroveDB path logic for the DAPI queries the dash-qt Platform GUI
//! makes. Each function verifies a real Drive proof with the layered GroveDB
//! verifier (platform/proof/grovedb.h), matches the returned (path, key,
//! element) tuples against the Drive tree layout for that query, decodes the
//! leaf bytes with the DPP decoders (platform/dpp/), and returns typed
//! results. std::nullopt means the object was cryptographically proven absent.
//!
//! Drive tree layout (dashpay/platform tag v4.0.0, protocol version 12):
//!  - RootTree children (packages/rs-drive/src/drive/mod.rs enum RootTree):
//!      Balances = 96 (sum tree), Identities = 32,
//!      UniquePublicKeyHashesToIdentities = 24.
//!  - Per-identity sub-structure (packages/rs-drive/src/drive/identity/mod.rs
//!    enum IdentityRootStructure), under path [[32], identity_id]:
//!      IdentityTreeRevision = 192 (8-byte big-endian item),
//!      IdentityTreeNonce    = 64  (8-byte big-endian item),
//!      IdentityTreeKeys     = 128 (sub-tree of serialized IdentityPublicKeys
//!                                  keyed by varint KeyID).
//!  - balance leaf: sum item in balance_path [[96]] keyed by identity id
//!    (packages/rs-drive/src/drive/balances/mod.rs balance_path_vec,
//!    verify/identity/verify_identity_balance_for_identity_id/v0).
//!
//! Sources for the match logic (packages/rs-drive/src/verify/identity/):
//!  - verify_identity_balance_for_identity_id/v0/mod.rs
//!  - verify_identity_nonce/v0/mod.rs
//!  - verify_identity_keys_by_identity_id/v0/mod.rs
//!  - verify_full_identity_by_identity_id/v0/mod.rs
//!  - verify_identity_id_by_unique_public_key_hash/v0/mod.rs
namespace platform::drive {

using platform::Identifier;
using platform::Identity;
using platform::IdentityPublicKey;
using platform::merk::Hash256;

//! getIdentityBalance: proof over balance_path [[96]] keyed by identity id.
//! `balance_out` is std::nullopt when the identity has no balance entry.
bool VerifyIdentityBalance(Span<const uint8_t> proof, const Identifier& identity_id,
                           std::optional<uint64_t>& balance_out, Hash256& root_out,
                           std::string& error);

//! getIdentity revision: proof over [[32], id] keyed by [192].
bool VerifyIdentityRevision(Span<const uint8_t> proof, const Identifier& identity_id,
                            std::optional<uint64_t>& revision_out, Hash256& root_out,
                            std::string& error);

//! getIdentityNonce: proof over [[32], id] keyed by [64].
bool VerifyIdentityNonce(Span<const uint8_t> proof, const Identifier& identity_id,
                         std::optional<uint64_t>& nonce_out, Hash256& root_out, std::string& error);
bool VerifyIdentityContractNonce(Span<const uint8_t> proof, const Identifier& identity_id,
                                 const Identifier& contract_id, std::optional<uint64_t>& nonce_out,
                                 Hash256& root_out, std::string& error);

//! getIdentityKeys (all keys): proof over the identity key tree
//! [[32], id, [128]] (range-full). Each leaf is a platform-serialized
//! IdentityPublicKey. `keys_out` is std::nullopt only if the key tree itself
//! is absent; an existing-but-empty key tree yields an empty vector.
bool VerifyIdentityKeys(Span<const uint8_t> proof, const Identifier& identity_id,
                        std::optional<std::vector<IdentityPublicKey>>& keys_out, Hash256& root_out,
                        std::string& error);

//! getIdentityByPublicKeyHash (id only): proof over
//! UniquePublicKeyHashesToIdentities [[24]] keyed by the 20-byte hash. The
//! leaf is the 32-byte identity id. `identity_out` is std::nullopt when the
//! hash is proven absent (no identity registered that key hash).
bool VerifyIdentityIdByPublicKeyHash(Span<const uint8_t> proof,
                                     const std::array<uint8_t, 20>& public_key_hash,
                                     std::optional<Identifier>& identity_out, Hash256& root_out,
                                     std::string& error);

//! getIdentity (full): the GUI issues three simple proofs — balance, revision,
//! and all-keys — each independently proof-verified, then assembles the
//! identity. This avoids replicating Drive's PathQuery::merge while proving
//! every field. All three proofs must commit to the same GroveDB root.
//! `identity_out` is std::nullopt when the identity is proven fully absent
//! (no balance, no revision, no keys).
bool VerifyFullIdentity(Span<const uint8_t> balance_proof, Span<const uint8_t> revision_proof,
                        Span<const uint8_t> keys_proof, const Identifier& identity_id,
                        std::optional<Identity>& identity_out, Hash256& root_out,
                        std::string& error);

//! Proof-backed document query shapes used by the DashPay GUI. These mirror
//! DriveDocumentQuery::construct_path_query for the pinned DPNS/DashPay v1
//! system-contract indexes and return the serialized document items resolved
//! through GroveDB index references.
bool VerifyDpnsNameExact(Span<const uint8_t> proof, const std::string& normalized_label,
                         std::vector<Bytes>& documents_out, Hash256& root_out, std::string& error);
bool VerifyDpnsNamePrefix(Span<const uint8_t> proof, const std::string& normalized_prefix,
                          uint16_t limit, std::vector<Bytes>& documents_out, Hash256& root_out,
                          std::string& error);
bool VerifyDpnsNamesByIdentity(Span<const uint8_t> proof, const Identifier& identity_id,
                               uint16_t limit, std::vector<Bytes>& documents_out,
                               Hash256& root_out, std::string& error);
bool VerifyDashPayProfileByOwner(Span<const uint8_t> proof, const Identifier& owner_id,
                                 std::vector<Bytes>& documents_out, Hash256& root_out,
                                 std::string& error);
bool VerifyDashPayContactRequests(Span<const uint8_t> proof, const Identifier& identity_id,
                                  bool to_identity, uint16_t limit,
                                  std::vector<Bytes>& documents_out, Hash256& root_out,
                                  std::string& error);

} // namespace platform::drive

#endif // BITCOIN_PLATFORM_DRIVE_QUERIES_H
