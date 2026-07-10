// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/dpp/identity.h>

#include <platform/dpp/bincode.h>

#include <tinyformat.h>

#include <limits>

namespace platform::dpp {

namespace {

//! rs-dpp limits: identity 15000 bytes, standalone key 2000 bytes.
constexpr size_t MAX_IDENTITY_SIZE{15000};
constexpr size_t MAX_KEY_SIZE{2000};
constexpr size_t MAX_KEY_DATA{256};
constexpr size_t MAX_KEYS{100};
constexpr size_t MAX_DOCUMENT_TYPE_NAME{256};

//! Reads one IdentityPublicKey (enum { V0 } + IdentityPublicKeyV0 fields).
bool ReadIdentityPublicKey(Reader& reader, platform::IdentityPublicKey& out)
{
    uint64_t version;
    if (!reader.ReadVarint(version)) return false;
    if (version != 0) return reader.SetError(strprintf("unsupported IdentityPublicKey version %d", version));

    uint64_t id, purpose, security_level, key_type;
    if (!reader.ReadVarint(id)) return false;
    if (id > std::numeric_limits<uint32_t>::max()) return reader.SetError("key id out of range");
    if (!reader.ReadVarint(purpose)) return false;
    if (purpose > 6) return reader.SetError(strprintf("unknown key purpose %d", purpose));
    if (!reader.ReadVarint(security_level)) return false;
    if (security_level > 3) return reader.SetError(strprintf("unknown key security level %d", security_level));

    // contract_bounds: Option<ContractBounds>; both variants start with the
    // bound contract id, SingleContractDocumentType adds a type name. The
    // GUI types do not surface bounds, so the payload is validated and
    // skipped.
    bool has_bounds;
    if (!reader.ReadOptionTag(has_bounds)) return false;
    if (has_bounds) {
        uint64_t bounds_kind;
        if (!reader.ReadVarint(bounds_kind)) return false;
        if (bounds_kind > 1) return reader.SetError(strprintf("unknown contract bounds kind %d", bounds_kind));
        Identifier bound_contract;
        if (!reader.ReadInto(bound_contract)) return false;
        if (bounds_kind == 1) {
            std::string document_type_name;
            if (!reader.ReadString(document_type_name, MAX_DOCUMENT_TYPE_NAME)) return false;
        }
    }

    if (!reader.ReadVarint(key_type)) return false;
    if (key_type > 4) return reader.SetError(strprintf("unknown key type %d", key_type));

    bool read_only;
    if (!reader.ReadBool(read_only)) return false;

    Bytes data;
    if (!reader.ReadByteVec(data, MAX_KEY_DATA)) return false;

    bool has_disabled_at;
    if (!reader.ReadOptionTag(has_disabled_at)) return false;
    uint64_t disabled_at{0};
    if (has_disabled_at && !reader.ReadVarint(disabled_at)) return false;

    out.id = static_cast<uint32_t>(id);
    out.purpose = static_cast<platform::IdentityPublicKey::Purpose>(purpose);
    out.security_level = static_cast<platform::IdentityPublicKey::SecurityLevel>(security_level);
    out.type = static_cast<platform::IdentityPublicKey::Type>(key_type);
    out.read_only = read_only;
    out.data = std::move(data);
    out.disabled_at = has_disabled_at ? std::optional<uint64_t>{disabled_at} : std::nullopt;
    return true;
}

} // namespace

std::optional<platform::Identity> DecodeIdentity(Span<const uint8_t> bytes, std::string& error)
{
    if (bytes.size() > MAX_IDENTITY_SIZE) {
        error = "identity too large";
        return std::nullopt;
    }
    Reader reader{bytes};
    uint64_t version;
    if (!reader.ReadVarint(version) || version != 0) {
        error = reader.HasError() ? reader.GetError() : strprintf("unsupported Identity version %d", version);
        return std::nullopt;
    }

    platform::Identity identity;
    if (!reader.ReadInto(identity.id)) {
        error = reader.GetError();
        return std::nullopt;
    }

    uint64_t num_keys;
    if (!reader.ReadVarint(num_keys) || num_keys > MAX_KEYS) {
        error = reader.HasError() ? reader.GetError() : "too many identity keys";
        return std::nullopt;
    }
    std::optional<uint64_t> last_key_id;
    for (uint64_t i = 0; i < num_keys; ++i) {
        uint64_t map_key_id;
        platform::IdentityPublicKey key;
        if (!reader.ReadVarint(map_key_id) || !ReadIdentityPublicKey(reader, key)) {
            error = reader.GetError();
            return std::nullopt;
        }
        // BTreeMap keys are strictly ascending.
        if (last_key_id && map_key_id <= *last_key_id) {
            error = "identity key ids not strictly ascending";
            return std::nullopt;
        }
        last_key_id = map_key_id;
        if (map_key_id != key.id) {
            error = strprintf("identity key map id %d does not match key id %d", map_key_id, key.id);
            return std::nullopt;
        }
        identity.public_keys.push_back(std::move(key));
    }

    if (!reader.ReadVarint(identity.balance) || !reader.ReadVarint(identity.revision)) {
        error = reader.GetError();
        return std::nullopt;
    }
    if (!reader.IsEof()) {
        error = "trailing bytes after identity";
        return std::nullopt;
    }
    return identity;
}

std::optional<platform::IdentityPublicKey> DecodeIdentityPublicKey(Span<const uint8_t> bytes,
                                                                   std::string& error)
{
    if (bytes.size() > MAX_KEY_SIZE) {
        error = "identity public key too large";
        return std::nullopt;
    }
    Reader reader{bytes};
    platform::IdentityPublicKey key;
    if (!ReadIdentityPublicKey(reader, key)) {
        error = reader.GetError();
        return std::nullopt;
    }
    if (!reader.IsEof()) {
        error = "trailing bytes after identity public key";
        return std::nullopt;
    }
    return key;
}

} // namespace platform::dpp
