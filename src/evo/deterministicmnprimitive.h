// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_DETERMINISTICMNPRIMITIVE_H
#define BITCOIN_EVO_DETERMINISTICMNPRIMITIVE_H

#include <uint256.h>
#include <serialize.h>
#include <primitives/transaction.h>
#include <univalue.h>
#include <bls/bls.h>
#include <evo/providertx.h>

#include <utility>

class CDeterministicMNState
{
private:
    int nPoSeBanHeight{-1};

    friend class CDeterministicMNStateDiff;

public:
    int nRegisteredHeight{-1};
    int nLastPaidHeight{0};
    int nPoSePenalty{0};
    int nPoSeRevivedHeight{-1};
    uint16_t nRevocationReason{CProUpRevTx::REASON_NOT_SPECIFIED};

    // the block hash X blocks after registration, used in quorum calculations
    uint256 confirmedHash;
    // sha256(proTxHash, confirmedHash) to speed up quorum calculations
    // please note that this is NOT a double-sha256 hash
    uint256 confirmedHashWithProRegTxHash;

    CKeyID keyIDOwner;
    CBLSLazyPublicKey pubKeyOperator;
    CKeyID keyIDVoting;
    CService addr;
    CScript scriptPayout;
    CScript scriptOperatorPayout;

public:
    CDeterministicMNState() = default;
    explicit CDeterministicMNState(const CProRegTx& proTx) :
            keyIDOwner(proTx.keyIDOwner),
            keyIDVoting(proTx.keyIDVoting),
            addr(proTx.addr),
            scriptPayout(proTx.scriptPayout)
    {
        pubKeyOperator.Set(proTx.pubKeyOperator);
    }
    template <typename Stream>
    CDeterministicMNState(deserialize_type, Stream& s)
    {
        s >> *this;
    }

    SERIALIZE_METHODS(CDeterministicMNState, obj)
    {
        READWRITE(
                obj.nRegisteredHeight,
                obj.nLastPaidHeight,
                obj.nPoSePenalty,
                obj.nPoSeRevivedHeight,
                obj.nPoSeBanHeight,
                obj.nRevocationReason,
                obj.confirmedHash,
                obj.confirmedHashWithProRegTxHash,
                obj.keyIDOwner,
                obj.pubKeyOperator,
                obj.keyIDVoting,
                obj.addr,
                obj.scriptPayout,
                obj.scriptOperatorPayout
        );
    }

    void ResetOperatorFields()
    {
        pubKeyOperator.Set(CBLSPublicKey());
        addr = CService();
        scriptOperatorPayout = CScript();
        nRevocationReason = CProUpRevTx::REASON_NOT_SPECIFIED;
    }
    void BanIfNotBanned(int height)
    {
        if (!IsBanned()) {
            nPoSeBanHeight = height;
        }
    }
    int GetBannedHeight() const
    {
        return nPoSeBanHeight;
    }
    bool IsBanned() const
    {
        return nPoSeBanHeight != -1;
    }
    void Revive(int nRevivedHeight)
    {
        nPoSePenalty = 0;
        nPoSeBanHeight = -1;
        nPoSeRevivedHeight = nRevivedHeight;
    }
    void UpdateConfirmedHash(const uint256& _proTxHash, const uint256& _confirmedHash)
    {
        confirmedHash = _confirmedHash;
        CSHA256 h;
        h.Write(_proTxHash.begin(), _proTxHash.size());
        h.Write(_confirmedHash.begin(), _confirmedHash.size());
        h.Finalize(confirmedHashWithProRegTxHash.begin());
    }

public:
    std::string ToString() const;
    void ToJson(UniValue& obj) const;
};
using CDeterministicMNStatePtr = std::shared_ptr<CDeterministicMNState>;
using CDeterministicMNStateCPtr = std::shared_ptr<const CDeterministicMNState>;


class CDeterministicMN
{
private:
    uint64_t internalId{std::numeric_limits<uint64_t>::max()};

public:
    CDeterministicMN() = delete; // no default constructor, must specify internalId
    explicit CDeterministicMN(uint64_t _internalId) : internalId(_internalId)
    {
        // only non-initial values
        assert(_internalId != std::numeric_limits<uint64_t>::max());
    }
    // TODO: can be removed in a future version
    CDeterministicMN(CDeterministicMN mn, uint64_t _internalId) : CDeterministicMN(std::move(mn)) {
        // only non-initial values
        assert(_internalId != std::numeric_limits<uint64_t>::max());
        internalId = _internalId;
    }

    template <typename Stream>
    CDeterministicMN(deserialize_type, Stream& s)
    {
        s >> *this;
    }

    uint256 proTxHash;
    COutPoint collateralOutpoint;
    uint16_t nOperatorReward{0};
    CDeterministicMNStateCPtr pdmnState;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, bool oldFormat)
    {
        READWRITE(proTxHash);
        if (!oldFormat) {
            READWRITE(VARINT(internalId));
        }
        READWRITE(collateralOutpoint);
        READWRITE(nOperatorReward);
        READWRITE(pdmnState);
    }

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        const_cast<CDeterministicMN*>(this)->SerializationOp(s, CSerActionSerialize(), false);
    }

    template<typename Stream>
    void Unserialize(Stream& s, bool oldFormat = false)
    {
        SerializationOp(s, CSerActionUnserialize(), oldFormat);
    }

    uint64_t GetInternalId() const;

    std::string ToString() const;
    void ToJson(UniValue& obj) const;
};
using CDeterministicMNCPtr = std::shared_ptr<const CDeterministicMN>;


#endif //BITCOIN_EVO_DETERMINISTICMNPRIMITIVE_H
