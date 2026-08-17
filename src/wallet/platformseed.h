// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_PLATFORMSEED_H
#define BITCOIN_WALLET_PLATFORMSEED_H

#include <support/allocators/secure.h>

namespace wallet {
class CWallet;

namespace platformkeys {

//! Platform data record key pinning the SeedFingerprint of the seed the
//! wallet's platform data was created from. Losing or damaging this record
//! must be treated as wallet corruption: silently dropping it could unpin
//! the Platform seed and let a multi-seed wallet sign under another identity.
inline constexpr char SEED_ID_RECORD[]{"platform/seed-id"};

//! Fetch the BIP39 seed the platform key provider derives from (requires the
//! wallet to be unlocked; fails for watch-only wallets and wallets without a
//! recoverable mnemonic).
//!
//! A descriptor wallet can hold active spk_mans with different mnemonics, so
//! candidates are selected deterministically: the wallet's "platform/seed-id"
//! record (the SeedFingerprint of the seed its platform data was created
//! from) takes precedence, otherwise the candidate from the spk_man with the
//! lowest ID wins. Legacy wallets use their HD chain seed.
bool GetPlatformSeed(const CWallet& wallet, SecureVector& seed_out);

} // namespace platformkeys
} // namespace wallet

#endif // BITCOIN_WALLET_PLATFORMSEED_H
