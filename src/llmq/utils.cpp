// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/utils.h>

#include <llmq/quorums.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/quorums_snapshot.h>
#include <llmq/utils.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <masternode/meta.h>
#include <net.h>
#include <random.h>
#include <spork.h>
#include <timedata.h>
#include <util/ranges.h>
#include <validation.h>
#include <versionbits.h>

namespace llmq
{

CCriticalSection cs_llmq_vbc;
VersionBitsCache llmq_versionbitscache;

std::vector<CDeterministicMNCPtr> CLLMQUtils::GetAllQuorumMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex)
{
    static CCriticalSection cs_members;
    static std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CDeterministicMNCPtr>, StaticSaltedHasher>> mapQuorumMembers;

    if (!IsQuorumTypeEnabled(llmqParams.type, pQuorumBaseBlockIndex->pprev)) {
        return {};
    }
    std::vector<CDeterministicMNCPtr> quorumMembers;
    {
        LOCK(cs_members);
        if (mapQuorumMembers.empty()) {
            InitQuorumsCache(mapQuorumMembers);
        }
        if (mapQuorumMembers[llmqParams.type].get(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers)) {
            return quorumMembers;
        }
    }

    auto allMns = deterministicMNManager->GetListForBlock(pQuorumBaseBlockIndex);
    auto modifier = ::SerializeHash(std::make_pair(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash()));
    quorumMembers = allMns.CalculateQuorum(llmqParams.size, modifier);
    LOCK(cs_members);
    mapQuorumMembers[llmqParams.type].insert(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers);
    return quorumMembers;
}

std::vector<CDeterministicMNCPtr> CLLMQUtils::GetAllQuorumMembersByQuarterRotation(Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum)
{
    static CCriticalSection cs_members;
    static std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CDeterministicMNCPtr>, StaticSaltedHasher>> mapQuorumMembers;

    if (!IsQuorumTypeEnabled(llmqType, pindexQuorum->pprev)) {
        return {};
    }
    std::vector<CDeterministicMNCPtr> quorumMembers;
    {
        LOCK(cs_members);
        if (mapQuorumMembers.empty()) {
            InitQuorumsCache(mapQuorumMembers);
        }
        if (mapQuorumMembers[llmqType].get(pindexQuorum->GetBlockHash(), quorumMembers)) {
            return quorumMembers;
        }
    }

    auto quorums = llmq::quorumBlockProcessor->GetMinedAndActiveCommitmentsUntilBlock(pindexQuorum);
    auto quorumIt = quorums.find(llmqType);
    if(quorumIt == quorums.end()) {
        return {};
    }
    if(quorumIt->second.empty()){
        return {};
    }

    // Since the returned quorums are in reversed order, the most recent one is at index 0
    const CBlockIndex* pBlockHMinusCIndex = LookupBlockIndex(quorumIt->second.at(0)->GetBlockHash());
    const CBlockIndex* pBlockHMinus2CIndex = LookupBlockIndex(quorumIt->second.at(1)->GetBlockHash());
    const CBlockIndex* pBlockHMinus3CIndex = LookupBlockIndex(quorumIt->second.at(2)->GetBlockHash());

    if(!pBlockHMinusCIndex || !pBlockHMinus2CIndex || !pBlockHMinus3CIndex) {
        return {};
    }

    CQuorumSnapshot quSnapshotHMinusC = quorumSnapshotManager->GetSnapshotForBlock(llmqType, pBlockHMinusCIndex);
    CQuorumSnapshot quSnapshotHMinus2C = quorumSnapshotManager->GetSnapshotForBlock(llmqType, pBlockHMinus2CIndex);
    CQuorumSnapshot quSnapshotHMinus3C = quorumSnapshotManager->GetSnapshotForBlock(llmqType, pBlockHMinus3CIndex);

    auto quarterHMinusC = CLLMQUtils::GetQuorumQuarterMembersBySnapshot(llmqType, pBlockHMinusCIndex, quSnapshotHMinusC);
    auto quarterHMinus2C = CLLMQUtils::GetQuorumQuarterMembersBySnapshot(llmqType, pBlockHMinus2CIndex, quSnapshotHMinus2C);
    auto quarterHMinus3C = CLLMQUtils::GetQuorumQuarterMembersBySnapshot(llmqType, pBlockHMinus3CIndex, quSnapshotHMinus3C);

    //TODO Add handling when build of new quorum quarter fails (newQuarterMembers is empty)
    auto newQuarterMembers = CLLMQUtils::BuildNewQuorumQuarterMembers(llmqType, pindexQuorum, quarterHMinusC, quarterHMinus2C, quarterHMinus3C);

    std::copy(quarterHMinus3C.begin(),
              quarterHMinus3C.end(),
              std::back_inserter(quorumMembers));
    std::copy(quarterHMinus2C.begin(),
              quarterHMinus2C.end(),
              std::back_inserter(quorumMembers));
    std::copy(quarterHMinusC.begin(),
              quarterHMinusC.end(),
              std::back_inserter(quorumMembers));
    std::copy(newQuarterMembers.begin(),
              newQuarterMembers.end(),
              std::back_inserter(quorumMembers));

    LOCK(cs_members);
    mapQuorumMembers[llmqType].insert(pindexQuorum->GetBlockHash(), quorumMembers);

    return quorumMembers;
}

