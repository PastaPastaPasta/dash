// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/document.h>

#include <hash.h>

#include <cassert>
#include <cctype>

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

// -----------------------------------------------------------------------------
// Stored-document decoders.
//
// TARGETED decoders over the known DPNS/DashPay v1 property order, not a
// general contract-schema-driven deserializer. The stored document format
// (rs-dpp DocumentV0::serialize) is [version(1)] [$id(32)] [$ownerId(32)]
// [revision] then the document type's properties in the contract's
// serialization order, with fixed-width 8-byte big-endian timestamps and
// single-byte length-prefixed strings/bytes. We extract the fields the GUI
// needs and validate structurally; a full schema-driven decode is a follow-on.
// -----------------------------------------------------------------------------

size_t DecodeDocumentHeader(Span<const uint8_t> doc, Identifier& id_out, Identifier& owner_out, uint64_t& revision_out)
{
    if (doc.size() < 66) return 0;
    size_t p = 1; // version marker
    std::copy(doc.begin() + p, doc.begin() + p + 32, id_out.begin()); p += 32;
    std::copy(doc.begin() + p, doc.begin() + p + 32, owner_out.begin()); p += 32;
    revision_out = doc[p]; p += 1; // revision (single byte for these low-revision docs)
    return p;
}

bool DecodeDpnsDomain(Span<const uint8_t> doc, DpnsName& out)
{
    Identifier id, owner;
    uint64_t rev;
    const size_t hdr = DecodeDocumentHeader(doc, id, owner, rev);
    if (hdr == 0) return false;
    out.document_id = id;

    auto is_label_char = [](uint8_t ch) {
        ch = static_cast<uint8_t>(std::tolower(ch));
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    };
    std::string label, normalized;
    for (size_t scan = hdr; scan + 1 < doc.size(); ++scan) {
        const uint8_t len = doc[scan];
        if (len < 1 || len > 63 || scan + 1 + len > doc.size()) continue;
        bool all_label = true;
        for (size_t k = 0; k < len; ++k) { if (!is_label_char(doc[scan + 1 + k])) { all_label = false; break; } }
        if (!all_label) continue;
        label.assign(doc.begin() + scan + 1, doc.begin() + scan + 1 + len);
        const size_t nxt = scan + 1 + len;
        if (nxt < doc.size()) {
            const uint8_t nlen = doc[nxt];
            if (nlen >= 1 && nlen <= 63 && nxt + 1 + nlen <= doc.size()) {
                normalized.assign(doc.begin() + nxt + 1, doc.begin() + nxt + 1 + nlen);
            }
        }
        break;
    }
    if (label.empty()) return false;
    out.label = label;
    out.normalized_label = normalized.empty() ? label : normalized;
    out.parent_domain = "dash";

    // records.identity: the last 32-byte identifier before the trailing 2-byte
    // subdomainRules marker; fall back to the owner if absent.
    if (doc.size() >= 34) {
        std::copy(doc.end() - 34, doc.end() - 2, out.identity.begin());
    } else {
        out.identity = owner;
    }
    return true;
}

bool DecodeDashPayProfile(Span<const uint8_t> doc, Profile& out)
{
    Identifier id, owner;
    uint64_t rev;
    if (DecodeDocumentHeader(doc, id, owner, rev) == 0) return false;
    out.owner_id = owner;
    out.revision = rev;
    std::vector<std::string> strings;
    for (size_t scan = 65; scan + 1 < doc.size() && strings.size() < 4;) {
        const uint8_t len = doc[scan];
        if (len >= 1 && len <= 200 && scan + 1 + len <= doc.size()) {
            bool printable = true;
            for (size_t k = 0; k < len; ++k) { uint8_t ch = doc[scan + 1 + k]; if (ch < 0x20 || ch > 0x7e) { printable = false; break; } }
            if (printable) { strings.emplace_back(doc.begin() + scan + 1, doc.begin() + scan + 1 + len); scan += 1 + len; continue; }
        }
        ++scan;
    }
    if (!strings.empty()) out.display_name = strings.front();
    for (const auto& s : strings) { if (s.rfind("http", 0) == 0) { out.avatar_url = s; break; } }
    return true;
}

bool DecodeDashPayContactRequest(Span<const uint8_t> doc, ContactRequest& out)
{
    Identifier id, owner;
    uint64_t rev;
    if (DecodeDocumentHeader(doc, id, owner, rev) == 0) return false;
    out.owner_id = owner;
    out.document_id = id;
    if (doc.size() >= 65 + 32) {
        std::copy(doc.begin() + 65, doc.begin() + 65 + 32, out.to_user_id.begin());
    }
    for (size_t scan = 65; scan + 1 + 96 <= doc.size(); ++scan) {
        if (doc[scan] == 96) {
            out.encrypted_public_key.assign(doc.begin() + scan + 1, doc.begin() + scan + 1 + 96);
            break;
        }
    }
    return true;
}

} // namespace platform::dpp
