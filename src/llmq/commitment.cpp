// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/commitment.h>

#include <evo/deterministicmnprimitive.h>
#include <evo/specialtx.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <logging.h>
#include <validation.h>

namespace llmq
{

CFinalCommitment::CFinalCommitment(const Consensus::LLMQParams& params, const uint256& _quorumHash) :
        llmqType(params.type),
        quorumHash(_quorumHash),
        quorumIndex(0),
        signers(params.size),
        validMembers(params.size)
{
}

#define LogPrintfFinalCommitment(...) do { \
    LogInstance().LogPrintStr(strprintf("CFinalCommitment::%s -- %s", __func__, tinyformat::format(__VA_ARGS__))); \
} while(0)

bool CFinalCommitment::Verify(const CBlockIndex* pQuorumBaseBlockIndex, bool checkSigs) const
{
    if (nVersion == 0 || nVersion > INDEXED_QUORUM_VERSION) {
        LogPrintfFinalCommitment("q[%s] invalid nVersion=%d\n", quorumHash.ToString(), nVersion);
        return false;
    }

    if (!Params().HasLLMQ(llmqType)) {
        LogPrintfFinalCommitment("q[%s] invalid llmqType=%d\n", quorumHash.ToString(), static_cast<uint8_t>(llmqType));
        return false;
    }
    const auto& llmq_params = GetLLMQParams(llmqType);

    if (!VerifySizes(llmq_params)) {
        return false;
    }

    if (CountValidMembers() < llmq_params.minSize) {
        LogPrintfFinalCommitment("q[%s] invalid validMembers count. validMembersCount=%d\n", quorumHash.ToString(), CountValidMembers());
        return false;
    }
    if (CountSigners() < llmq_params.minSize) {
        LogPrintfFinalCommitment("q[%s] invalid signers count. signersCount=%d\n", quorumHash.ToString(), CountSigners());
        return false;
    }
    if (!quorumPublicKey.IsValid()) {
        LogPrintfFinalCommitment("q[%s] invalid quorumPublicKey\n", quorumHash.ToString());
        return false;
    }
    if (quorumVvecHash.IsNull()) {
        LogPrintfFinalCommitment("q[%s] invalid quorumVvecHash\n", quorumHash.ToString());
        return false;
    }
    if (!membersSig.IsValid()) {
        LogPrintfFinalCommitment("q[%s] invalid membersSig\n", quorumHash.ToString());
        return false;
    }
    if (!quorumSig.IsValid()) {
        LogPrintfFinalCommitment("q[%s] invalid vvecSig\n");
        return false;
    }

    auto members = CLLMQUtils::GetAllQuorumMembers(llmqType, pQuorumBaseBlockIndex);
    for (size_t i = members.size(); i < llmq_params.size; i++) {
        if (validMembers[i]) {
            LogPrintfFinalCommitment("q[%s] invalid validMembers bitset. bit %d should not be set\n", quorumHash.ToString(), i);
            return false;
        }
        if (signers[i]) {
            LogPrintfFinalCommitment("q[%s] invalid signers bitset. bit %d should not be set\n", quorumHash.ToString(), i);
            return false;
        }
    }

    // sigs are only checked when the block is processed
    if (checkSigs) {
        uint256 commitmentHash = CLLMQUtils::BuildCommitmentHash(llmq_params.type, quorumHash, validMembers, quorumPublicKey, quorumVvecHash);

        std::vector<CBLSPublicKey> memberPubKeys;
        for (size_t i = 0; i < members.size(); i++) {
            if (!signers[i]) {
                continue;
            }
            memberPubKeys.emplace_back(members[i]->pdmnState->pubKeyOperator.Get());
        }

        if (!membersSig.VerifySecureAggregated(memberPubKeys, commitmentHash)) {
            LogPrintfFinalCommitment("q[%s] invalid aggregated members signature\n",quorumHash.ToString());
            return false;
        }

        if (!quorumSig.VerifyInsecure(quorumPublicKey, commitmentHash)) {
            LogPrintfFinalCommitment("q[%s] invalid quorum signature\n", quorumHash.ToString());
            return false;
        }
    }

    LogPrintfFinalCommitment("q[%s] VALID\n", quorumHash.ToString());

    return true;
}

bool CFinalCommitment::VerifyNull() const
{
    if (!Params().HasLLMQ(llmqType)) {
        LogPrintfFinalCommitment("q[%s]invalid llmqType=%d\n", quorumHash.ToString(), static_cast<uint8_t>(llmqType));
        return false;
    }

    if (!IsNull() || !VerifySizes(GetLLMQParams(llmqType))) {
        return false;
    }

    return true;
}

bool CFinalCommitment::VerifySizes(const Consensus::LLMQParams& params) const
{
    if (signers.size() != params.size) {
        LogPrintfFinalCommitment("q[%s] invalid signers.size=%d\n", signers.size(), quorumHash.ToString());
        return false;
    }
    if (validMembers.size() != params.size) {
        LogPrintfFinalCommitment("q[%d] invalid signers.size=%d\n", signers.size(), quorumHash.ToString());
        return false;
    }
    return true;
}

bool CheckLLMQCommitment(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    CFinalCommitmentTxPayload qcTx;
    if (!GetTxPayload(tx, qcTx)) {
        LogPrintfFinalCommitment("h[%d] GetTxPayload failed\n", pindexPrev->nHeight);
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-payload");
    }

    if (qcTx.nVersion == 0 || qcTx.nVersion > CFinalCommitmentTxPayload::CURRENT_VERSION) {
        LogPrintfFinalCommitment("h[%d] invalid qcTx.nVersion[%d]\n", pindexPrev->nHeight, qcTx.nVersion);
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-version");
    }

    if (qcTx.nHeight != pindexPrev->nHeight + 1) {
        LogPrintfFinalCommitment("h[%d] invalid qcTx.nHeight[%d]\n", pindexPrev->nHeight, qcTx.nHeight);
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-height");
    }

    const CBlockIndex* pQuorumBaseBlockIndex = WITH_LOCK(cs_main, return LookupBlockIndex(qcTx.commitment.quorumHash));
    if (!pQuorumBaseBlockIndex) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-hash");
    }


    if (pQuorumBaseBlockIndex != pindexPrev->GetAncestor(pQuorumBaseBlockIndex->nHeight)) {
        // not part of active chain
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-hash");
    }

    if (!Params().HasLLMQ(qcTx.commitment.llmqType)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-type");
    }

    if (qcTx.commitment.IsNull()) {
        if (!qcTx.commitment.VerifyNull()) {
            LogPrintfFinalCommitment("h[%d] invalid qcTx.commitment[%s] VerifyNull failed\n", pindexPrev->nHeight, qcTx.commitment.quorumHash.ToString());
            return state.DoS(100, false, REJECT_INVALID, "bad-qc-invalid-null");
        }
        return true;
    }

    if (!qcTx.commitment.Verify(pQuorumBaseBlockIndex, false)) {
        LogPrintfFinalCommitment("h[%d] invalid qcTx.commitment[%s] Verify failed\n", pindexPrev->nHeight, qcTx.commitment.quorumHash.ToString());
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-invalid");
    }

    LogPrintfFinalCommitment("h[%d] CheckLLMQCommitment VALID\n", pindexPrev->nHeight);

    return true;
}

} // namespace llmq
