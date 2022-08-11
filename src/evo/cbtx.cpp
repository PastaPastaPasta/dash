// Copyright (c) 2017-2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <evo/simplifiedmns.h>
#include <evo/specialtx.h>
#include <consensus/validation.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>

bool CheckCbTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    if (tx.nType != TRANSACTION_COINBASE) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-type");
    }

    if (!tx.IsCoinBase()) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-invalid");
    }

    CCbTx cbTx;
    if (!GetTxPayload(tx, cbTx)) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-payload");
    }

    if (cbTx.nVersion == 0 || cbTx.nVersion > CCbTx::CURRENT_VERSION) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-version");
    }

    if (pindexPrev != nullptr && pindexPrev->nHeight + 1 != cbTx.nHeight) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-height");
    }

    if (pindexPrev != nullptr) {
        bool fDIP0008Active = pindexPrev->nHeight >= Params().GetConsensus().DIP0008Height;
        if (fDIP0008Active && cbTx.nVersion < 2) {
            return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-version");
        }
    }

    return true;
}

// This can only be done after the block has been fully processed, as otherwise we won't have the finished MN list
bool CheckCbTxMerkleRoots(const CBlock& block, const CBlockIndex* pindex, CValidationState& state, const CCoinsViewCache& view)
{
    if (block.vtx[0]->nType != TRANSACTION_COINBASE) {
        return true;
    }

    static std::chrono::microseconds nTimePayload = std::chrono::microseconds(0);

    const auto nTime1 = std::chrono::microseconds(GetTimeMicros());

    CCbTx cbTx;
    if (!GetTxPayload(*block.vtx[0], cbTx)) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-payload");
    }

    const auto nTime2 = std::chrono::microseconds(GetTimeMicros());
    nTimePayload += nTime2 - nTime1;
    LogPrint(BCLog::BENCHMARK, "          - GetTxPayload: %.2fms [%.2fs]\n",
             std::chrono::duration_cast<std::chrono::milliseconds>(nTime2 - nTime1).count(),
             std::chrono::duration_cast<std::chrono::seconds>(nTimePayload).count());

    if (pindex != nullptr) {
        static auto nTimeMerkleMNL = std::chrono::microseconds(0);
        static auto nTimeMerkleQuorum = std::chrono::microseconds(0);

        uint256 calculatedMerkleRoot;
        if (!CalcCbTxMerkleRootMNList(block, pindex->pprev, calculatedMerkleRoot, state, view)) {
            // pass the state returned by the function above
            return false;
        }
        if (calculatedMerkleRoot != cbTx.merkleRootMNList) {
            return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-mnmerkleroot");
        }

        const auto nTime3 = std::chrono::microseconds(GetTimeMicros());
        nTimeMerkleMNL += nTime3 - nTime2;
        LogPrint(BCLog::BENCHMARK, "          - CalcCbTxMerkleRootMNList: %.2fms [%.2fs]\n",
                    std::chrono::duration_cast<std::chrono::milliseconds>(nTime3 - nTime2).count(),
                    std::chrono::duration_cast<std::chrono::seconds>(nTimeMerkleMNL).count());

        if (cbTx.nVersion >= 2) {
            if (!CalcCbTxMerkleRootQuorums(block, pindex->pprev, calculatedMerkleRoot, state)) {
                // pass the state returned by the function above
                return false;
            }
            if (calculatedMerkleRoot != cbTx.merkleRootQuorums) {
                return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-cbtx-quorummerkleroot");
            }
        }

        const auto nTime4 = std::chrono::microseconds(GetTimeMicros());
        nTimeMerkleQuorum += nTime4 - nTime3;
        LogPrint(BCLog::BENCHMARK, "          - CalcCbTxMerkleRootQuorums: %.2fms [%.2fs]\n",
                    std::chrono::duration_cast<std::chrono::milliseconds>(nTime4 - nTime3).count(),
                    std::chrono::duration_cast<std::chrono::seconds>(nTimeMerkleQuorum).count());
    }

    return true;
}

