// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_NODE_H
#define BITCOIN_MASTERNODE_NODE_H

#include <bls/bls.h>
#include <netaddress.h>
#include <primitives/transaction.h>
#include <threadsafety.h>
#include <validationinterface.h>

struct CActiveMasternodeInfo {
    // Keys for the active Masternode
    const CBLSSecretKey blsKeyOperator;
    const CBLSPublicKey blsPubKeyOperator;

    // Initialized while registering Masternode
    uint256 proTxHash;
    COutPoint outpoint;
    CService service;
    bool legacy{true};

    CActiveMasternodeInfo(const CBLSSecretKey& blsKeyOperator, const CBLSPublicKey& blsPubKeyOperator) :
        blsKeyOperator(blsKeyOperator), blsPubKeyOperator(blsPubKeyOperator) {};
};

class CActiveMasternodeManager final : public CValidationInterface
{
public:
    enum masternode_state_t {
        MASTERNODE_WAITING_FOR_PROTX,
        MASTERNODE_POSE_BANNED,
        MASTERNODE_REMOVED,
        MASTERNODE_OPERATOR_KEY_CHANGED,
        MASTERNODE_PROTX_IP_CHANGED,
        MASTERNODE_READY,
        MASTERNODE_ERROR,
    };

    mutable RecursiveMutex cs;

private:
    masternode_state_t state{MASTERNODE_WAITING_FOR_PROTX};
    CActiveMasternodeInfo m_info GUARDED_BY(cs);
    std::string strError;

    CConnman& connman;

public:
    explicit CActiveMasternodeManager(const CBLSSecretKey& sk, CConnman& _connman);

    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

    void Init(const CBlockIndex* pindex);

    std::string GetStateString() const;
    std::string GetStatus() const;

    static bool IsValidNetAddr(const CService& addrIn);

    template <template <typename> class EncryptedObj, typename Obj>
    [[nodiscard]] bool Decrypt(const EncryptedObj<Obj>& obj, size_t idx, Obj& ret_obj, int version) const
        LOCKS_EXCLUDED(cs);
    [[nodiscard]] CBLSSignature Sign(const uint256& hash) const LOCKS_EXCLUDED(cs);
    [[nodiscard]] CBLSSignature Sign(const uint256& hash, const bool is_legacy) const LOCKS_EXCLUDED(cs);

    [[nodiscard]] const COutPoint& GetOutPoint() const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        return m_info.outpoint;
    }

    [[nodiscard]] const uint256& GetProTxHash() const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        return m_info.proTxHash;
    }

    [[nodiscard]] CBLSPublicKey GetPubKey() const EXCLUSIVE_LOCKS_REQUIRED(cs);

    [[nodiscard]] const CService& GetService() const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        return m_info.service;
    }

    [[nodiscard]] const bool IsLegacy() const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        return m_info.legacy;
    }

private:
    bool GetLocalAddress(CService& addrRet);
};

extern std::unique_ptr<CActiveMasternodeManager> activeMasternodeManager;

#endif // BITCOIN_MASTERNODE_NODE_H
