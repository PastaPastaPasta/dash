// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/document.h>

#include <hash.h>

#include <cassert>

namespace platform::dpp {

Value Value::U32(uint32_t value)
{
    Value ret;
    ret.m_kind = ValueKind::U32;
    ret.m_int = value;
    return ret;
}

Value Value::U64(uint64_t value)
{
    Value ret;
    ret.m_kind = ValueKind::U64;
    ret.m_int = value;
    return ret;
}

Value Value::MakeBytes(Bytes bytes)
{
    Value ret;
    ret.m_kind = ValueKind::BYTES;
    ret.m_bytes = std::move(bytes);
    return ret;
}

Value Value::MakeBytes32(const std::array<uint8_t, 32>& bytes)
{
    Value ret;
    ret.m_kind = ValueKind::BYTES32;
    ret.m_bytes.assign(bytes.begin(), bytes.end());
    return ret;
}

Value Value::Id(const Identifier& id)
{
    Value ret;
    ret.m_kind = ValueKind::IDENTIFIER;
    ret.m_bytes.assign(id.begin(), id.end());
    return ret;
}

Value Value::Text(std::string text)
{
    Value ret;
    ret.m_kind = ValueKind::TEXT;
    ret.m_text = std::move(text);
    return ret;
}

Value Value::Boolean(bool value)
{
    Value ret;
    ret.m_kind = ValueKind::BOOL;
    ret.m_bool = value;
    return ret;
}

Value Value::MakeMap(Map entries)
{
    Value ret;
    ret.m_kind = ValueKind::MAP;
    ret.m_map = std::make_shared<Map>(std::move(entries));
    return ret;
}

void Value::Encode(Writer& writer) const
{
    writer.WriteVarint(static_cast<uint64_t>(m_kind));
    switch (m_kind) {
    case ValueKind::U32:
    case ValueKind::U64:
        writer.WriteVarint(m_int);
        break;
    case ValueKind::BYTES:
        writer.WriteByteVec(m_bytes);
        break;
    case ValueKind::BYTES32:
    case ValueKind::IDENTIFIER:
        // Fixed-size arrays carry no length prefix.
        assert(m_bytes.size() == 32);
        writer.WriteBytes(m_bytes);
        break;
    case ValueKind::TEXT:
        writer.WriteString(m_text);
        break;
    case ValueKind::BOOL:
        writer.WriteBool(m_bool);
        break;
    case ValueKind::MAP:
        assert(m_map);
        writer.WriteVarint(m_map->size());
        for (const auto& [key, value] : *m_map) {
            key.Encode(writer);
            value.Encode(writer);
        }
        break;
    default:
        assert(false); // unsupported Value kind
    }
}

void EncodeDocumentData(Writer& writer, const DocumentData& data)
{
    writer.WriteVarint(data.size());
    for (const auto& [key, value] : data) {
        writer.WriteString(key);
        value.Encode(writer);
    }
}

Identifier GenerateDocumentId(const Identifier& contract_id,
                              const Identifier& owner_id,
                              const std::string& document_type_name,
                              Span<const uint8_t> entropy)
{
    CHash256 hasher;
    hasher.Write(contract_id);
    hasher.Write(owner_id);
    hasher.Write(MakeUCharSpan(document_type_name));
    hasher.Write(entropy);
    Identifier ret;
    uint256 hash;
    hasher.Finalize(hash);
    std::copy(hash.begin(), hash.end(), ret.begin());
    return ret;
}

} // namespace platform::dpp