std::vector<CDeterministicMNCPtr> CLLMQUtils::BuildNewQuorumQuarterMembers(Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum, const std::vector<CDeterministicMNCPtr>& quartersMembersMinusC, const std::vector<CDeterministicMNCPtr>& quartersMembersMinus2C, const std::vector<CDeterministicMNCPtr>& quartersMembersMinus3C)
{
    std::vector<CDeterministicMNCPtr> quarterQuorumMembers;

    //TODO Add handling cases where previous quarters are empty
    auto modifier = ::SerializeHash(std::make_pair(llmqType, pindexQuorum->GetBlockHash()));
    auto Mns = deterministicMNManager->GetListForBlock(pindexQuorum);
    auto quarterSize = GetLLMQParams(llmqType).size / 4;

    auto MnsUsedAtH = CDeterministicMNList();
    auto MnsNotUsedAtH = CDeterministicMNList();

    for(const auto mn : quartersMembersMinusC) {
        MnsUsedAtH.AddMN(mn);
    }
    for(const auto mn : quartersMembersMinus2C) {
        MnsUsedAtH.AddMN(mn);
    }
    for(const auto mn : quartersMembersMinus3C) {
        MnsUsedAtH.AddMN(mn);
    }

    Mns.ForEachMN(false, [&MnsUsedAtH, &MnsNotUsedAtH](const CDeterministicMNCPtr& dmn){
        if(!MnsUsedAtH.ContainsMN(dmn->proTxHash)){
            MnsNotUsedAtH.AddMN(dmn);
        }
    });

    auto sortedMnsUsedAtHM = MnsUsedAtH.CalculateQuorum(MnsUsedAtH.GetAllMNsCount(), modifier);
    auto sortedMnsNotUsedAtH = MnsNotUsedAtH.CalculateQuorum(MnsNotUsedAtH.GetAllMNsCount(), modifier);
    auto sortedCombinedMnsList = std::vector<CDeterministicMNCPtr>();
    std::copy(sortedMnsNotUsedAtH.begin(),
              sortedMnsNotUsedAtH.end(),
              std::back_inserter(sortedCombinedMnsList));
    std::copy(sortedMnsUsedAtHM.begin(),
              sortedMnsUsedAtHM.end(),
              std::back_inserter(sortedCombinedMnsList));

    CQuorumSnapshot quorumSnapshot = {};

    CLLMQUtils::BuildQuorumSnapshot(llmqType, Mns, MnsUsedAtH, sortedCombinedMnsList, quarterQuorumMembers, quorumSnapshot);

    quorumSnapshotManager->StoreSnapshotForBlock(llmqType, pindexQuorum, quorumSnapshot);

    return quarterQuorumMembers;
}

