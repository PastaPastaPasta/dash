// Copyright (c) 2017-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <evo/simplifiedmns.h>
#include <llmq/quorums.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/quorums_snapshot.h>

#include <evo/specialtx.h>

#include <serialize.h>
#include <version.h>

#include <base58.h>
#include <chainparams.h>
#include <univalue.h>
#include <validation.h>


static const std::string DB_QUORUM_SNAPSHOT = "llmq_S";

void CQuorumSnapshot::ToJson(UniValue& obj) const
{
    obj.setObject();

    obj.pushKV("creationHeight", creationHeight);

    CDataStream ssMnListDiff(SER_NETWORK, PROTOCOL_VERSION);

    ssMnListDiff << mnListDiffTip;
    obj.pushKV("mnListDiffTip", HexStr(ssMnListDiff));

    ssMnListDiff.clear();
    ssMnListDiff << mnListDiffH;
    obj.pushKV("mnListDiffH", HexStr(ssMnListDiff));

    ssMnListDiff.clear();
    ssMnListDiff << mnListDiffH_C;
    obj.pushKV("mnListDiffH_C", HexStr(ssMnListDiff));

    ssMnListDiff.clear();
    ssMnListDiff << mnListDiffH_2C;
    obj.pushKV("mnListDiffH_2C", HexStr(ssMnListDiff));

    ssMnListDiff.clear();
    ssMnListDiff << mnListDiffH_3C;
    obj.pushKV("mnListDiffH_3C", HexStr(ssMnListDiff));

    UniValue activeQMHC(UniValue::VARR);
    for (const auto& h : activeQuorumMembersH_C) {
        activeQMHC.push_back(h);
    }
    obj.pushKV("activeQuorumMembersH_C", activeQMHC);

    UniValue activeQMH2C(UniValue::VARR);
    for (const auto& h : activeQuorumMembersH_2C) {
        activeQMH2C.push_back(h);
    }
    obj.pushKV("activeQuorumMembersH_2C", activeQMH2C);

    UniValue activeQMH3C(UniValue::VARR);
    for (const auto& h : activeQuorumMembersH_3C) {
        activeQMH3C.push_back(h);
    }
    obj.pushKV("activeQuorumMembersH_3C", activeQMH3C);
}

bool CQuorumSnapshot::BuildQuorumMembersBitSet(const CDeterministicMNList& mnList, const llmq::CQuorumCPtr qu, std::vector<bool>& activeQuorumMembers)
{
    activeQuorumMembers.resize(mnList.GetAllMNsCount());

    std::fill(activeQuorumMembers.begin(), activeQuorumMembers.end(), false);

    size_t      index = {};

    mnList.ForEachMN(false, [&index, &qu, &activeQuorumMembers](const CDeterministicMNCPtr& dmn){
       if(qu->IsMember(dmn->proTxHash)) {
           activeQuorumMembers.at(index) = true;
       }
       index++;
    });

    return true;
}

