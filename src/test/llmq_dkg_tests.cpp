// Copyright (c) 2022-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <dbwrapper.h>
#include <evo/deterministicmns.h>
#include <llmq/context.h>
#include <llmq/dkgmessages.h>
#include <llmq/dkgsession.h>
#include <llmq/dkgsessionhandler.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <protocol.h>
#include <streams.h>
#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>
#include <util/helpers.h>
#include <util/std23.h>

#include <validation.h>

#include <array>
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(llmq_dkg_tests)

BOOST_AUTO_TEST_CASE(llmq_dkgerror)
{
    using namespace llmq;
    for (auto i : util::irange(std23::to_underlying(llmq::DKGError::type::_COUNT))) {
        BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type(i)) == 0.0);
        SetSimulatedDKGErrorRate(llmq::DKGError::type(i), 1.0);
        BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type(i)) == 1.0);
    }
    BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type::_COUNT) == 0.0);
    SetSimulatedDKGErrorRate(llmq::DKGError::type::_COUNT, 1.0);
    BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type::_COUNT) == 0.0);
}

namespace {
std::shared_ptr<CDataStream> MakeDKGMessage()
{
    return std::make_shared<CDataStream>(SER_NETWORK, PROTOCOL_VERSION);
}

uint256 MakeTestHash(uint8_t value)
{
    uint256 hash;
    hash.begin()[0] = value;
    return hash;
}

struct VerifiedContributionsSetup : RegTestingSetup {
    VerifiedContributionsSetup() :
        RegTestingSetup{{"-dip3params=1:1", "-testactivationheight=v20@1"}}
    {
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(pending_messages_own_messages_share_quota_path)
{
    using namespace llmq;

    const uint256 own_protx = MakeTestHash(0xee);

    // Own messages (from=-1) are enqueued under this node's proTxHash and
    // charged like any other sender's.
    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2};
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(2));
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(3));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(2)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(3)));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(3).size(), 2U);
}

BOOST_AUTO_TEST_CASE(pending_messages_quota_survives_reconnect)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);

    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(1)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(2)));

    // Reconnecting mints a fresh NodeId but keeps the proTxHash, so the quota
    // is already spent.
    pending.PushPendingMessage(/*from=*/2, protx_a, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/3, protx_a, MakeDKGMessage(), MakeTestHash(4));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(4)));

    // Draining frees queue slots but does not refund the per-proTx quota.
    BOOST_CHECK_EQUAL(pending.PopPendingMessages(5).size(), 2U);
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // A new round resets everything.
    pending.Clear();
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(5)));
}

BOOST_AUTO_TEST_CASE(pending_messages_quota_is_per_protx)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);
    const uint256 protx_b = MakeTestHash(0xb1);

    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));

    // Duplicates are rejected before charging the quota.
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(2));

    // One proTx's spent quota has no effect on another's.
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(4));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(4)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // A quota-dropped hash is not marked seen, so a sender with remaining
    // budget can still deliver it.
    const uint256 protx_c = MakeTestHash(0xc1);
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(5)));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(6).size(), 5U);
}

BOOST_AUTO_TEST_CASE(contribution_records_vvec_scheme)
{
    // A contribution remembers which BLS scheme its vvec was deserialized under, since only the basic scheme checks
    // subgroup membership, which weighted contribution verification relies on
    using namespace llmq;

    const bool legacy_scheme_before{bls::bls_legacy_scheme.load()};

    uint256 seed{uint256S("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")};
    *seed.begin() = 0; // keep it below the group order
    const CBLSSecretKey sk{Span{seed}};
    BOOST_REQUIRE(sk.IsValid());
    CDKGContribution qc;
    qc.llmqType = Consensus::LLMQType::LLMQ_TEST;
    qc.vvec = std::make_shared<std::vector<CBLSPublicKey>>(std::vector<CBLSPublicKey>{sk.GetPublicKey()});
    qc.contributions = std::make_shared<CBLSIESMultiRecipientObjects<CBLSSecretKey>>();

    for (const bool legacy : {true, false}) {
        bls::bls_legacy_scheme.store(legacy);
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << qc;
        CDKGContribution received;
        stream >> received;
        BOOST_CHECK_EQUAL(received.vvecLegacyScheme, legacy);
        BOOST_REQUIRE_EQUAL(received.vvec->size(), 1U);
        BOOST_CHECK((*received.vvec)[0].IsValid());
        BOOST_CHECK((*received.vvec)[0] == (*qc.vvec)[0]);
        BOOST_CHECK(stream.empty());
    }

    bls::bls_legacy_scheme.store(legacy_scheme_before);
}