std::vector<CDeterministicMNCPtr> CLLMQUtils::GetQuorumQuarterMembersBySnapshot(Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum, CQuorumSnapshot& snapshot)
{
    std::vector<CDeterministicMNCPtr> quarterQuorumMembers;

    auto modifier = ::SerializeHash(std::make_pair(llmqType, pindexQuorum->GetBlockHash()));
    auto Mns = deterministicMNManager->GetListForBlock(pindexQuorum);
    auto quarterSize = GetLLMQParams(llmqType).size / 4;
    auto MnsUsedAtH = CDeterministicMNList();
    auto MnsNotUsedAtH = CDeterministicMNList();

    size_t  index = {};
    Mns.ForEachMN(false, [&index, &snapshot, &MnsUsedAtH](const CDeterministicMNCPtr& dmn){
        if(snapshot.activeQuorumMembers.at(index)) {
            MnsUsedAtH.AddMN(dmn);
        }
        index++;
    });
    Mns.ForEachMN(false, [&MnsNotUsedAtH, &MnsUsedAtH](const CDeterministicMNCPtr& dmn){
        if(!MnsUsedAtH.ContainsMN(dmn->proTxHash)) {
            MnsNotUsedAtH.AddMN(dmn);
        }
    });

    auto sortedMnsUsedAtH = MnsUsedAtH.CalculateQuorum(Mns.GetAllMNsCount(), modifier);
    auto sortedMnsNotUsedAtH = MnsNotUsedAtH.CalculateQuorum(Mns.GetAllMNsCount(), modifier);
    auto sortedCombinedMnsList = std::vector<CDeterministicMNCPtr>();
    std::copy(sortedMnsNotUsedAtH.begin(),
              sortedMnsNotUsedAtH.end(),
              std::back_inserter(sortedCombinedMnsList));
    std::copy(sortedMnsUsedAtH.begin(),
              sortedMnsUsedAtH.end(),
              std::back_inserter(sortedCombinedMnsList));

    //Mode 0: No skipping
    if(snapshot.mnSkipListMode == 0){
        std::copy_n(sortedCombinedMnsList.begin(),
                    quarterSize,
                    std::back_inserter(quarterQuorumMembers));
    }
    //Mode 1: List holds entries to be skipped
    else if(snapshot.mnSkipListMode == 1){
        std::set<uint256> mnProTxHashToRemove;
        size_t first_entry_index = {};
        for(const auto& s : snapshot.mnSkipList) {
            if(first_entry_index == 0){
                first_entry_index = s;
                mnProTxHashToRemove.insert(sortedCombinedMnsList.at(s)->proTxHash);
            }
            else {
                mnProTxHashToRemove.insert(sortedCombinedMnsList.at(first_entry_index + s)->proTxHash);
            }
        }
        auto itm = std::stable_partition(sortedCombinedMnsList.begin(),
                                         sortedCombinedMnsList.end(),
                                         [&mnProTxHashToRemove](const CDeterministicMNCPtr& dmn){
                                             return mnProTxHashToRemove.find(dmn->proTxHash) == mnProTxHashToRemove.end();
                                         });
        sortedCombinedMnsList.erase(itm, sortedCombinedMnsList.end());
        std::copy_n(sortedCombinedMnsList.begin(),
                    quarterSize,
                    std::back_inserter(quarterQuorumMembers));
    }
    //Mode 2: List holds entries to be kept
    else if(snapshot.mnSkipListMode == 2) {
        std::set<uint256> mnProTxHashToKeep;
        size_t first_entry_index = {};
        for(const auto& s : snapshot.mnSkipList) {
            if(first_entry_index == 0){
                first_entry_index = s;
                mnProTxHashToKeep.insert(sortedCombinedMnsList.at(s)->proTxHash);
            }
            else {
                mnProTxHashToKeep.insert(sortedCombinedMnsList.at(first_entry_index + s)->proTxHash);
            }
        }
        auto itm = std::stable_partition(sortedCombinedMnsList.begin(),
                                         sortedCombinedMnsList.end(),
                                         [&mnProTxHashToKeep](const CDeterministicMNCPtr& dmn){
                                             return mnProTxHashToKeep.find(dmn->proTxHash) != mnProTxHashToKeep.end();
                                         });
        sortedCombinedMnsList.erase(itm, sortedCombinedMnsList.end());
        std::copy_n(sortedCombinedMnsList.begin(),
                    quarterSize,
                    std::back_inserter(quarterQuorumMembers));
    }
    //Mode 3: Every node was skipped. Returning empty quarterQuorumMembers

    return quarterQuorumMembers;
}

