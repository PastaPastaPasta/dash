// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DPP_DOCUMENT_H
#define BITCOIN_PLATFORM_DPP_DOCUMENT_H

#include <platform/dpp/bincode.h>
#include <platform/params.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/**
 * platform_value::Value encoding for document properties, restricted to the
 * kinds the DPNS and DashPay document types need. Mirrors dashpay/platform
 * (tag v4.0.0) packages/rs-platform-value/src/lib.rs: `Value` is a plain
 * bincode Encode enum, so each value is the varint variant index in
 * declaration order followed by the payload.
 *
 * Document create/replace transitions carry the user properties as
 * BTreeMap<String, Value> (rs-dpp
 * .../document_create_transition/v0/mod.rs `data`), which bincode encodes as
 * a varint entry count followed by (key, value) pairs in ascending byte-wise
 * key order — matched here by std::map<std::string, Value>.
 */
namespace platform::dpp {

//! Variant indexes of platform_value::Value (declaration order in
//! rs-platform-value/src/lib.rs).
enum class ValueKind : uint8_t {
    U128 = 0,
    I128 = 1,
    U64 = 2,
    I64 = 3,
    U32 = 4,
    I32 = 5,
    U16 = 6,
    I16 = 7,
    U8 = 8,
    I8 = 9,
    BYTES = 10,
    BYTES20 = 11,
    BYTES32 = 12,
    BYTES36 = 13,
    ENUM_U8 = 14,
    ENUM_STRING = 15,
    IDENTIFIER = 16,
    FLOAT = 17,
    TEXT = 18,
    BOOL = 19,
    NULL_VALUE = 20,
    ARRAY = 21,
    MAP = 22,
};

//! Subset of platform_value::Value used by the four GUI document types.
class Value
{
public:
    using Map = std::vector<std::pair<Value, Value>>;

    static Value U32(uint32_t value);
    static Value U64(uint64_t value);
    static Value MakeBytes(Bytes bytes);
    static Value MakeBytes32(const std::array<uint8_t, 32>& bytes);
    static Value Id(const Identifier& id);
    static Value Text(std::string text);
    static Value Boolean(bool value);
    static Value MakeMap(Map entries);

    void Encode(Writer& writer) const;

private:
    Value() = default;

    ValueKind m_kind{ValueKind::NULL_VALUE};
    uint64_t m_int{0};
    bool m_bool{false};
    Bytes m_bytes;      //!< BYTES / BYTES32 / IDENTIFIER payload
    std::string m_text; //!< TEXT payload
    std::shared_ptr<Map> m_map; //!< MAP payload
};

//! Document properties in BTreeMap<String, Value> order.
using DocumentData = std::map<std::string, Value>;

//! Encodes a document data map: varint count + sorted (key, value) pairs.
void EncodeDocumentData(Writer& writer, const DocumentData& data);

//! Document id: DSHA256(contract_id || owner_id || type_name || entropy),
//! per rs-dpp src/document/generate_document_id.rs
//! Document::generate_document_id_v0.
Identifier GenerateDocumentId(const Identifier& contract_id,
                              const Identifier& owner_id,
                              const std::string& document_type_name,
                              Span<const uint8_t> entropy);

} // namespace platform::dpp

#endif // BITCOIN_PLATFORM_DPP_DOCUMENT_H
