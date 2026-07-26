// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chain.h>
#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <compat/endian.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <evo/cbtx.h>
#include <evo/evodb.h>
#include <evo/specialtxman.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/params.h>
#include <llmq/quorumsman.h>
#include <primitives/block.h>
#include <uint256.h>
#include <validation.h>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

// Keys mirror the file-local constants in src/llmq/blockprocessor.cpp so unit tests can
// seed mined-commitment rows without going through full DKG/mining validation.
static const std::string DB_MINED_COMMITMENT = "q_mc";
static const std::string DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT = "q_mcih";

static std::tuple<std::string, Consensus::LLMQType, uint32_t> BuildInversedHeightKey(Consensus::LLMQType llmqType,
                                                                                    int nMinedHeight)
{
    return std::make_tuple(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT, llmqType,
                           htobe32_internal(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

static void WriteMinedCommitment(CEvoDB& evo_db, const llmq::CFinalCommitment& qc, const uint256& mined_block_hash,
                                 int mined_height, int quorum_height)
{
    const auto cache_key = std::make_pair(qc.llmqType, qc.quorumHash);
    evo_db.Write(std::make_pair(DB_MINED_COMMITMENT, cache_key), std::make_pair(qc, mined_block_hash));
    evo_db.Write(BuildInversedHeightKey(qc.llmqType, mined_height), quorum_height);
}

static void EraseMinedCommitment(CEvoDB& evo_db, Consensus::LLMQType llmq_type, const uint256& quorum_hash,
                                 int mined_height)
{
    evo_db.Erase(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(llmq_type, quorum_hash)));
    evo_db.Erase(BuildInversedHeightKey(llmq_type, mined_height));
}

static llmq::CFinalCommitment MakeDistinctCommitment(const Consensus::LLMQParams& params, const uint256& quorum_hash,
                                                     bool flip_last_signer)
{
    auto commitment = llmq::testutils::CreateValidCommitment(params, quorum_hash);
    // signers is part of SERIALIZE_METHODS / SerializeHash but is not covered by the signed
    // commitmentHash, so two valid commitments for the same quorum can differ only here.
    BOOST_REQUIRE(!commitment.signers.empty());
    if (flip_last_signer) {
        commitment.signers.back() = !commitment.signers.back();
        commitment.membersSig = llmq::testutils::CreateRandomBLSSignature();
    }
    return commitment;
}

static uint256 CalcQuorumMerkleRoot(const CBlockIndex* pindex_prev, const llmq::CQuorumBlockProcessor& qbp)
{
    CBlock block;
    // Coinbase only: current-block commitments are intentionally absent so the result is
    // driven solely by CachedGetQcHashesQcIndexedHashes / GetMinedCommitment.
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back();
    coinbase.vout.emplace_back();
    block.vtx.emplace_back(MakeTransactionRef(std::move(coinbase)));
    uint256 merkle_root;
    BlockValidationState state;
    BOOST_REQUIRE(CalcCbTxMerkleRootQuorums(block, pindex_prev, qbp, merkle_root, state));
    return merkle_root;
}

BOOST_AUTO_TEST_SUITE(evo_cbtx_tests)

// Out-of-range bestCLHeightDiff (>= pindex->nHeight) must be rejected with
// "bad-cbtx-cldiff" so that the subsequent GetAncestor() call sees a valid height.
//
// The defensive nullptr branch after GetAncestor() returns "bad-cbtx-cldiff-ancestor".
// That branch is unreachable in practice (the range check guarantees the requested
// ancestor height is in [0, pindex->nHeight - 1], for which GetAncestor() never returns
// nullptr) and cannot be exercised from a unit test: a fake CBlockIndex with no pprev
// would trip GetAncestor()'s `assert(pprev)` while walking, not return nullptr.
BOOST_FIXTURE_TEST_CASE(check_cbtx_best_chainlock_rejects_excessive_height_diff, RegTestingSetup)
{
    const auto& consensus_params = Params().GetConsensus();
    const auto& chain = *WITH_LOCK(::cs_main, return &m_node.chainman->ActiveChain());
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    auto& chainlocks = *Assert(m_node.chainlocks);

    // Standalone fake block index with no predecessor, so the prevBlockCoinbaseChainlock
    // branch is skipped and the validation path under test is reached directly.
    CBlockIndex pindex;
    pindex.nHeight = 5;

    // A structurally-valid BLS signature is required for the IsValid() guard.
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const bool legacy_scheme = bls::bls_legacy_scheme.load();
    CBLSSignature valid_sig = sk.Sign(uint256::ONE, legacy_scheme);
    BOOST_REQUIRE(valid_sig.IsValid());

    CCbTx cbTx;
    cbTx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbTx.bestCLSignature = valid_sig;

    // bestCLHeightDiff == nHeight: lower boundary of the rejected range.
    cbTx.bestCLHeightDiff = static_cast<uint32_t>(pindex.nHeight);
    BlockValidationState state;
    BOOST_CHECK(!CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cbtx-cldiff");

    // Upper boundary: uint32_t max.
    cbTx.bestCLHeightDiff = std::numeric_limits<uint32_t>::max();
    BlockValidationState state_big;
    BOOST_CHECK(!CheckCbTxBestChainlock(cbTx, &pindex, consensus_params, chain, qman, chainlocks, state_big));
    BOOST_CHECK_EQUAL(state_big.GetRejectReason(), "bad-cbtx-cldiff");
}

// CachedGetQcHashesQcIndexedHashes must never memoise ::SerializeHash(minedCommitment)
// under a key that is only the quorum *base* block hash. The EvoDB mined-commitment row for a
// given quorumHash is mutable: on reorg UndoBlock erases it and a competing chain may mine a
// different but equally valid CFinalCommitment for the same quorum (`signers` feeds into
// SerializeHash but is not covered by the signed commitmentHash). A cache surviving that
// mutation makes CalcCbTxMerkleRootQuorums return a root derived from the pre-reorg commitment,
// rejecting the honest majority tip with bad-cbtx-quorummerkleroot (BLOCK_CONSENSUS) -- which is
// persisted as BLOCK_FAILED_VALID and therefore survives restart. Permanent chain split.
//
// This test seeds EvoDB directly (no full DKG) to pin that invariant:
//   1. Chain A mines C1 for quorum base H_Q; compute the root (warms any cache keyed on H_Q).
//   2. Evaluate against a tip below the mined height, so the outer quorums_cached short-circuit
//      misses exactly as it does for intermediate blocks of a real reorg.
//   3. Replace the EvoDB row with C2 (simulating UndoBlock + ConnectBlock of the competing tip).
//   4. Recompute: must agree with a fresh SerializeHash(C2) root, not the stale C1 value.
BOOST_FIXTURE_TEST_CASE(cbtx_quorum_merkle_root_reorg_invalidates_mined_commitment_cache, TestChain100Setup)
{
    auto& evo_db = *Assert(m_node.evodb);
    auto& qbp = *Assert(m_node.llmq_ctx)->quorum_block_processor;
    const auto& params = llmq::testutils::GetLLMQParams(Consensus::LLMQType::LLMQ_TEST);

    const CBlockIndex* pindex_tip;
    const CBlockIndex* pindex_quorum;
    const CBlockIndex* pindex_mined;
    const CBlockIndex* pindex_before_mined;
    {
        LOCK(::cs_main);
        const CChain& chain = m_node.chainman->ActiveChain();
        BOOST_REQUIRE_GE(chain.Height(), 30);
        pindex_tip = chain.Tip();
        // Quorum base and the height at which the commitment is recorded as mined. The
        // inverse-height index is only visible for pindex->nHeight >= mined_height.
        pindex_quorum = chain[10];
        pindex_mined = chain[20];
        pindex_before_mined = chain[19];
        BOOST_REQUIRE(pindex_quorum && pindex_mined && pindex_before_mined);
    }

    const uint256 quorum_hash = pindex_quorum->GetBlockHash();
    const uint256 mined_block_hash = pindex_mined->GetBlockHash();
    const int quorum_height = pindex_quorum->nHeight;
    const int mined_height = pindex_mined->nHeight;

    auto c1 = MakeDistinctCommitment(params, quorum_hash, /*flip_last_signer=*/false);
    auto c2 = MakeDistinctCommitment(params, quorum_hash, /*flip_last_signer=*/true);
    const uint256 hash_c1 = ::SerializeHash(c1);
    const uint256 hash_c2 = ::SerializeHash(c2);
    BOOST_REQUIRE(hash_c1 != hash_c2);

    // Independent expected roots (single active commitment -> single leaf).
    const uint256 expected_root_c1 = ComputeMerkleRoot(std::vector<uint256>{hash_c1});
    const uint256 expected_root_c2 = ComputeMerkleRoot(std::vector<uint256>{hash_c2});
    BOOST_REQUIRE(expected_root_c1 != expected_root_c2);

    // 1. Chain A mines C1. Warm qc_hashes_cached with SerializeHash(C1).
    WriteMinedCommitment(evo_db, c1, mined_block_hash, mined_height, quorum_height);
    {
        const auto [got, got_mined] = qbp.GetMinedCommitment(params.type, quorum_hash);
        BOOST_REQUIRE(got_mined == mined_block_hash);
        BOOST_REQUIRE(::SerializeHash(got) == hash_c1);
    }
    const uint256 root_after_c1 = CalcQuorumMerkleRoot(pindex_tip, qbp);
    BOOST_CHECK_EQUAL(root_after_c1, expected_root_c1);

    // 2. Intermediate reorg block: active-quorum set differs (commitment not yet visible
    // below mined_height). This clears the outer quorums/qcHashes caches the way a real
    // reorg does, but must NOT leave a process-lifetime stale entry for H_Q.
    {
        const uint256 root_empty = CalcQuorumMerkleRoot(pindex_before_mined, qbp);
        BOOST_CHECK(root_empty != expected_root_c1);
        BOOST_CHECK(root_empty != expected_root_c2);
    }

    // 3. Competing chain B: UndoBlock erases C1, ConnectBlock writes C2 for the same H_Q.
    EraseMinedCommitment(evo_db, params.type, quorum_hash, mined_height);
    WriteMinedCommitment(evo_db, c2, mined_block_hash, mined_height, quorum_height);
    {
        const auto [got, got_mined] = qbp.GetMinedCommitment(params.type, quorum_hash);
        BOOST_REQUIRE(got_mined == mined_block_hash);
        BOOST_REQUIRE(::SerializeHash(got) == hash_c2);
    }

    // 4. Recompute against the same tip. Must track C2 (fresh EvoDB), not the LRU hit on C1.
    const uint256 root_after_reorg = CalcQuorumMerkleRoot(pindex_tip, qbp);
    BOOST_CHECK_EQUAL(root_after_reorg, expected_root_c2);
    BOOST_CHECK(root_after_reorg != expected_root_c1);

    // Cleanup so later cases in this process do not observe our inverse-height row.
    EraseMinedCommitment(evo_db, params.type, quorum_hash, mined_height);
}

BOOST_AUTO_TEST_SUITE_END()
