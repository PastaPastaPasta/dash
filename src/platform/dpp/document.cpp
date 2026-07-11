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

// -----------------------------------------------------------------------------
// Stored-document decoders. These are deliberately schema-specific, but they
// follow rs-dpp DocumentV0::{serialize_v0,v1,v2} exactly: bincode varints for
// version/revision and variable lengths, a big-endian timestamp bitmap, then
// contract properties in their declared position order. Optional properties
// carry a one-byte presence marker; fixed byte arrays do not carry a length.
// -----------------------------------------------------------------------------

namespace {
class StoredDocumentReader
{
public:
    explicit StoredDocumentReader(Span<const uint8_t> bytes) : m_bytes(bytes) {}

    bool U8(uint8_t& out) { if (m_pos == m_bytes.size()) return false; out = m_bytes[m_pos++]; return true; }
    bool Bytes(size_t count, std::vector<uint8_t>& out) {
        if (count > Remaining()) return false;
        out.assign(m_bytes.begin() + m_pos, m_bytes.begin() + m_pos + count); m_pos += count; return true;
    }
    bool IdentifierValue(Identifier& out) {
        if (out.size() > Remaining()) return false;
        std::copy(m_bytes.begin() + m_pos, m_bytes.begin() + m_pos + out.size(), out.begin());
        m_pos += out.size(); return true;
    }
    bool Varint(uint64_t& out) {
        uint8_t first;
        if (!U8(first)) return false;
        if (first < 0xfb) { out = first; return true; }
        const size_t width = first == 0xfb ? 2 : first == 0xfc ? 4 : first == 0xfd ? 8 : 0;
        if (width == 0 || width > Remaining()) return false;
        out = 0;
        for (size_t i = 0; i < width; ++i) out = (out << 8) | m_bytes[m_pos++];
        return true;
    }
    bool BE16(uint16_t& out) {
        if (Remaining() < 2) return false;
        out = (uint16_t{m_bytes[m_pos]} << 8) | m_bytes[m_pos + 1]; m_pos += 2; return true;
    }
    bool BE32(uint32_t& out) {
        if (Remaining() < 4) return false;
        out = 0; for (int i = 0; i < 4; ++i) out = (out << 8) | m_bytes[m_pos++]; return true;
    }
    bool BE64(uint64_t& out) {
        if (Remaining() < 8) return false;
        out = 0; for (int i = 0; i < 8; ++i) out = (out << 8) | m_bytes[m_pos++]; return true;
    }
    bool String(std::string& out, size_t max) {
        uint64_t len;
        if (!Varint(len) || len > max || len > Remaining()) return false;
        out.assign(m_bytes.begin() + m_pos, m_bytes.begin() + m_pos + len); m_pos += len; return true;
    }
    bool Optional(bool& present) { uint8_t marker; if (!U8(marker) || marker > 1) return false; present = marker == 1; return true; }
    bool Skip(size_t count) { if (count > Remaining()) return false; m_pos += count; return true; }
    size_t Position() const { return m_pos; }
    size_t Remaining() const { return m_bytes.size() - m_pos; }
private:
    Span<const uint8_t> m_bytes;
    size_t m_pos{0};
};

bool Header(StoredDocumentReader& reader, bool revisioned, bool transferable, uint64_t& version,
            Identifier& id, Identifier& owner, uint64_t& revision,
            uint16_t& flags)
{
    revision = 0;
    if (!reader.Varint(version) || version > 2 || !reader.IdentifierValue(id) ||
        !reader.IdentifierValue(owner)) return false;
    if (version >= 2 && transferable) {
        bool creator_present;
        if (!reader.Optional(creator_present) || (creator_present && !reader.Skip(32))) return false;
    }
    return (!revisioned || reader.Varint(revision)) && reader.BE16(flags);
}

bool SkipTimes(StoredDocumentReader& reader, uint16_t flags, Profile* profile = nullptr,
               ContactRequest* contact = nullptr)
{
    for (uint16_t bit = 1; bit <= 32; bit <<= 1) {
        if (!(flags & bit)) continue;
        uint64_t value;
        if (!reader.BE64(value)) return false;
        if (profile && bit == 1) profile->created_at = value;
        if (profile && bit == 2) profile->updated_at = value;
        if (contact && bit == 1) contact->created_at = value;
    }
    for (uint16_t bit = 64; bit <= 256; bit <<= 1) {
        if (!(flags & bit)) continue;
        uint32_t value;
        if (!reader.BE32(value)) return false;
        if (contact && bit == 64) contact->core_height_created_at = value;
    }
    return true;
}
} // namespace

