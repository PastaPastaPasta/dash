// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_MARSHAL_H
#define BITCOIN_PLATFORM_MARSHAL_H

#include <platform/types.h>

#include <dash/sdk/ffi.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

class uint256;

//! Conversions between dash-sdk-cxx's bridge structs and platform:: types.
namespace platform::marshal {

Result<Identity> FromFfi(const dash_sdk::IdentityResult& result);
Result<Identifier> FromFfi(const dash_sdk::IdResult& result);
Result<Names> FromFfi(const dash_sdk::NamesResult& result);
Result<std::vector<Contest>> FromFfi(const dash_sdk::ContestsResult& result);
//! An outcome or state the binding adds later settles nothing: it is
//! UNAVAILABLE, or an INTERNAL status, never OK.
FundingResult<Identity> FromFfi(const dash_sdk::RegisterIdentityResult& result);
FundingResult<uint64_t> FromFfi(const dash_sdk::TopUpIdentityResult& result);
Result<AssetLockState> FromFfi(const dash_sdk::AssetLockStateResult& result);
//! The page the binding answered for since_ms. An incomplete page that does
//! not move the cursor past since_ms would be read again forever, so it is
//! UNAVAILABLE.
Result<ContactRequestPage> FromFfi(const dash_sdk::ContactRequestsResult& result, uint64_t since_ms);

std::array<uint8_t, 32> ToFfi(const uint256& hash);
std::vector<dash_sdk::IdentityKey> ToFfi(const std::vector<IdentityPublicKey>& keys);
dash_sdk::AssetLockProof ToFfi(const AssetLockProof& proof);
dash_sdk::ContactRequestInput ToFfi(const ContactRequestInput& input);
dash_sdk::ContestPolicy ToFfi(const ContestPolicy& policy);
dash_sdk::Proxy ToFfi(const std::optional<ProxyConfig>& proxy);

} // namespace platform::marshal

#endif // BITCOIN_PLATFORM_MARSHAL_H