BOOST_FIXTURE_TEST_CASE(verified_contributions_cache_tracks_writes, VerifiedContributionsSetup)
{
    using namespace llmq;

    const auto& params = llmq::testutils::GetLLMQParams(Consensus::LLMQType::LLMQ_TEST);
    std::array<uint256, 25> hashes;
    std::array<CBlockIndex, 25> blocks;
    for (const auto i : util::irange(blocks.size())) {
        hashes[i] = GetRandHash();
        blocks[i].phashBlock = &hashes[i];
        blocks[i].nHeight = i;
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].BuildSkip();
    }
    const CBlockIndex* base = &blocks.back();

    const auto* work = base->GetAncestor(base->nHeight - llmq::WORK_DIFF_DEPTH);
    CDeterministicMNList mnList{work->GetBlockHash(), work->nHeight, 3};
    std::vector<uint256> proTxHashes;
    std::vector<BLSVerificationVectorPtr> expectedVvecs;
    std::vector<CBLSSecretKey> expectedSkContributions;
    for (const auto i : util::irange(3)) {
        const uint256 proTxHash = MakeTestHash(static_cast<uint8_t>(i + 10));
        auto dmn = std::make_shared<CDeterministicMN>(i, MnType::Regular);
        dmn->proTxHash = proTxHash;
        dmn->collateralOutpoint = COutPoint(proTxHash, 0);
        auto state = std::make_shared<CDeterministicMNState>();
        uint160 owner;
        WriteLE64(owner.begin(), i + 1);
        state->keyIDOwner = CKeyID{owner};
        state->netInfo = NetInfoInterface::MakeNetInfo(ProTxVersion::LegacyBLS);
        state->UpdateConfirmedHash(proTxHash, MakeTestHash(static_cast<uint8_t>(i + 20)));
        CBLSSecretKey operatorKey;
        operatorKey.MakeNewKey();
        state->pubKeyOperator.Set(operatorKey.GetPublicKey(), false);
        dmn->pdmnState = std::move(state);
        mnList.AddMN(dmn);
        proTxHashes.emplace_back(proTxHash);

        auto vvec = std::make_shared<std::vector<CBLSPublicKey>>();
        CBLSSecretKey coefficient;
        coefficient.MakeNewKey();
        vvec->emplace_back(coefficient.GetPublicKey());
        expectedVvecs.emplace_back(std::move(vvec));
        expectedSkContributions.emplace_back(coefficient);
    }
    m_node.dmnman->SetListForBlockForTesting(mnList);
    const auto members = utils::GetAllQuorumMembers(params.type,
                                                    {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, base},
                                                    true);
    BOOST_REQUIRE_EQUAL(members.size(), 3U);
    auto checkResult = [&](const auto& vvecs, const auto& skContributions) {
        BOOST_REQUIRE_EQUAL(vvecs.size(), 3U);
        BOOST_REQUIRE_EQUAL(skContributions.size(), 3U);
        for (const auto i : util::irange(members.size())) {
            const auto original = std::find(proTxHashes.begin(), proTxHashes.end(), members[i]->proTxHash) -
                                  proTxHashes.begin();
            BOOST_CHECK(*vvecs[i] == *expectedVvecs[original]);
            BOOST_CHECK(skContributions[i] == expectedSkContributions[original]);
            BOOST_CHECK((*vvecs[i])[0].ToBytes(false) == (*expectedVvecs[original])[0].ToBytes(false));
            BOOST_CHECK(skContributions[i].ToBytes(false) == expectedSkContributions[original].ToBytes(false));
        }
    };

    const auto dbPath = m_path_root / "dkg-cache-test";
    const util::DbWrapperParams dbParams{.path = dbPath, .memory = false, .wipe = true};
    std::vector<bool> allMembers(3, true);
    {
        CDKGSessionManager manager{*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, *m_node.sporkman,
                                   dbParams};
        std::vector<uint16_t> memberIndexes;
        std::vector<BLSVerificationVectorPtr> vvecs;
        std::vector<CBLSSecretKey> skContributions;
        manager.WriteVerifiedVvecContribution(params.type, base, proTxHashes[0], expectedVvecs[0], true);
        manager.WriteVerifiedSkContribution(params.type, base, proTxHashes[0], expectedSkContributions[0]);
        manager.WriteVerifiedSkContribution(params.type, base, proTxHashes[1], expectedSkContributions[1]);
        manager.WriteVerifiedVvecContribution(params.type, base, proTxHashes[1], expectedVvecs[1]);
        BOOST_CHECK(!manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        const std::vector<bool> knownMembers = [&] {
            std::vector<bool> selected(3, true);
            for (const auto i : util::irange(members.size())) {
                if (members[i]->proTxHash == proTxHashes[2]) selected[i] = false;
            }
            return selected;
        }();
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, knownMembers, memberIndexes, vvecs, skContributions));
        BOOST_CHECK_EQUAL(vvecs.size(), 2U);
        manager.WriteVerifiedVvecContribution(params.type, base, proTxHashes[2], expectedVvecs[2]);
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        for (const auto i : util::irange(members.size())) {
            if (members[i]->proTxHash == proTxHashes[2]) BOOST_CHECK(!skContributions[i].IsValid());
        }
        manager.WriteVerifiedSkContribution(params.type, base, proTxHashes[2], expectedSkContributions[2]);
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        BOOST_REQUIRE_EQUAL(memberIndexes.size(), 3U);
        BOOST_REQUIRE_EQUAL(vvecs.size(), 3U);
        BOOST_REQUIRE_EQUAL(skContributions.size(), 3U);
        checkResult(vvecs, skContributions);
        manager.UpdatedBlockTip(base, true);
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        for (const auto i : util::irange(members.size())) {
            const auto original = std::find(proTxHashes.begin(), proTxHashes.end(), members[i]->proTxHash) -
                                  proTxHashes.begin();
            BOOST_CHECK(vvecs[i] == expectedVvecs[original]);
            BOOST_CHECK_EQUAL(memberIndexes[i], i);
        }

        CBLSSecretKey replacementSk;
        replacementSk.MakeNewKey();
        expectedVvecs[0] = std::make_shared<std::vector<CBLSPublicKey>>(
            std::vector<CBLSPublicKey>{replacementSk.GetPublicKey()});
        manager.WriteVerifiedVvecContribution(params.type, base, proTxHashes[0], expectedVvecs[0]);
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        checkResult(vvecs, skContributions);
        expectedSkContributions[0] = replacementSk;
        manager.WriteVerifiedSkContribution(params.type, base, proTxHashes[0], expectedSkContributions[0]);
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        checkResult(vvecs, skContributions);

        std::vector<std::weak_ptr<std::vector<CBLSPublicKey>>> sessionVvecs;
        for (auto& vvec : expectedVvecs) {
            sessionVvecs.emplace_back(vvec);
            vvec = std::make_shared<std::vector<CBLSPublicKey>>(*vvec);
        }
        vvecs.clear();
        manager.UpdatedBlockTip(base, true);
        for (const auto& vvec : sessionVvecs)
            BOOST_CHECK(vvec.expired());
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        checkResult(vvecs, skContributions);

        for (const auto& vvec : vvecs) {
            for (const auto& pubkey : *vvec)
                pubkey.GetHash();
        }
        const bool originalScheme{bls::bls_legacy_scheme.load()};
        struct RestoreScheme {
            const bool saved;
            ~RestoreScheme() { bls::bls_legacy_scheme.store(saved); }
        } restoreScheme{originalScheme};
        bls::bls_legacy_scheme.store(!originalScheme);
        manager.WriteVerifiedVvecContribution(params.type, base, proTxHashes[0], expectedVvecs[0]);
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        checkResult(vvecs, skContributions);
        for (const auto& vvec : vvecs) {
            BOOST_CHECK(std::find(expectedVvecs.begin(), expectedVvecs.end(), vvec) == expectedVvecs.end());
            for (auto& pubkey : *vvec) {
                BOOST_CHECK(pubkey.GetHash() == ::SerializeHash(CBLSPublicKeyVersionWrapper(pubkey, !originalScheme)));
            }
        }
        bls::bls_legacy_scheme.store(originalScheme);
        BOOST_REQUIRE(
            manager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
        checkResult(vvecs, skContributions);
    }

    CDKGSessionManager coldManager{*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, *m_node.sporkman,
                                   util::DbWrapperParams{.path = dbPath, .memory = false, .wipe = false}};
    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    std::vector<CBLSSecretKey> skContributions;
    BOOST_REQUIRE(
        coldManager.GetVerifiedContributions(params.type, base, allMembers, memberIndexes, vvecs, skContributions));
    checkResult(vvecs, skContributions);
}

BOOST_AUTO_TEST_SUITE_END()