size_t DecodeDocumentHeader(Span<const uint8_t> doc, Identifier& id_out, Identifier& owner_out, uint64_t& revision_out)
{
    StoredDocumentReader reader{doc};
    uint64_t version;
    uint16_t flags;
    if (!Header(reader, true, false, version, id_out, owner_out, revision_out, flags)) return 0;
    return reader.Position();
}

bool DecodeDpnsDomain(Span<const uint8_t> doc, DpnsName& out)
{
    StoredDocumentReader r{doc};
    uint64_t version, revision;
    uint16_t flags;
    Identifier owner;
    if (!Header(r, true, true, version, out.document_id, owner, revision, flags) || !SkipTimes(r, flags)) return false;
    // DPNS domains use seller-set trade mode in protocol 12, so a price
    // presence marker follows timestamps even when no sale price is set.
    bool price_present;
    if (!r.Optional(price_present) || (price_present && !r.Skip(8))) return false;
    if (!r.String(out.label, 63) || !r.String(out.normalized_label, 63)) return false;
    bool parent_present;
    if (!r.Optional(parent_present)) return false;
    std::string display_parent;
    if (parent_present && !r.String(display_parent, 63)) return false;
    if (!r.String(out.parent_domain, 63)) return false;
    // preorderSalt is required on document creation but transient in the
    // contract, so Drive omits its value from stored documents. The schema
    // slot still carries the optional-property marker; tolerate a value for
    // older protocol fixtures, but do not require one.
    bool salt_present;
    if (!r.Optional(salt_present) || (salt_present && !r.Skip(32))) return false;
    uint64_t records_len;
    if (!r.Varint(records_len) || records_len > r.Remaining()) return false;
    bool identity_present;
    if (!r.Optional(identity_present) || !identity_present || !r.IdentifierValue(out.identity)) return false;
    // The records object currently contains only identity; consume any future
    // bytes within the length before parsing subdomainRules.
    if (records_len < 33 || !r.Skip(records_len - 33)) return false;
    uint64_t rules_len;
    uint8_t allow_subdomains;
    return r.Varint(rules_len) && rules_len == 1 && r.U8(allow_subdomains) && allow_subdomains <= 1;
}

bool DecodeDashPayProfile(Span<const uint8_t> doc, Profile& out)
{
    StoredDocumentReader r{doc};
    uint64_t version;
    uint16_t flags;
    if (!Header(r, true, false, version, out.document_id, out.owner_id, out.revision, flags) ||
        !SkipTimes(r, flags, &out)) return false;
    bool present;
    if (!r.Optional(present) || (present && !r.String(out.avatar_url, 2048))) return false;
    if (!r.Optional(present) || (present && !r.Bytes(32, out.avatar_hash))) return false;
    if (!r.Optional(present) || (present && !r.Bytes(8, out.avatar_fingerprint))) return false;
    if (!r.Optional(present) || (present && !r.String(out.public_message, 140))) return false;
    if (!r.Optional(present) || (present && !r.String(out.display_name, 25))) return false;
    return true;
}

bool DecodeDashPayContactRequest(Span<const uint8_t> doc, ContactRequest& out)
{
    StoredDocumentReader r{doc};
    uint64_t version, unused_revision;
    uint16_t flags;
    if (!Header(r, false, false, version, out.document_id, out.owner_id, unused_revision, flags) ||
        !SkipTimes(r, flags, nullptr, &out) || !r.IdentifierValue(out.to_user_id) ||
        !r.Bytes(96, out.encrypted_public_key)) return false;
    uint64_t value;
    // The unbounded JSON-schema integer fields are I64 in DPP. Stored v0
    // encoded all integer properties as I64 as well, so all versions use 8B.
    if (!r.BE64(value) || value > UINT32_MAX) return false; out.sender_key_index = value;
    if (!r.BE64(value) || value > UINT32_MAX) return false; out.recipient_key_index = value;
    if (!r.BE64(value) || value > UINT32_MAX) return false; out.account_reference = value;
    bool present;
    if (!r.Optional(present)) return false;
    if (present) { uint64_t len; if (!r.Varint(len) || len < 48 || len > 80 || !r.Bytes(len, out.encrypted_account_label)) return false; }
    if (!r.Optional(present)) return false;
    if (present) { uint64_t len; std::vector<uint8_t> ignored; if (!r.Varint(len) || len < 38 || len > 102 || !r.Bytes(len, ignored)) return false; }
    return true;
}

} // namespace platform::dpp