bool BuildQuorumSnapshot(const uint16_t heightsNb, const std::vector<uint16_t>& knownHeights, CQuorumSnapshot& quorumSnapshotRet, std::string& errorRet)
{
    AssertLockHeld(cs_main);

    if(heightsNb > 4) {
        errorRet = strprintf("invalid requested heightsNb");
        return false;
    }

    if(heightsNb != knownHeights.size()) {
        errorRet = strprintf("missmatch requested heightsNb and size(knownHeights)");
        return false;
    }

    LOCK(deterministicMNManager->cs);

    //TODO Handle request if client knows some heights
    if(heightsNb == 0){
        const CBlockIndex* baseBlockIndex = chainActive.Genesis();
        if (!baseBlockIndex) {
            errorRet = strprintf("genesis block not found");
            return false;
        }

        const CBlockIndex* tipBlockIndex = chainActive.Tip();
        if (!tipBlockIndex) {
            errorRet = strprintf("tip block not found");
            return false;
        }

        auto baseDmnList = deterministicMNManager->GetListForBlock(baseBlockIndex);
        auto tipMnList = deterministicMNManager->GetListForBlock(tipBlockIndex);

        quorumSnapshotRet.mnListDiffTip = baseDmnList.BuildSimplifiedDiff(tipMnList);

        auto quorums = llmq::quorumBlockProcessor->GetMinedAndActiveCommitmentsUntilBlock(tipBlockIndex);

        auto instantSendQuorum = quorums.find(Params().GetConsensus().llmqTypeInstantSend);
        if(instantSendQuorum == quorums.end()) {
            errorRet = strprintf("No InstantSend quorum found");
            return false;
        }

        if(instantSendQuorum->second.empty()){
            errorRet = strprintf("Empty list for InstantSend quorum");
            return false;
        }

        // Since the returned quorums are in reversed order, the most recent one is at index 0
        const CBlockIndex* hBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(0)->GetBlockHash());
        if(!hBlockIndex) {
            errorRet = strprintf("Can not find block H");
            return false;
        }
        auto HDmnList = deterministicMNManager->GetListForBlock(hBlockIndex);
        quorumSnapshotRet.mnListDiffH = baseDmnList.BuildSimplifiedDiff(HDmnList);
        quorumSnapshotRet.creationHeight = hBlockIndex->nHeight;

        // H-C
        const CBlockIndex* hcBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(1)->GetBlockHash());
        if(!hcBlockIndex) {
            errorRet = strprintf("Can not find block H-C");
            return false;
        }
        auto HCDmnList = deterministicMNManager->GetListForBlock(hcBlockIndex);
        quorumSnapshotRet.mnListDiffH_C = baseDmnList.BuildSimplifiedDiff(HCDmnList);
        llmq::CQuorumCPtr quHC = llmq::quorumManager->GetQuorum(Params().GetConsensus().llmqTypeInstantSend, instantSendQuorum->second.at(1)->GetBlockHash());
        CQuorumSnapshot::BuildQuorumMembersBitSet(HCDmnList, quHC, quorumSnapshotRet.activeQuorumMembersH_C);

        // H-2C
        const CBlockIndex* h2cBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(2)->GetBlockHash());
        if(!h2cBlockIndex) {
            errorRet = strprintf("Can not find block H-2C");
            return false;
        }
        auto H2CDmnList = deterministicMNManager->GetListForBlock(h2cBlockIndex);
        quorumSnapshotRet.mnListDiffH_2C = baseDmnList.BuildSimplifiedDiff(H2CDmnList);
        llmq::CQuorumCPtr quH2C = llmq::quorumManager->GetQuorum(Params().GetConsensus().llmqTypeInstantSend, instantSendQuorum->second.at(2)->GetBlockHash());
        CQuorumSnapshot::BuildQuorumMembersBitSet(H2CDmnList, quH2C, quorumSnapshotRet.activeQuorumMembersH_2C);

        // H-3C
        const CBlockIndex* h3cBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(3)->GetBlockHash());
        if(!h3cBlockIndex) {
            errorRet = strprintf("Can not find block H-3C");
            return false;
        }
        auto H3CDmnList = deterministicMNManager->GetListForBlock(h3cBlockIndex);
        quorumSnapshotRet.mnListDiffH_3C = baseDmnList.BuildSimplifiedDiff(H3CDmnList);
        llmq::CQuorumCPtr quH3C = llmq::quorumManager->GetQuorum(Params().GetConsensus().llmqTypeInstantSend, instantSendQuorum->second.at(3)->GetBlockHash());
        CQuorumSnapshot::BuildQuorumMembersBitSet(H3CDmnList, quH3C, quorumSnapshotRet.activeQuorumMembersH_3C);


    }

    //In case of successful build, write it in evoDb for future use
    //TODO handle read from evoDB before re-building it
    evoDb->Write(std::make_pair(DB_QUORUM_SNAPSHOT, std::make_pair(Params().GetConsensus().llmqTypeInstantSend, quorumSnapshotRet.creationHeight)), quorumSnapshotRet);

    return true;
}