void CLLMQUtils::BuildQuorumSnapshot(Consensus::LLMQType llmqType, const CDeterministicMNList& mnAtH, const CDeterministicMNList& mnUsedAtH, const std::vector<CDeterministicMNCPtr>& sortedCombinedMns, std::vector<CDeterministicMNCPtr>& quarterMembers, CQuorumSnapshot& quorumSnapshot)
{
    quorumSnapshot.activeQuorumMembers.resize(mnAtH.GetAllMNsCount());
    std::fill(quorumSnapshot.activeQuorumMembers.begin(),
              quorumSnapshot.activeQuorumMembers.end(),
              false);
    size_t  index = {};
    mnAtH.ForEachMN(false, [&index, &quorumSnapshot, &mnUsedAtH](const CDeterministicMNCPtr& dmn){
        if(mnUsedAtH.ContainsMN(dmn->proTxHash)){
            quorumSnapshot.activeQuorumMembers.at(index) = true;
        }
        index++;
    });

    CLLMQUtils::BuildQuorumSnapshotSkipList(llmqType, mnUsedAtH, sortedCombinedMns, quarterMembers, quorumSnapshot);

}

void CLLMQUtils::BuildQuorumSnapshotSkipList(Consensus::LLMQType llmqType, const CDeterministicMNList& mnUsedAtH, const std::vector<CDeterministicMNCPtr>& sortedCombinedMns, std::vector<CDeterministicMNCPtr>& quarterMembers, CQuorumSnapshot& quorumSnapshot)
{
    auto quarterSize = GetLLMQParams(llmqType).size / 4;

    quarterMembers.clear();

    if(std::all_of(sortedCombinedMns.begin(),
                   sortedCombinedMns.begin() + quarterSize,
                   [&mnUsedAtH](const CDeterministicMNCPtr& dmn){
                       return !mnUsedAtH.ContainsMN(dmn->proTxHash);
                   })) {
        //Mode 0: No skipping
        quorumSnapshot.mnSkipListMode = 0;
        quorumSnapshot.mnSkipList.clear();

        std::copy_n(sortedCombinedMns.begin(),
                    quarterSize,
                    std::back_inserter(quarterMembers));
    }
    else {
        //TODO Determine if mode 2 is more optimal
        //Mode 1: Skipping entries
        quorumSnapshot.mnSkipListMode = 1;

        size_t first_entry_index = {};
        size_t i = {};
        while(quarterMembers.size() < quarterSize && i < sortedCombinedMns.size()) {
            if(mnUsedAtH.ContainsMN(sortedCombinedMns.at(i)->proTxHash)) {
                if(first_entry_index == 0) {
                    first_entry_index = i;
                    quorumSnapshot.mnSkipList.push_back(i);
                }
                else {
                    quorumSnapshot.mnSkipList.push_back(first_entry_index - i);
                }
            }
            else {
                quarterMembers.push_back(sortedCombinedMns.at(i));
            }
            i++;
        }

        //Not enough quorums selected to form the new quarter...
        if(quarterMembers.size() < quarterSize) {
            quorumSnapshot.mnSkipListMode = 3;
            quarterMembers.clear();
        }
    }
}

