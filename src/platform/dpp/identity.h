// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DPP_IDENTITY_H
#define BITCOIN_PLATFORM_DPP_IDENTITY_H

#include <platform/types.h>
#include <span.h>

#include <optional>
#include <string>

/**
 * Decoding of platform-serialized Identity / IdentityPublicKey objects
 * (bincode standard()+big_endian(), see platform/dpp/bincode.h) into the
 * platform::Identity / platform::IdentityPublicKey GUI types.
 *
 * Wire layout mirrors dashpay/platform (tag v4.0.0):
 *  - Identity: rs-dpp/src/identity/identity.rs — enum { V0 } with
 *    #[platform_serialize(limit = 15000, unversioned)]; IdentityV0 is
 *    { id: Identifier, public_keys: BTreeMap<KeyID, IdentityPublicKey>,
 *      balance: u64, revision: u64 } (rs-dpp/src/identity/v0/mod.rs).
 *  - IdentityPublicKey: rs-dpp/src/identity/identity_public_key/mod.rs —
 *    enum { V0 } with #[platform_serialize(limit = 2000, unversioned)];
 *    IdentityPublicKeyV0 is { id, purpose, security_level,
 *      contract_bounds: Option<ContractBounds>, key_type, read_only,
 *      data: BinaryData, disabled_at: Option<u64> }
 *    (rs-dpp/src/identity/identity_public_key/v0/mod.rs).
 */
namespace platform::dpp {

//! Decodes a platform-serialized Identity. Returns std::nullopt and sets
//! error on malformed input (including trailing bytes).
std::optional<platform::Identity> DecodeIdentity(Span<const uint8_t> bytes, std::string& error);

//! Decodes a standalone platform-serialized IdentityPublicKey.
std::optional<platform::IdentityPublicKey> DecodeIdentityPublicKey(Span<const uint8_t> bytes,
                                                                   std::string& error);

} // namespace platform::dpp

#endif // BITCOIN_PLATFORM_DPP_IDENTITY_H
