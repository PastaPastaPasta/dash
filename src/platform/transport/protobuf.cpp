// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/transport/protobuf.h>

namespace platform::pb {

void Writer::WriteVarint(uint64_t value)
{
    while (value >= 0x80) {
        m_out.push_back(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    m_out.push_back(static_cast<uint8_t>(value));
}

void Writer::WriteTag(uint32_t field, WireType wt)
{
    WriteVarint((static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(wt));
}

void Writer::Varint(uint32_t field, uint64_t value)
{
    WriteTag(field, WireType::Varint);
    WriteVarint(value);
}

void Writer::Bytes(uint32_t field, Span<const uint8_t> value)
{
    WriteTag(field, WireType::Len);
    WriteVarint(value.size());
    m_out.insert(m_out.end(), value.begin(), value.end());
}

void Writer::Str(uint32_t field, const std::string& value)
{
    WriteTag(field, WireType::Len);
    WriteVarint(value.size());
    m_out.insert(m_out.end(), value.begin(), value.end());
}

bool Reader::ReadVarint(uint64_t& out)
{
    out = 0;
    int shift = 0;
    while (m_pos < m_data.size()) {
        const uint8_t b = m_data[m_pos++];
        if (shift < 64) out |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift > 70) break;
    }
    m_failed = true;
    return false;
}

bool Reader::Next(Field& out)
{
    if (m_failed || m_pos >= m_data.size()) return false;
    uint64_t tag{0};
    if (!ReadVarint(tag)) return false;
    out.number = static_cast<uint32_t>(tag >> 3);
    out.type = static_cast<WireType>(tag & 0x7);
    switch (out.type) {
    case WireType::Varint:
        return ReadVarint(out.varint);
    case WireType::I64: {
        // m_pos <= m_data.size() always, so size()-m_pos never underflows;
        // the subtraction form also avoids any m_pos + N overflow.
        if (m_data.size() - m_pos < 8) { m_failed = true; return false; }
        out.varint = 0;
        for (int i = 0; i < 8; ++i) out.varint |= static_cast<uint64_t>(m_data[m_pos++]) << (8 * i);
        return true;
    }
    case WireType::I32: {
        if (m_data.size() - m_pos < 4) { m_failed = true; return false; }
        out.varint = 0;
        for (int i = 0; i < 4; ++i) out.varint |= static_cast<uint64_t>(m_data[m_pos++]) << (8 * i);
        return true;
    }
    case WireType::Len: {
        uint64_t len{0};
        if (!ReadVarint(len)) return false;
        // len is attacker-controlled and up to 2^64-1; compare against the
        // remaining byte count without ever computing m_pos + len (which
        // would wrap and yield an out-of-bounds subspan).
        if (len > m_data.size() - m_pos) { m_failed = true; return false; }
        out.bytes = m_data.subspan(m_pos, static_cast<size_t>(len));
        m_pos += static_cast<size_t>(len);
        return true;
    }
    default:
        m_failed = true;
        return false;
    }
}

std::optional<Span<const uint8_t>> GetLenField(Span<const uint8_t> msg, uint32_t field)
{
    Reader r{msg};
    Field f;
    while (r.Next(f)) {
        if (f.number == field && f.type == WireType::Len) return f.bytes;
    }
    return std::nullopt;
}

std::optional<uint64_t> GetVarintField(Span<const uint8_t> msg, uint32_t field)
{
    Reader r{msg};
    Field f;
    while (r.Next(f)) {
        if (f.number == field && f.type == WireType::Varint) return f.varint;
    }
    return std::nullopt;
}

} // namespace platform::pb
