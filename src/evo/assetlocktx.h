// Copyright (c) 2020 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_ASSETLOCKTX_H
#define BITCOIN_EVO_ASSETLOCKTX_H

#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <evo/providertx.h>
#include <univalue.h>

#include <serialize.h>

class CBlockIndex;

class CAssetLockPayload
{
public:
    static constexpr uint16_t CURRENT_VERSION = 1;
    static constexpr auto SPECIALTX_TYPE = TRANSACTION_ASSET_LOCK;

private:
    uint16_t nVersion{CURRENT_VERSION};
    uint16_t nType{0};
    CKeyID pubKeyHash;

public:
    CAssetLockPayload(uint16_t nType, const CKeyID& pubKeyHash) : nType(nType),
                                                                  pubKeyHash(pubKeyHash) {}

    CAssetLockPayload() = default;

    SERIALIZE_METHODS(CAssetLockPayload, obj)
    {
        READWRITE(
                obj.nVersion,
                obj.nType,
                obj.pubKeyHash
        );
    }

    std::string ToString() const;

    void ToJson(UniValue& obj) const
    {
        obj.clear();
        obj.setObject();
        obj.pushKV("version", int(nVersion));
        obj.pushKV("type", int(nType));
        obj.pushKV("pubKeyHash", pubKeyHash.GetHex());
    }

    uint16_t getVersion() const;

    uint16_t getType() const;

    const CKeyID& getPubKeyHash() const;
};

maybe_error CheckAssetLockTx(const CTransaction& tx, const CBlockIndex* pindexPrev);

#endif // BITCOIN_EVO_ASSETLOCKTX_H
