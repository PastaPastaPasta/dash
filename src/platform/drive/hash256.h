// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DRIVE_HASH256_H
#define BITCOIN_PLATFORM_DRIVE_HASH256_H

#include <array>
#include <cstdint>

namespace platform::merk {

//! A 32-byte GroveDB/merk root hash. Kept in the platform::merk namespace it
//! historically lived in (platform/proof/merk.h) so the drive-layer
//! signatures did not change when proof verification moved to the Rust
//! bridge (rust/platform).
using Hash256 = std::array<uint8_t, 32>;

} // namespace platform::merk

#endif // BITCOIN_PLATFORM_DRIVE_HASH256_H