uint256 CLLMQUtils::BuildCommitmentHash(Consensus::LLMQType llmqType, const uint256& blockHash, const std::vector<bool>& validMembers, const CBLSPublicKey& pubKey, const uint256& vvecHash)
{
    CHashWriter hw(SER_NETWORK, 0);
    hw << llmqType;
    hw << blockHash;
    hw << DYNBITSET(validMembers);
    hw << pubKey;
    hw << vvecHash;
    return hw.GetHash();
}

uint256 CLLMQUtils::BuildSignHash(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& id, const uint256& msgHash)
{
    CHashWriter h(SER_GETHASH, 0);
    h << llmqType;
    h << quorumHash;
    h << id;
    h << msgHash;
    return h.GetHash();
}

static bool EvalSpork(Consensus::LLMQType llmqType, int64_t spork_value)
{
    if (spork_value == 0) {
        return true;
    }
    if (spork_value == 1 && llmqType != Consensus::LLMQType::LLMQ_100_67 && llmqType != Consensus::LLMQType::LLMQ_400_60 && llmqType != Consensus::LLMQType::LLMQ_400_85) {
        return true;
    }
    return false;
}

bool CLLMQUtils::IsAllMembersConnectedEnabled(Consensus::LLMQType llmqType)
{
    return EvalSpork(llmqType, sporkManager.GetSporkValue(SPORK_21_QUORUM_ALL_CONNECTED));
}

bool CLLMQUtils::IsQuorumPoseEnabled(Consensus::LLMQType llmqType)
{
    return EvalSpork(llmqType, sporkManager.GetSporkValue(SPORK_23_QUORUM_POSE));
}

uint256 CLLMQUtils::DeterministicOutboundConnection(const uint256& proTxHash1, const uint256& proTxHash2)
{
    // We need to deterministically select who is going to initiate the connection. The naive way would be to simply
    // return the min(proTxHash1, proTxHash2), but this would create a bias towards MNs with a numerically low
    // hash. To fix this, we return the proTxHash that has the lowest value of:
    //   hash(min(proTxHash1, proTxHash2), max(proTxHash1, proTxHash2), proTxHashX)
    // where proTxHashX is the proTxHash to compare
    uint256 h1;
    uint256 h2;
    if (proTxHash1 < proTxHash2) {
        h1 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash2));
    } else {
        h1 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash2));
    }
    if (h1 < h2) {
        return proTxHash1;
    }
    return proTxHash2;
}

std::set<uint256> CLLMQUtils::GetQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound)
{
    if (IsAllMembersConnectedEnabled(llmqParams.type)) {
        auto mns = GetAllQuorumMembers(llmqParams, pQuorumBaseBlockIndex);
        std::set<uint256> result;

        for (const auto& dmn : mns) {
            if (dmn->proTxHash == forMember) {
                continue;
            }
            // Determine which of the two MNs (forMember vs dmn) should initiate the outbound connection and which
            // one should wait for the inbound connection. We do this in a deterministic way, so that even when we
            // end up with both connecting to each other, we know which one to disconnect
            uint256 deterministicOutbound = DeterministicOutboundConnection(forMember, dmn->proTxHash);
            if (!onlyOutbound || deterministicOutbound == dmn->proTxHash) {
                result.emplace(dmn->proTxHash);
            }
        }
        return result;
    } else {
        return GetQuorumRelayMembers(llmqParams, pQuorumBaseBlockIndex, forMember, onlyOutbound);
    }
}

