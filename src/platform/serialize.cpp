// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/serialize.h>

namespace platform {

void WriteLEB128(Bytes& out, uint64_t value)
{
    while (true) {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
        if (value == 0) return;
    }
}

} // namespace platform
