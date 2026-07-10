// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TRANSPORT_PROTOBUF_H
#define BITCOIN_PLATFORM_TRANSPORT_PROTOBUF_H

#include <cstdint>
#include <optional>
#include <span.h>
#include <string>
#include <vector>

/**
 * Minimal protobuf wire-format reader/writer — just enough to (de)serialize
 * the handful of DAPI Platform messages the GUI uses. Only the wire types we
 * need are supported: varint (0), 64-bit (1), length-delimited (2), 32-bit
 * (5). Field numbers/types come from
 * dashpay/platform packages/dapi-grpc/protos/platform/v0/platform.proto.
 */
namespace platform::pb {

enum class WireType : uint8_t { Varint = 0, I64 = 1, Len = 2, I32 = 5 };

class Writer
{
public:
    void Varint(uint32_t field, uint64_t value);
    void Bytes(uint32_t field, Span<const uint8_t> value);
    void Bytes(uint32_t field, const std::vector<uint8_t>& value) { Bytes(field, Span<const uint8_t>{value}); }
    void Str(uint32_t field, const std::string& value);
    void Bool(uint32_t field, bool value) { if (value) Varint(field, 1); }
    //! Write a nested message (its already-serialized bytes) as a length-
    //! delimited field.
    void Message(uint32_t field, const std::vector<uint8_t>& msg) { Bytes(field, msg); }

    const std::vector<uint8_t>& data() const { return m_out; }
    std::vector<uint8_t> take() { return std::move(m_out); }

private:
    void WriteTag(uint32_t field, WireType wt);
    void WriteVarint(uint64_t value);
    std::vector<uint8_t> m_out;
};

//! One decoded field: tag + the raw payload interpreted lazily.
struct Field {
    uint32_t number{0};
    WireType type{WireType::Varint};
    uint64_t varint{0};                 //!< for Varint/I64/I32
    Span<const uint8_t> bytes;          //!< for Len
};

class Reader
{
public:
    explicit Reader(Span<const uint8_t> data) : m_data(data) {}

    //! Read the next field. Returns false at end of input or on malformed
    //! data (check failed()).
    bool Next(Field& out);
    bool failed() const { return m_failed; }
    bool eof() const { return m_pos >= m_data.size(); }

private:
    bool ReadVarint(uint64_t& out);
    Span<const uint8_t> m_data;
    size_t m_pos{0};
    bool m_failed{false};
};

//! Convenience: extract the length-delimited payload of a single field number
//! from a message (first occurrence), following an optional oneof-version
//! wrapper. Returns nullopt if absent/malformed.
std::optional<Span<const uint8_t>> GetLenField(Span<const uint8_t> msg, uint32_t field);
std::optional<uint64_t> GetVarintField(Span<const uint8_t> msg, uint32_t field);

} // namespace platform::pb

#endif // BITCOIN_PLATFORM_TRANSPORT_PROTOBUF_H