std::set<uint256> CLLMQUtils::GetQuorumRelayMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex *pQuorumBaseBlockIndex, const uint256 &forMember, bool onlyOutbound)
{
    auto mns = GetAllQuorumMembers(llmqParams, pQuorumBaseBlockIndex);
    std::set<uint256> result;

    auto calcOutbound = [&](size_t i, const uint256& proTxHash) {
        // Relay to nodes at indexes (i+2^k)%n, where
        //   k: 0..max(1, floor(log2(n-1))-1)
        //   n: size of the quorum/ring
        std::set<uint256> r;
        int gap = 1;
        int gap_max = (int)mns.size() - 1;
        int k = 0;
        while ((gap_max >>= 1) || k <= 1) {
            size_t idx = (i + gap) % mns.size();
            const auto& otherDmn = mns[idx];
            if (otherDmn->proTxHash == proTxHash) {
                continue;
            }
            r.emplace(otherDmn->proTxHash);
            gap <<= 1;
            k++;
        }
        return r;
    };

    for (size_t i = 0; i < mns.size(); i++) {
        const auto& dmn = mns[i];
        if (dmn->proTxHash == forMember) {
            auto r = calcOutbound(i, dmn->proTxHash);
            result.insert(r.begin(), r.end());
        } else if (!onlyOutbound) {
            auto r = calcOutbound(i, dmn->proTxHash);
            if (r.count(forMember)) {
                result.emplace(dmn->proTxHash);
            }
        }
    }

    return result;
}

std::set<size_t> CLLMQUtils::CalcDeterministicWatchConnections(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, size_t memberCount, size_t connectionCount)
{
    static uint256 qwatchConnectionSeed;
    static std::atomic<bool> qwatchConnectionSeedGenerated{false};
    static CCriticalSection qwatchConnectionSeedCs;
    if (!qwatchConnectionSeedGenerated) {
        LOCK(qwatchConnectionSeedCs);
        qwatchConnectionSeed = GetRandHash();
        qwatchConnectionSeedGenerated = true;
    }

    std::set<size_t> result;
    uint256 rnd = qwatchConnectionSeed;
    for (size_t i = 0; i < connectionCount; i++) {
        rnd = ::SerializeHash(std::make_pair(rnd, std::make_pair(llmqType, pQuorumBaseBlockIndex->GetBlockHash())));
        result.emplace(rnd.GetUint64(0) % memberCount);
    }
    return result;
}

bool CLLMQUtils::EnsureQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& myProTxHash)
{
    auto members = GetAllQuorumMembers(llmqParams, pQuorumBaseBlockIndex);
    bool isMember = std::find_if(members.begin(), members.end(), [&](const auto& dmn) { return dmn->proTxHash == myProTxHash; }) != members.end();

    if (!isMember && !CLLMQUtils::IsWatchQuorumsEnabled()) {
        return false;
    }

    std::set<uint256> connections;
    std::set<uint256> relayMembers;
    if (isMember) {
        connections = CLLMQUtils::GetQuorumConnections(llmqParams, pQuorumBaseBlockIndex, myProTxHash, true);
        relayMembers = CLLMQUtils::GetQuorumRelayMembers(llmqParams, pQuorumBaseBlockIndex, myProTxHash, true);
    } else {
        auto cindexes = CLLMQUtils::CalcDeterministicWatchConnections(llmqParams.type, pQuorumBaseBlockIndex, members.size(), 1);
        for (auto idx : cindexes) {
            connections.emplace(members[idx]->proTxHash);
        }
        relayMembers = connections;
    }
    if (!connections.empty()) {
        if (!g_connman->HasMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash()) && LogAcceptCategory(BCLog::LLMQ)) {
            auto mnList = deterministicMNManager->GetListAtChainTip();
            std::string debugMsg = strprintf("CLLMQUtils::%s -- adding masternodes quorum connections for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
            for (auto& c : connections) {
                auto dmn = mnList.GetValidMN(c);
                if (!dmn) {
                    debugMsg += strprintf("  %s (not in valid MN set anymore)\n", c.ToString());
                } else {
                    debugMsg += strprintf("  %s (%s)\n", c.ToString(), dmn->pdmnState->addr.ToString(false));
                }
            }
            LogPrint(BCLog::NET_NETCONN, debugMsg.c_str()); /* Continued */
        }
        g_connman->SetMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), connections);
    }
    if (!relayMembers.empty()) {
        g_connman->SetMasternodeQuorumRelayMembers(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), relayMembers);
    }
    return true;
}