bool CalcCbTxMerkleRootMNList(const CBlock& block, const CBlockIndex* pindexPrev, uint256& merkleRootRet, CValidationState& state, const CCoinsViewCache& view)
{
    LOCK(deterministicMNManager->cs);

    try {
        static auto nTimeDMN = std::chrono::microseconds(0);
        static auto nTimeSMNL = std::chrono::microseconds(0);
        static auto nTimeMerkle = std::chrono::microseconds(0);

        const auto nTime1 = std::chrono::microseconds(GetTimeMicros());

        CDeterministicMNList tmpMNList;
        if (!deterministicMNManager->BuildNewListFromBlock(block, pindexPrev, state, view, tmpMNList, false)) {
            // pass the state returned by the function above
            return false;
        }

        const auto nTime2 = std::chrono::microseconds(GetTimeMicros());
        nTimeDMN += nTime2 - nTime1;
        LogPrint(BCLog::BENCHMARK, "            - BuildNewListFromBlock: %.2fms [%.2fs]\n",
                    std::chrono::duration_cast<std::chrono::milliseconds>(nTime2 - nTime1).count(),
                    std::chrono::duration_cast<std::chrono::seconds>(nTimeDMN).count());

        CSimplifiedMNList sml(tmpMNList);

        const auto nTime3 = std::chrono::microseconds(GetTimeMicros());
        nTimeSMNL += nTime3 - nTime2;
        LogPrint(BCLog::BENCHMARK, "            - CSimplifiedMNList: %.2fms [%.2fs]\n",
                    std::chrono::duration_cast<std::chrono::milliseconds>(nTime3 - nTime2).count(),
                    std::chrono::duration_cast<std::chrono::seconds>(nTimeSMNL).count());

        static CSimplifiedMNList smlCached;
        static uint256 merkleRootCached;
        static bool mutatedCached{false};

        if (sml.mnList == smlCached.mnList) {
            merkleRootRet = merkleRootCached;
            if (mutatedCached) {
                return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "mutated-cached-calc-cb-mnmerkleroot");
            }
            return true;
        }

        bool mutated = false;
        merkleRootRet = sml.CalcMerkleRoot(&mutated);

        const auto nTime4 = std::chrono::microseconds(GetTimeMicros());
        nTimeMerkle += nTime4 - nTime3;
        LogPrint(BCLog::BENCHMARK, "            - CalcMerkleRoot: %.2fms [%.2fs]\n",
                 std::chrono::duration_cast<std::chrono::milliseconds>(nTime4 - nTime3).count(),
                 std::chrono::duration_cast<std::chrono::seconds>(nTimeMerkle).count());

        smlCached = std::move(sml);
        merkleRootCached = merkleRootRet;
        mutatedCached = mutated;

        if (mutated) {
            return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "mutated-calc-cb-mnmerkleroot");
        }

        return true;
    } catch (const std::exception& e) {
        LogPrintf("%s -- failed: %s\n", __func__, e.what());
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "failed-calc-cb-mnmerkleroot");
    }
}

