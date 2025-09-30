// Copyright (c) 2023-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/context.h>

#include <net.h>
#include <scheduler.h>

#include <evo/deterministicmns.h>

#ifdef ENABLE_WALLET
#include <coinjoin/client.h>
#endif // ENABLE_WALLET

CJContext::CJContext(ChainstateManager& chainman, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
                     CTxMemPool& mempool, const CActiveMasternodeManager* const mn_activeman,
                     const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman, bool relay_txes)
#ifdef ENABLE_WALLET
    :
    m_relay_txes{relay_txes},
    walletman{std::make_unique<CoinJoinWalletManager>(chainman, dmnman, mn_metaman, mempool, mn_sync, isman, queueman,
                                                      /*is_masternode=*/mn_activeman != nullptr)},
    queueman{m_relay_txes ? std::make_unique<CCoinJoinClientQueueManager>(*walletman, dmnman, mn_metaman, mn_sync,
                                                                          /*is_masternode=*/mn_activeman != nullptr)
                          : nullptr}
#endif // ENABLE_WALLET
{}

CJContext::~CJContext() = default;

void CJContext::Schedule(CConnman& connman, CScheduler& scheduler)
{
#ifdef ENABLE_WALLET
    if (!m_relay_txes) return;
    scheduler.scheduleEvery(std::bind(&CCoinJoinClientQueueManager::DoMaintenance, std::ref(*queueman)),
                            std::chrono::seconds{1});
    scheduler.scheduleEvery(std::bind(&CoinJoinWalletManager::DoMaintenance, std::ref(*walletman), std::ref(connman)),
                            std::chrono::seconds{1});
#endif // ENABLE_WALLET
}

void CJContext::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
#ifdef ENABLE_WALLET
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    walletman->ForEachCJClientMan(
        [&pindexNew](std::unique_ptr<CCoinJoinClientManager>& clientman) { clientman->UpdatedBlockTip(pindexNew); });
#endif // ENABLE_WALLET
}

bool CJContext::hasQueue(const uint256& hash) const
{
#ifdef ENABLE_WALLET
    if (queueman) {
        return queueman->HasQueue(hash);
    }
#endif // ENABLE_WALLET
    return false;
}

CCoinJoinClientManager* CJContext::getClient(const std::string& name)
{
#ifdef ENABLE_WALLET
    return walletman->Get(name);
#else
    return nullptr;
#endif // ENABLE_WALLET
}

MessageProcessingResult CJContext::processMessage(CNode& pfrom, CChainState& chainstate, CConnman& connman,
                                                  CTxMemPool& mempool, std::string_view msg_type, CDataStream& vRecv)
{
#ifdef ENABLE_WALLET
    walletman->ForEachCJClientMan([&](std::unique_ptr<CCoinJoinClientManager>& clientman) {
        clientman->ProcessMessage(pfrom, chainstate, connman, mempool, msg_type, vRecv);
    });
    if (queueman) {
        return queueman->ProcessMessage(pfrom.GetId(), connman, msg_type, vRecv);
    }
#endif // ENABLE_WALLET
    return {};
}

std::optional<CCoinJoinQueue> CJContext::getQueueFromHash(const uint256& hash) const
{
#ifdef ENABLE_WALLET
    if (queueman) {
        return queueman->GetQueueFromHash(hash);
    }
#endif // ENABLE_WALLET
    return std::nullopt;
}

std::optional<int> CJContext::getQueueSize() const
{
#ifdef ENABLE_WALLET
    if (queueman) {
        return queueman->GetQueueSize();
    }
#endif // ENABLE_WALLET
    return std::nullopt;
}

std::vector<CDeterministicMNCPtr> CJContext::getMixingMasternodes() const
{
    std::vector<CDeterministicMNCPtr> ret{};
#ifdef ENABLE_WALLET
    walletman->ForEachCJClientMan(
        [&](const std::unique_ptr<CCoinJoinClientManager>& clientman) { clientman->GetMixingMasternodesInfo(ret); });
#endif // ENABLE_WALLET
    return ret;
}

void CJContext::addWallet(const std::shared_ptr<wallet::CWallet>& wallet)
{
#ifdef ENABLE_WALLET
    walletman->Add(wallet);
#endif // ENABLE_WALLET
}

void CJContext::flushWallet(const std::string& name)
{
#ifdef ENABLE_WALLET
    walletman->Flush(name);
#endif // ENABLE_WALLET
}

void CJContext::removeWallet(const std::string& name)
{
#ifdef ENABLE_WALLET
    walletman->Remove(name);
#endif // ENABLE_WALLET
}
