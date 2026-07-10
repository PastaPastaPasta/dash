// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/serialize.h>

#include <tinyformat.h>

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

bool BytesReader::SetError(const std::string& error)
{
    if (m_error.empty()) m_error = error;
    return false;
}

bool BytesReader::ReadBytes(size_t len, Bytes& out)
{
    if (HasError()) return false;
    if (Remaining() < len) return SetError(strprintf("unexpected end of data: wanted %u bytes, have %u", len, Remaining()));
    out.assign(m_data.begin() + m_pos, m_data.begin() + m_pos + len);
    m_pos += len;
    return true;
}

bool BytesReader::ReadInto(Span<uint8_t> out)
{
    if (HasError()) return false;
    if (Remaining() < out.size()) return SetError(strprintf("unexpected end of data: wanted %u bytes, have %u", out.size(), Remaining()));
    std::copy(m_data.begin() + m_pos, m_data.begin() + m_pos + out.size(), out.begin());
    m_pos += out.size();
    return true;
}

bool BytesReader::ReadU8(uint8_t& out)
{
    if (HasError()) return false;
    if (Remaining() < 1) return SetError("unexpected end of data reading u8");
    out = m_data[m_pos++];
    return true;
}

bool BytesReader::ReadU16BE(uint16_t& out)
{
    if (HasError()) return false;
    if (Remaining() < 2) return SetError("unexpected end of data reading u16");
    out = (uint16_t{m_data[m_pos]} << 8) | uint16_t{m_data[m_pos + 1]};
    m_pos += 2;
    return true;
}

bool BytesReader::ReadU32BE(uint32_t& out)
{
    if (HasError()) return false;
    if (Remaining() < 4) return SetError("unexpected end of data reading u32");
    out = 0;
    for (int i = 0; i < 4; ++i) {
        out = (out << 8) | m_data[m_pos + i];
    }
    m_pos += 4;
    return true;
}

bool BytesReader::ReadU64BE(uint64_t& out)
{
    if (HasError()) return false;
    if (Remaining() < 8) return SetError("unexpected end of data reading u64");
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | m_data[m_pos + i];
    }
    m_pos += 8;
    return true;
}

bool BytesReader::ReadI64BE(int64_t& out)
{
    uint64_t value;
    if (!ReadU64BE(value)) return false;
    out = static_cast<int64_t>(value);
    return true;
}

bool BytesReader::ReadLEB128(uint64_t& out)
{
    if (HasError()) return false;
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        uint8_t byte;
        if (!ReadU8(byte)) return false;
        if (shift == 63 && (byte & 0x7e) != 0) return SetError("LEB128 varint overflows 64 bits");
        out |= uint64_t{static_cast<uint8_t>(byte & 0x7f)} << shift;
        if ((byte & 0x80) == 0) return true;
    }
    return SetError("LEB128 varint too long");
}

bool BytesReader::ReadLEB128Signed(int64_t& out)
{
    uint64_t zigzag;
    if (!ReadLEB128(zigzag)) return false;
    out = static_cast<int64_t>((zigzag >> 1) ^ (~(zigzag & 1) + 1));
    return true;
}

bool BytesReader::ReadBincodeVarint(uint64_t& out)
{
    if (HasError()) return false;
    uint8_t first;
    if (!ReadU8(first)) return false;
    if (first < 251) {
        out = first;
        return true;
    }
    switch (first) {
    case 251: {
        uint16_t v;
        if (!ReadU16BE(v)) return false;
        out = v;
        return true;
    }
    case 252: {
        uint32_t v;
        if (!ReadU32BE(v)) return false;
        out = v;
        return true;
    }
    case 253:
        return ReadU64BE(out);
    default:
        return SetError(strprintf("unsupported bincode varint marker %u", first));
    }
}

bool BytesReader::ReadBincodeVarintSigned(int64_t& out)
{
    uint64_t zigzag;
    if (!ReadBincodeVarint(zigzag)) return false;
    out = static_cast<int64_t>((zigzag >> 1) ^ (~(zigzag & 1) + 1));
    return true;
}

bool BytesReader::ReadBincodeOptionTag(bool& has_value)
{
    uint8_t tag;
    if (!ReadU8(tag)) return false;
    if (tag > 1) return SetError(strprintf("invalid bincode Option tag %u", tag));
    has_value = tag == 1;
    return true;
}

bool BytesReader::ReadBincodeByteVec(Bytes& out, size_t max_len)
{
    uint64_t len;
    if (!ReadBincodeVarint(len)) return false;
    if (len > max_len) return SetError(strprintf("byte vector length %u exceeds limit %u", len, max_len));
    if (len > Remaining()) return SetError(strprintf("byte vector length %u exceeds remaining data %u", len, Remaining()));
    return ReadBytes(static_cast<size_t>(len), out);
}

} // namespace platform