bool CalcCbTxMerkleRootQuorums(const CBlock& block, const CBlockIndex* pindexPrev, uint256& merkleRootRet, CValidationState& state)
{
    static auto nTimeMinedAndActive = std::chrono::microseconds(0);
    static auto nTimeMined = std::chrono::microseconds(0);
    static auto nTimeLoop = std::chrono::microseconds(0);
    static auto nTimeMerkle = std::chrono::microseconds(0);

    const auto nTime1 = std::chrono::microseconds(GetTimeMicros());

    // The returned quorums are in reversed order, so the most recent one is at index 0
    auto quorums = llmq::quorumBlockProcessor->GetMinedAndActiveCommitmentsUntilBlock(pindexPrev);
    std::map<Consensus::LLMQType, std::vector<uint256>> qcHashes;
    std::map<Consensus::LLMQType, std::map<int16_t, uint256>> qcIndexedHashes;
    size_t hashCount = 0;

    const auto nTime2 = std::chrono::microseconds(GetTimeMicros());
    nTimeMinedAndActive += nTime2 - nTime1;
    LogPrint(BCLog::BENCHMARK, "            - GetMinedAndActiveCommitmentsUntilBlock: %.2fms [%.2fs]\n",
                std::chrono::duration_cast<std::chrono::milliseconds>(nTime2 - nTime1).count(),
                std::chrono::duration_cast<std::chrono::seconds>(nTimeMinedAndActive).count());

    for (const auto& p : quorums) {
        auto& v = qcHashes[p.first];
        v.reserve(p.second.size());
        for (const auto& p2 : p.second) {
            uint256 minedBlockHash;
            llmq::CFinalCommitmentPtr qc = llmq::quorumBlockProcessor->GetMinedCommitment(p.first, p2->GetBlockHash(), minedBlockHash);
            if (qc == nullptr) {
                return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "commitment-not-found");
            }
            if (llmq::CLLMQUtils::IsQuorumRotationEnabled(qc->llmqType, pindexPrev)) {
                auto& qi = qcIndexedHashes[p.first];
                qi.insert(std::make_pair(qc->quorumIndex, ::SerializeHash(*qc)));
                continue;
            }
            v.emplace_back(::SerializeHash(*qc));
            hashCount++;
        }
    }


    const auto nTime3 = std::chrono::microseconds(GetTimeMicros());
    nTimeMined += nTime3 - nTime2;
    LogPrint(BCLog::BENCHMARK, "            - GetMinedCommitment: %.2fms [%.2fs]\n",
                std::chrono::duration_cast<std::chrono::milliseconds>(nTime3 - nTime2).count(),
                std::chrono::duration_cast<std::chrono::seconds>(nTimeMined).count());

    // now add the commitments from the current block, which are not returned by GetMinedAndActiveCommitmentsUntilBlock
    // due to the use of pindexPrev (we don't have the tip index here)
    for (size_t i = 1; i < block.vtx.size(); i++) {
        if (const auto& tx = block.vtx[i];
                tx->nVersion == 3 && tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            llmq::CFinalCommitmentTxPayload qc;
            if (!GetTxPayload(*tx, qc)) {
                return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-qc-payload-calc-cbtx-quorummerkleroot");
            }
            if (qc.commitment.IsNull()) {
                continue;
            }
            auto qcHash = ::SerializeHash(qc.commitment);
            const auto& llmq_params = llmq::GetLLMQParams(qc.commitment.llmqType);
            auto& v = qcHashes[llmq_params.type];
            if (llmq::CLLMQUtils::IsQuorumRotationEnabled(qc.commitment.llmqType, pindexPrev)) {
                auto& qi = qcIndexedHashes[qc.commitment.llmqType];
                qi[qc.commitment.quorumIndex] = qcHash;
                continue;
            }
            if (v.size() == size_t(llmq_params.signingActiveQuorumCount)) {
                // we pop the last entry, which is actually the oldest quorum as GetMinedAndActiveCommitmentsUntilBlock
                // returned quorums in reversed order. This pop and later push can only work ONCE, but we rely on the
                // fact that a block can only contain a single commitment for one LLMQ type
                v.pop_back();
            }
            v.emplace_back(qcHash);
            hashCount++;
            if (v.size() > uint64_t(llmq_params.signingActiveQuorumCount)) {
                return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "excess-quorums-calc-cbtx-quorummerkleroot");
            }
        }
    }

    if (!qcIndexedHashes.empty()) {
        for (const auto& q : qcIndexedHashes) {
            auto& v = qcHashes[q.first];
            for (const auto& qq : q.second) {
                v.emplace_back(qq.second);
                hashCount++;
            }
        }
    }

    std::vector<uint256> qcHashesVec;
    qcHashesVec.reserve(hashCount);

    for (const auto& p : qcHashes) {
        // Copy p.second into qcHashesVec
        std::copy(p.second.begin(), p.second.end(), std::back_inserter(qcHashesVec));
    }
    std::sort(qcHashesVec.begin(), qcHashesVec.end());

    const auto nTime4 = std::chrono::microseconds(GetTimeMicros());
    nTimeLoop += nTime4 - nTime3;
    LogPrint(BCLog::BENCHMARK, "            - Loop: %.2fms [%.2fs]\n",
                std::chrono::duration_cast<std::chrono::milliseconds>(nTime4 - nTime3).count()
             ,  std::chrono::duration_cast<std::chrono::seconds>(nTimeLoop).count());

    bool mutated = false;
    merkleRootRet = ComputeMerkleRoot(qcHashesVec, &mutated);

    const auto nTime5 = std::chrono::microseconds(GetTimeMicros());
    nTimeMerkle += nTime5 - nTime4;
    LogPrint(BCLog::BENCHMARK, "            - ComputeMerkleRoot: %.2fms [%.2fs]\n",
                std::chrono::duration_cast<std::chrono::milliseconds>(nTime5 - nTime4).count(),
                std::chrono::duration_cast<std::chrono::seconds>(nTimeMerkle).count());

    if (mutated) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "mutated-calc-cbtx-quorummerkleroot");
    }

    return true;
}

std::string CCbTx::ToString() const
{
    return strprintf("CCbTx(nVersion=%d, nHeight=%d, merkleRootMNList=%s, merkleRootQuorums=%s)",
        nVersion, nHeight, merkleRootMNList.ToString(), merkleRootQuorums.ToString());
}
