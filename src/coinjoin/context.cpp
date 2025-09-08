// Copyright (c) 2023-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <coinjoin/context.h>

#include <net.h>
#include <scheduler.h>
#include <streams.h>

#include <evo/deterministicmns.h>

#ifdef ENABLE_WALLET
#include <coinjoin/client.h>
#endif // ENABLE_WALLET

#include <memory>

#ifdef ENABLE_WALLET
class CJContextImpl final : public CJContext
{
public:
    CJContextImpl(ChainstateManager& chainman, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
                  CTxMemPool& mempool, const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman,
                  bool relay_txes);
    virtual ~CJContextImpl() = default;

public:
    void Schedule(CConnman& connman, CScheduler& scheduler) override;

public:
    bool hasQueue(const uint256& hash) const override;
    CCoinJoinClientManager* getClient(const std::string& name) override;
    MessageProcessingResult processMessage(CNode& peer, CChainState& chainstate, CConnman& connman, CTxMemPool& mempool,
                                           std::string_view msg_type, CDataStream& vRecv) override;
    std::optional<CCoinJoinQueue> getQueueFromHash(const uint256& hash) const override;
    std::optional<int> getQueueSize() const override;
    std::vector<CDeterministicMNCPtr> getMixingMasternodes() const override;
    void addWallet(const std::shared_ptr<wallet::CWallet>& wallet) override;
    void removeWallet(const std::string& name) override;
    void flushWallet(const std::string& name) override;

protected:
    // CValidationInterface
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

private:
    const bool m_relay_txes;

    const std::unique_ptr<CoinJoinWalletManager> walletman;
    const std::unique_ptr<CCoinJoinClientQueueManager> queueman;
};

CJContextImpl::CJContextImpl(ChainstateManager& chainman, CDeterministicMNManager& dmnman,
                             CMasternodeMetaMan& mn_metaman, CTxMemPool& mempool, const CMasternodeSync& mn_sync,
                             const llmq::CInstantSendManager& isman, bool relay_txes) :
    m_relay_txes{relay_txes},
    walletman{std::make_unique<CoinJoinWalletManager>(chainman, dmnman, mn_metaman, mempool, mn_sync, isman, queueman,
                                                      /*is_masternode=*/false)},
    queueman{m_relay_txes ? std::make_unique<CCoinJoinClientQueueManager>(*walletman, dmnman, mn_metaman, mn_sync) : nullptr}
{
}

void CJContextImpl::Schedule(CConnman& connman, CScheduler& scheduler)
{
    if (!m_relay_txes) return;
    scheduler.scheduleEvery(std::bind(&CCoinJoinClientQueueManager::DoMaintenance, std::ref(*queueman)),
                            std::chrono::seconds{1});
    scheduler.scheduleEvery(std::bind(&CoinJoinWalletManager::DoMaintenance, std::ref(*walletman), std::ref(connman)),
                            std::chrono::seconds{1});
}

void CJContextImpl::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    walletman->ForEachCJClientMan(
        [&pindexNew](std::unique_ptr<CCoinJoinClientManager>& clientman) { clientman->UpdatedBlockTip(pindexNew); });
}

bool CJContextImpl::hasQueue(const uint256& hash) const
{
    if (queueman) {
        return queueman->HasQueue(hash);
    }
    return false;
}

CCoinJoinClientManager* CJContextImpl::getClient(const std::string& name)
{
    return walletman->Get(name);
}

MessageProcessingResult CJContextImpl::processMessage(CNode& pfrom, CChainState& chainstate, CConnman& connman,
                                                      CTxMemPool& mempool, std::string_view msg_type, CDataStream& vRecv)
{
    walletman->ForEachCJClientMan([&](std::unique_ptr<CCoinJoinClientManager>& clientman) {
        clientman->ProcessMessage(pfrom, chainstate, connman, mempool, msg_type, vRecv);
    });
    if (queueman) {
        return queueman->ProcessMessage(pfrom.GetId(), connman, msg_type, vRecv);
    }
    return {};
}

std::optional<CCoinJoinQueue> CJContextImpl::getQueueFromHash(const uint256& hash) const
{
    if (queueman) {
        return queueman->GetQueueFromHash(hash);
    }
    return std::nullopt;
}

std::optional<int> CJContextImpl::getQueueSize() const
{
    if (queueman) {
        return queueman->GetQueueSize();
    }
    return std::nullopt;
}

std::vector<CDeterministicMNCPtr> CJContextImpl::getMixingMasternodes() const
{
    std::vector<CDeterministicMNCPtr> ret{};
    walletman->ForEachCJClientMan(
        [&](const std::unique_ptr<CCoinJoinClientManager>& clientman) { clientman->GetMixingMasternodesInfo(ret); });
    return ret;
}

void CJContextImpl::addWallet(const std::shared_ptr<wallet::CWallet>& wallet)
{
    walletman->Add(wallet);
}

void CJContextImpl::flushWallet(const std::string& name)
{
    walletman->Flush(name);
}

void CJContextImpl::removeWallet(const std::string& name)
{
    walletman->Remove(name);
}
#endif // ENABLE_WALLET

std::unique_ptr<CJContext> CJContext::make(ChainstateManager& chainman, CDeterministicMNManager& dmnman,
                                           CMasternodeMetaMan& mn_metaman, CTxMemPool& mempool,
                                           const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman,
                                           bool relay_txes)
{
#ifdef ENABLE_WALLET
    return std::make_unique<CJContextImpl>(chainman, dmnman, mn_metaman, mempool, mn_sync, isman, relay_txes);
#else
    // Cannot be constructed if wallet support isn't built
    return nullptr;
#endif // ENABLE_WALLET
}