void CLLMQUtils::AddQuorumProbeConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex *pQuorumBaseBlockIndex, const uint256 &myProTxHash)
{
    if (!CLLMQUtils::IsQuorumPoseEnabled(llmqParams.type)) {
        return;
    }

    auto members = GetAllQuorumMembers(llmqParams, pQuorumBaseBlockIndex);
    auto curTime = GetAdjustedTime();

    std::set<uint256> probeConnections;
    for (const auto& dmn : members) {
        if (dmn->proTxHash == myProTxHash) {
            continue;
        }
        auto lastOutbound = mmetaman.GetMetaInfo(dmn->proTxHash)->GetLastOutboundSuccess();
        // re-probe after 50 minutes so that the "good connection" check in the DKG doesn't fail just because we're on
        // the brink of timeout
        if (curTime - lastOutbound > 50 * 60) {
            probeConnections.emplace(dmn->proTxHash);
        }
    }

    if (!probeConnections.empty()) {
        if (LogAcceptCategory(BCLog::LLMQ)) {
            auto mnList = deterministicMNManager->GetListAtChainTip();
            std::string debugMsg = strprintf("CLLMQUtils::%s -- adding masternodes probes for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
            for (auto& c : probeConnections) {
                auto dmn = mnList.GetValidMN(c);
                if (!dmn) {
                    debugMsg += strprintf("  %s (not in valid MN set anymore)\n", c.ToString());
                } else {
                    debugMsg += strprintf("  %s (%s)\n", c.ToString(), dmn->pdmnState->addr.ToString(false));
                }
            }
            LogPrint(BCLog::NET_NETCONN, debugMsg.c_str()); /* Continued */
        }
        g_connman->AddPendingProbeConnections(probeConnections);
    }
}

bool CLLMQUtils::IsQuorumActive(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    // sig shares and recovered sigs are only accepted from recent/active quorums
    // we allow one more active quorum as specified in consensus, as otherwise there is a small window where things could
    // fail while we are on the brink of a new quorum
    auto quorums = quorumManager->ScanQuorums(llmqType, GetLLMQParams(llmqType).signingActiveQuorumCount + 1);
    return ranges::any_of(quorums, [&quorumHash](const auto& q){ return q->qc->quorumHash == quorumHash; });
}

bool CLLMQUtils::IsQuorumTypeEnabled(Consensus::LLMQType llmqType, const CBlockIndex* pindex)
{
    const Consensus::Params& consensusParams = Params().GetConsensus();

    switch (llmqType)
    {
        case Consensus::LLMQType::LLMQ_50_60:
        case Consensus::LLMQType::LLMQ_400_60:
        case Consensus::LLMQType::LLMQ_400_85:
            break;
        case Consensus::LLMQType::LLMQ_100_67:
        case Consensus::LLMQType::LLMQ_TEST_V17:
            if (LOCK(cs_llmq_vbc); VersionBitsState(pindex, consensusParams, Consensus::DEPLOYMENT_DIP0020, llmq_versionbitscache) != ThresholdState::ACTIVE) {
                return false;
            }
            break;
        case Consensus::LLMQType::LLMQ_TEST:
        case Consensus::LLMQType::LLMQ_DEVNET:
            break;
        default:
            throw std::runtime_error(strprintf("%s: Unknown LLMQ type %d", __func__, static_cast<uint8_t>(llmqType)));
    }

    return true;
}

std::vector<Consensus::LLMQType> CLLMQUtils::GetEnabledQuorumTypes(const CBlockIndex* pindex)
{
    std::vector<Consensus::LLMQType> ret;
    ret.reserve(Params().GetConsensus().llmqs.size());
    for (const auto& params : Params().GetConsensus().llmqs) {
        if (IsQuorumTypeEnabled(params.type, pindex)) {
            ret.push_back(params.type);
        }
    }
    return ret;
}

std::vector<std::reference_wrapper<const Consensus::LLMQParams>> CLLMQUtils::GetEnabledQuorumParams(const CBlockIndex* pindex)
{
    std::vector<std::reference_wrapper<const Consensus::LLMQParams>> ret;
    ret.reserve(Params().GetConsensus().llmqs.size());

    std::copy_if(Params().GetConsensus().llmqs.begin(), Params().GetConsensus().llmqs.end(), std::back_inserter(ret),
                 [&pindex](const auto& params){return IsQuorumTypeEnabled(params.type, pindex);});

    return ret;
}

bool CLLMQUtils::QuorumDataRecoveryEnabled()
{
    return gArgs.GetBoolArg("-llmq-data-recovery", DEFAULT_ENABLE_QUORUM_DATA_RECOVERY);
}

bool CLLMQUtils::IsWatchQuorumsEnabled()
{
    static bool fIsWatchQuroumsEnabled = gArgs.GetBoolArg("-watchquorums", DEFAULT_WATCH_QUORUMS);
    return fIsWatchQuroumsEnabled;
}

std::map<Consensus::LLMQType, QvvecSyncMode> CLLMQUtils::GetEnabledQuorumVvecSyncEntries()
{
    std::map<Consensus::LLMQType, QvvecSyncMode> mapQuorumVvecSyncEntries;
    for (const auto& strEntry : gArgs.GetArgs("-llmq-qvvec-sync")) {
        Consensus::LLMQType llmqType = Consensus::LLMQType::LLMQ_NONE;
        QvvecSyncMode mode{QvvecSyncMode::Invalid};
        std::istringstream ssEntry(strEntry);
        std::string strLLMQType, strMode, strTest;
        const bool fLLMQTypePresent = std::getline(ssEntry, strLLMQType, ':') && strLLMQType != "";
        const bool fModePresent = std::getline(ssEntry, strMode, ':') && strMode != "";
        const bool fTooManyEntries = static_cast<bool>(std::getline(ssEntry, strTest, ':'));
        if (!fLLMQTypePresent || !fModePresent || fTooManyEntries) {
            throw std::invalid_argument(strprintf("Invalid format in -llmq-qvvec-sync: %s", strEntry));
        }

        if (auto optLLMQParams = ranges::find_if_opt(Params().GetConsensus().llmqs,
                                                     [&strLLMQType](const auto& params){return params.name == strLLMQType;})) {
            llmqType = optLLMQParams->type;
        } else {
            throw std::invalid_argument(strprintf("Invalid llmqType in -llmq-qvvec-sync: %s", strEntry));
        }
        if (mapQuorumVvecSyncEntries.count(llmqType) > 0) {
            throw std::invalid_argument(strprintf("Duplicated llmqType in -llmq-qvvec-sync: %s", strEntry));
        }

        int32_t nMode;
        if (ParseInt32(strMode, &nMode)) {
            switch (nMode) {
            case (int32_t)QvvecSyncMode::Always:
                mode = QvvecSyncMode::Always;
                break;
            case (int32_t)QvvecSyncMode::OnlyIfTypeMember:
                mode = QvvecSyncMode::OnlyIfTypeMember;
                break;
            default:
                mode = QvvecSyncMode::Invalid;
                break;
            }
        }
        if (mode == QvvecSyncMode::Invalid) {
            throw std::invalid_argument(strprintf("Invalid mode in -llmq-qvvec-sync: %s", strEntry));
        }
        mapQuorumVvecSyncEntries.emplace(llmqType, mode);
    }
    return mapQuorumVvecSyncEntries;
}

template <typename CacheType>
void CLLMQUtils::InitQuorumsCache(CacheType& cache)
{
    for (auto& llmq : Params().GetConsensus().llmqs) {
        cache.emplace(std::piecewise_construct, std::forward_as_tuple(llmq.type),
                      std::forward_as_tuple(llmq.signingActiveQuorumCount + 1));
    }
}
template void CLLMQUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>& cache);
template void CLLMQUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>& cache);
template void CLLMQUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>&);

const Consensus::LLMQParams& GetLLMQParams(Consensus::LLMQType llmqType)
{
    return Params().GetLLMQ(llmqType);
}

} // namespace llmq
