// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_SERIALIZE_H
#define BITCOIN_PLATFORM_SERIALIZE_H

#include <span.h>

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

using Bytes = std::vector<uint8_t>;

//! Appends an unsigned LEB128 varint (integer-encoding crate compatible) to
//! out. Used inside blake3 hash preimages, mirroring
//! grovedb merk/src/tree/hash.rs (integer_encoding::VarInt::encode_var).
void WriteLEB128(Bytes& out, uint64_t value);

//! Safe forward-only cursor over a byte span. All Read* methods return false
//! on truncation or malformed input and record a human readable error; once
//! an error is set every further read fails. No exceptions are thrown.
class BytesReader
{
public:
    explicit BytesReader(Span<const uint8_t> data) : m_data(data) {}

    size_t Remaining() const { return m_data.size() - m_pos; }
    bool IsEof() const { return m_pos == m_data.size(); }
    bool HasError() const { return !m_error.empty(); }
    const std::string& GetError() const { return m_error; }
    //! Records an error (first error wins) and returns false.
    bool SetError(const std::string& error);

    //! Reads len raw bytes into out (overwriting it).
    bool ReadBytes(size_t len, Bytes& out);
    //! Reads exactly out.size() bytes into a fixed-size buffer.
    bool ReadInto(Span<uint8_t> out);
    bool ReadU8(uint8_t& out);
    //! Fixed-width big-endian reads ("ed" crate style, used by merk proof ops).
    bool ReadU16BE(uint16_t& out);
    bool ReadU32BE(uint32_t& out);
    bool ReadU64BE(uint64_t& out);
    bool ReadI64BE(int64_t& out);

    //! Unsigned LEB128 varint (integer-encoding crate). Rejects encodings
    //! longer than 10 bytes or overflowing 64 bits.
    bool ReadLEB128(uint64_t& out);
    //! Signed LEB128 varint with zigzag (integer-encoding crate signed ints).
    bool ReadLEB128Signed(int64_t& out);

    //! bincode-v2 varint in the big-endian configuration
    //! (bincode::config::standard().with_big_endian()): values < 251 are a
    //! single byte; marker 251 is followed by a big-endian u16, 252 by a
    //! big-endian u32, 253 by a big-endian u64 (marker 254/u128 rejected).
    //! Used for collection lengths and enum discriminants in grovedb
    //! envelopes and elements.
    bool ReadBincodeVarint(uint64_t& out);
    //! bincode-v2 signed varint: zigzag then ReadBincodeVarint.
    bool ReadBincodeVarintSigned(int64_t& out);
    //! bincode Option tag: one byte, 0 (nullopt) or 1 (value follows).
    bool ReadBincodeOptionTag(bool& has_value);
    //! bincode Vec<u8>: varint length + raw bytes. max_len guards allocation.
    bool ReadBincodeByteVec(Bytes& out, size_t max_len);

private:
    Span<const uint8_t> m_data;
    size_t m_pos{0};
    std::string m_error;
};

} // namespace platform

#endif // BITCOIN_PLATFORM_SERIALIZE_H
