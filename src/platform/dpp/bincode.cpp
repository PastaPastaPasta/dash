// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/bincode.h>

#include <tinyformat.h>

namespace platform::dpp {

namespace {
constexpr uint8_t VARINT_U16{0xfb}; // 251
constexpr uint8_t VARINT_U32{0xfc}; // 252
constexpr uint8_t VARINT_U64{0xfd}; // 253
constexpr uint8_t VARINT_U128{0xfe}; // 254 (unused by DPP; rejected)

//! Zigzag encoding as used by bincode-v2 for signed varints.
uint64_t ZigZag(int64_t value)
{
    return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}

int64_t UnZigZag(uint64_t value)
{
    return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}
} // namespace

void Writer::WriteVarint(uint64_t value)
{
    if (value < VARINT_U16) {
        m_data.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        m_data.push_back(VARINT_U16);
        m_data.push_back(static_cast<uint8_t>(value >> 8));
        m_data.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffffffff) {
        m_data.push_back(VARINT_U32);
        for (int shift = 24; shift >= 0; shift -= 8) {
            m_data.push_back(static_cast<uint8_t>(value >> shift));
        }
    } else {
        m_data.push_back(VARINT_U64);
        for (int shift = 56; shift >= 0; shift -= 8) {
            m_data.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
}

void Writer::WriteSignedVarint(int64_t value)
{
    WriteVarint(ZigZag(value));
}

void Writer::WriteByteVec(Span<const uint8_t> bytes)
{
    WriteVarint(bytes.size());
    WriteBytes(bytes);
}

void Writer::WriteString(const std::string& str)
{
    WriteVarint(str.size());
    m_data.insert(m_data.end(), str.begin(), str.end());
}

bool Reader::SetError(const std::string& error)
{
    if (m_error.empty()) m_error = error;
    return false;
}

bool Reader::ReadU8(uint8_t& out)
{
    if (HasError()) return false;
    if (Remaining() < 1) return SetError("bincode: unexpected end of data");
    out = m_data[m_pos++];
    return true;
}

bool Reader::ReadInto(Span<uint8_t> out)
{
    if (HasError()) return false;
    if (Remaining() < out.size()) return SetError("bincode: unexpected end of data");
    std::copy(m_data.begin() + m_pos, m_data.begin() + m_pos + out.size(), out.begin());
    m_pos += out.size();
    return true;
}

bool Reader::ReadBool(bool& out)
{
    uint8_t byte;
    if (!ReadU8(byte)) return false;
    if (byte > 1) return SetError(strprintf("bincode: invalid bool byte 0x%02x", byte));
    out = byte == 1;
    return true;
}

bool Reader::ReadVarint(uint64_t& out)
{
    uint8_t first;
    if (!ReadU8(first)) return false;
    if (first < VARINT_U16) {
        out = first;
        return true;
    }
    int width{0};
    switch (first) {
    case VARINT_U16: width = 2; break;
    case VARINT_U32: width = 4; break;
    case VARINT_U64: width = 8; break;
    case VARINT_U128:
    default:
        return SetError(strprintf("bincode: unsupported varint marker 0x%02x", first));
    }
    if (Remaining() < static_cast<size_t>(width)) return SetError("bincode: truncated varint");
    uint64_t value{0};
    for (int i = 0; i < width; ++i) {
        value = (value << 8) | m_data[m_pos++]; // big-endian
    }
    out = value;
    return true;
}

bool Reader::ReadSignedVarint(int64_t& out)
{
    uint64_t raw;
    if (!ReadVarint(raw)) return false;
    out = UnZigZag(raw);
    return true;
}

bool Reader::ReadByteVec(Bytes& out, size_t max_len)
{
    uint64_t len;
    if (!ReadVarint(len)) return false;
    if (len > max_len) return SetError(strprintf("bincode: byte vector length %d exceeds limit %d", len, max_len));
    if (Remaining() < len) return SetError("bincode: truncated byte vector");
    out.assign(m_data.begin() + m_pos, m_data.begin() + m_pos + len);
    m_pos += len;
    return true;
}

bool Reader::ReadString(std::string& out, size_t max_len)
{
    uint64_t len;
    if (!ReadVarint(len)) return false;
    if (len > max_len) return SetError(strprintf("bincode: string length %d exceeds limit %d", len, max_len));
    if (Remaining() < len) return SetError("bincode: truncated string");
    out.assign(m_data.begin() + m_pos, m_data.begin() + m_pos + len);
    m_pos += len;
    return true;
}

bool Reader::ReadOptionTag(bool& has_value)
{
    uint8_t byte;
    if (!ReadU8(byte)) return false;
    if (byte > 1) return SetError(strprintf("bincode: invalid Option tag 0x%02x", byte));
    has_value = byte == 1;
    return true;
}

} // namespace platform::dpp
