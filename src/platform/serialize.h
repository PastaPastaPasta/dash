// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_SERIALIZE_H
#define BITCOIN_PLATFORM_SERIALIZE_H

#include <cstdint>
#include <vector>

namespace platform {

using Bytes = std::vector<uint8_t>;

//! Appends an unsigned LEB128 varint (protobuf wire varint) to out. Used by
//! the Tenderdash StateId protobuf sign bytes (drive/quorumsig.cpp).
void WriteLEB128(Bytes& out, uint64_t value);

} // namespace platform

#endif // BITCOIN_PLATFORM_SERIALIZE_H
