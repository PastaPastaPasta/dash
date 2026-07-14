// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/deterministicmns.h>
#include <governance/vote.h>
#include <key_io.h>
#include <messagesigner.h>
#include <spork.h>
#include <timedata.h>

#include <test/util/setup_common.h>

#include <limits>
#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

struct ChronoOverflowSetup : BasicTestingSetup {
    ChronoOverflowSetup() :
        BasicTestingSetup{CBaseChainParams::REGTEST}
    {
    }
};

BOOST_FIXTURE_TEST_SUITE(chrono_overflow_tests, ChronoOverflowSetup)

BOOST_AUTO_TEST_CASE(spork_signed_time_extremes)
{
    auto& sporkman{*Assert(m_node.sporkman)};
    const CKey key{GenerateRandomKey()};
    const CKeyID signer_id{key.GetPubKey().GetID()};
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash{key.GetPubKey()})));
    BOOST_REQUIRE(sporkman.SetMinSporkKeys(1));

    CSporkMessage future_spork;
    future_spork.nSporkID = SPORK_2_INSTANTSEND_ENABLED;
    future_spork.nTimeSigned = std::numeric_limits<int64_t>::max();
    BOOST_REQUIRE(future_spork.Sign(key));
    BOOST_REQUIRE(future_spork.CheckSignature(signer_id));
    BOOST_CHECK(!sporkman.GetValidSporkSigner(future_spork));

    CSporkMessage past_spork;
    past_spork.nSporkID = SPORK_2_INSTANTSEND_ENABLED;
    past_spork.nValue = std::numeric_limits<int64_t>::max();
    past_spork.nTimeSigned = std::numeric_limits<int64_t>::min();
    BOOST_REQUIRE(past_spork.Sign(key));
    const auto past_signer{sporkman.GetValidSporkSigner(past_spork)};
    BOOST_REQUIRE(past_signer);
    BOOST_CHECK(*past_signer == signer_id);
    BOOST_REQUIRE(sporkman.ProcessSpork(past_spork, *past_signer));
    BOOST_CHECK(!sporkman.IsSporkActive(past_spork.nSporkID));

    CSporkMessage active_spork;
    active_spork.nSporkID = SPORK_17_QUORUM_DKG_ENABLED;
    active_spork.nValue = std::numeric_limits<int64_t>::min();
    active_spork.nTimeSigned = std::numeric_limits<int64_t>::min();
    BOOST_REQUIRE(active_spork.Sign(key));
    const auto active_signer{sporkman.GetValidSporkSigner(active_spork)};
    BOOST_REQUIRE(active_signer);
    BOOST_REQUIRE(sporkman.ProcessSpork(active_spork, *active_signer));
    BOOST_CHECK(sporkman.IsSporkActive(active_spork.nSporkID));
}

BOOST_AUTO_TEST_CASE(governance_vote_future_time_extreme)
{
    const CKey voting_key{GenerateRandomKey()};
    const CKeyID voting_id{voting_key.GetPubKey().GetID()};
    const COutPoint collateral{uint256::ONE, 0};

    auto dmn_state{std::make_shared<CDeterministicMNState>()};
    dmn_state->keyIDOwner = voting_id;
    dmn_state->keyIDVoting = voting_id;
    dmn_state->netInfo = NetInfoInterface::MakeNetInfo(dmn_state->nVersion);

    auto dmn{std::make_shared<CDeterministicMN>(0)};
    dmn->proTxHash = uint256S("02");
    dmn->collateralOutpoint = collateral;
    dmn->pdmnState = dmn_state;

    CDeterministicMNList mn_list{uint256{}, 0, 0};
    mn_list.AddMN(dmn);

    const auto sign_vote{[&](CGovernanceVote& vote) {
        std::vector<unsigned char> signature;
        if (!CMessageSigner::SignMessage(vote.GetSignatureString(), signature, voting_key)) return false;
        vote.SetSignature(signature);
        return vote.CheckSignature(voting_id);
    }};

    CGovernanceVote current_vote{collateral, uint256::ONE, VOTE_SIGNAL_FUNDING, VOTE_OUTCOME_YES};
    current_vote.SetTime(GetAdjustedTime());
    BOOST_REQUIRE(sign_vote(current_vote));
    BOOST_CHECK(current_vote.IsValid(mn_list, /*useVotingKey=*/true));

    CGovernanceVote future_vote{collateral, uint256::ONE, VOTE_SIGNAL_FUNDING, VOTE_OUTCOME_YES};
    future_vote.SetTime(std::numeric_limits<int64_t>::max());
    BOOST_REQUIRE(sign_vote(future_vote));
    BOOST_CHECK(!future_vote.IsValid(mn_list, /*useVotingKey=*/true));
}

BOOST_AUTO_TEST_SUITE_END()
