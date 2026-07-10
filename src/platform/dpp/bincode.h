// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DPP_BINCODE_H
#define BITCOIN_PLATFORM_DPP_BINCODE_H

#include <span.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * bincode-v2 codec in the configuration used by rs-platform-serialization
 * for every DPP object: bincode::config::standard().with_big_endian(), i.e.
 * variable-width integers with BIG-endian payloads.
 *
 * Source: dashpay/platform (tag v4.0.0)
 * packages/rs-platform-serialization-derive/src/{serialize,deserialize}_*.rs —
 * every derived PlatformSerialize/PlatformDeserialize impl builds
 * `bincode::config::standard().with_big_endian()`. Note this matches the
 * GroveDB proof envelope config; bincode's default little-endian standard()
 * config is NOT used anywhere on the DPP wire (validated against the
 * rs-dpp-generated vectors in src/test/data/platform/).
 *
 * Varint scheme (bincode "varint" encoding):
 *   value < 251           -> 1 byte
 *   value <= 0xffff       -> 0xfb marker + u16 big-endian
 *   value <= 0xffffffff   -> 0xfc marker + u32 big-endian
 *   otherwise             -> 0xfd marker + u64 big-endian
 *   (0xfe marker introduces u128, which DPP never uses; rejected on read)
 * Signed integers are zigzag-encoded and then written as unsigned varints.
 * Fixed-size arrays are raw bytes without a length prefix; Vec<u8>/String
 * carry a varint length; Option is one byte (0/1); enum discriminants are
 * the varint variant index in declaration order.
 */
namespace platform::dpp {

using Bytes = std::vector<uint8_t>;

class Writer
{
public:
    void WriteU8(uint8_t byte) { m_data.push_back(byte); }
    void WriteBytes(Span<const uint8_t> bytes) { m_data.insert(m_data.end(), bytes.begin(), bytes.end()); }
    void WriteBool(bool value) { m_data.push_back(value ? 1 : 0); }
    //! Unsigned bincode varint (big-endian payloads).
    void WriteVarint(uint64_t value);
    //! Signed bincode varint: zigzag, then WriteVarint.
    void WriteSignedVarint(int64_t value);
    //! Vec<u8> / BinaryData: varint length + raw bytes.
    void WriteByteVec(Span<const uint8_t> bytes);
    //! String: varint byte length + UTF-8 bytes.
    void WriteString(const std::string& str);
    //! Option tag: 1 byte, 0 (None) or 1 (Some; value follows).
    void WriteOptionTag(bool has_value) { m_data.push_back(has_value ? 1 : 0); }

    const Bytes& Data() const { return m_data; }
    Bytes Take() { return std::move(m_data); }

private:
    Bytes m_data;
};

//! Forward-only reader; all Read* methods return false on truncation or
//! malformed input and latch the first error. No exceptions.
class Reader
{
public:
    explicit Reader(Span<const uint8_t> data) : m_data(data) {}

    size_t Remaining() const { return m_data.size() - m_pos; }
    bool IsEof() const { return m_pos == m_data.size(); }
    bool HasError() const { return !m_error.empty(); }
    const std::string& GetError() const { return m_error; }
    //! Records an error (first error wins) and returns false.
    bool SetError(const std::string& error);

    bool ReadU8(uint8_t& out);
    //! Reads exactly out.size() raw bytes (fixed-size array).
    bool ReadInto(Span<uint8_t> out);
    bool ReadBool(bool& out);
    bool ReadVarint(uint64_t& out);
    bool ReadSignedVarint(int64_t& out);
    //! Vec<u8>: varint length (bounded by max_len) + raw bytes.
    bool ReadByteVec(Bytes& out, size_t max_len);
    //! String: varint length (bounded by max_len) + UTF-8 bytes.
    bool ReadString(std::string& out, size_t max_len);
    //! Option tag: exactly 0 or 1.
    bool ReadOptionTag(bool& has_value);

private:
    Span<const uint8_t> m_data;
    size_t m_pos{0};
    std::string m_error;
};

} // namespace platform::dpp

#endif // BITCOIN_PLATFORM_DPP_BINCODE_H
