// Copyright (c) 2014-2022 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_SERVER_H
#define BITCOIN_COINJOIN_SERVER_H

#include <coinjoin/coinjoin.h>
#include <net.h>

class CCoinJoinServer;
class UniValue;
class CMasternodeSync;

// The main object for accessing mixing
extern std::unique_ptr<CCoinJoinServer> coinJoinServer;

/** Used to keep track of current status of mixing pool
 */
class CCoinJoinServer : public CCoinJoinBaseSession, public CCoinJoinBaseManager
{
private:
    CConnman& connman;
    std::shared_ptr<CMasternodeSync> m_masternode_sync;

    // Mixing uses collateral transactions to trust parties entering the pool
    // to behave honestly. If they don't it takes their money.
    std::vector<CTransactionRef> vecSessionCollaterals;

    bool fUnitTest;

    /// Add a clients entry to the pool
    bool AddEntry(const CCoinJoinEntry& entry, PoolMessage& nMessageIDRet) LOCKS_EXCLUDED(cs_coinjoin);
    /// Add signature to a txin
    bool AddScriptSig(const CTxIn& txin) LOCKS_EXCLUDED(cs_coinjoin);

    /// Charge fees to bad actors (Charge clients a fee if they're abusive)
    void ChargeFees() const LOCKS_EXCLUDED(cs_coinjoin);
    /// Rarely charge fees to pay miners
    void ChargeRandomFees() const;
    /// Consume collateral in cases when peer misbehaved
    void ConsumeCollateral(const CTransactionRef& txref) const;

    /// Check for process
    void CheckPool();

    void CreateFinalTransaction() LOCKS_EXCLUDED(cs_coinjoin);
    void CommitFinalTransaction() LOCKS_EXCLUDED(cs_coinjoin);

    /// Is this nDenom and txCollateral acceptable?
    bool IsAcceptableDSA(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet) const;
    bool CreateNewSession(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet) LOCKS_EXCLUDED(cs_vecqueue);
    bool AddUserToExistingSession(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet);
    /// Do we have enough users to take entries?
    bool IsSessionReady() const;

    /// Check that all inputs are signed. (Are all inputs signed?)
    bool IsSignaturesComplete() const LOCKS_EXCLUDED(cs_coinjoin);
    /// Check to make sure a given input matches an input in the pool and its scriptSig is valid
    bool IsInputScriptSigValid(const CTxIn& txin) const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    // Set the 'state' value, with some logging and capturing when the state changed
    void SetState(PoolState nStateNew);

    /// Relay mixing Messages
    void RelayFinalTransaction(const CTransaction& txFinal) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    void PushStatus(CNode* pnode, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID) const;
    void RelayStatus(PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID = MSG_NOERR) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    void RelayCompletedTransaction(PoolMessage nMessageID) LOCKS_EXCLUDED(cs_coinjoin);

    void ProcessDSACCEPT(CNode* pfrom, const std::string& msg_type, CDataStream& vRecv, bool enable_bip61) LOCKS_EXCLUDED(cs_vecqueue);
    void ProcessDSQUEUE(CNode* pfrom, const std::string& msg_type, CDataStream& vRecv, bool enable_bip61) LOCKS_EXCLUDED(cs_vecqueue);
    void ProcessDSVIN(CNode* pfrom, const std::string& msg_type, CDataStream& vRecv, bool enable_bip61) LOCKS_EXCLUDED(cs_coinjoin);
    void ProcessDSSIGNFINALTX(CNode* pfrom, const std::string& msg_type, CDataStream& vRecv, bool enable_bip61) LOCKS_EXCLUDED(cs_coinjoin);

    void SetNull() EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

public:
    explicit CCoinJoinServer(CConnman& _connman, std::shared_ptr<CMasternodeSync> mn_sync) :
        vecSessionCollaterals(),
        fUnitTest(false),
        connman(_connman),
        m_masternode_sync(mn_sync) {};

    void ProcessMessage(CNode* pfrom, const std::string& msg_type, CDataStream& vRecv, bool enable_bip61);

    bool HasTimedOut() const;
    void CheckTimeout();
    void CheckForCompleteQueue();

    void DoMaintenance() const;

    void GetJsonInfo(UniValue& obj) const;
};

#endif // BITCOIN_COINJOIN_SERVER_H
