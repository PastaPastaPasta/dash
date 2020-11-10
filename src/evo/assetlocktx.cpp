// Copyright (c) 2017-2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/assetlocktx.h>
#include <evo/specialtx.h>

#include <consensus/merkle.h>

maybe_error CheckAssetLockTx(const CTransaction& tx, const CBlockIndex* pindexPrev)
{
    if (tx.nType != TRANSACTION_ASSET_LOCK) {
        return {100, "bad-assetlocktx-type"};
    }

    CAssetLockPayload assetLockTx;
    if (!GetTxPayload(tx, assetLockTx)) {
        return {100, "bad-assetlocktx-payload"};
    }

    if (assetLockTx.getVersion() == 0 || assetLockTx.getVersion() > CAssetLockPayload::CURRENT_VERSION) {
        return {100, "bad-assetlocktx-version"};
    }

    if (assetLockTx.getPubKeyHash().IsNull()) {
        return {100, "bad-assetlocktx-pubKeyHash"};
    }

    return {};
}

std::string CAssetLockPayload::ToString() const
{
    return strprintf("CAssetLockPayload(nVersion=%d,nType=%d,pubKeyHash=%d)", nVersion, nType, pubKeyHash.GetHex());
}

uint16_t CAssetLockPayload::getVersion() const {
    return nVersion;
}

uint16_t CAssetLockPayload::getType() const {
    return nType;
}

const CKeyID& CAssetLockPayload::getPubKeyHash() const {
    return pubKeyHash;
}
