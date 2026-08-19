// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <interfaces/chain.h>
#include <test/util/masternode.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(availablecoins_tests, WalletTestingSetup)
class AvailableCoinsTestingSetup : public TestChain100Setup
{
public:
    AvailableCoinsTestingSetup()
    {
        CreateAndProcessBlock({}, {});
        wallet = CreateSyncedWallet(*m_node.chain, *m_node.coinjoin_loader, *m_node.chainman, m_args, coinbaseKey);
    }

    ~AvailableCoinsTestingSetup()
    {
        wallet.reset();
    }
    CWalletTx& AddTx(CRecipient recipient)
    {
        CTransactionRef tx;
        CCoinControl dummy;
        {
            auto res = CreateTransaction(*wallet, {recipient}, RANDOM_CHANGE_POSITION, dummy);
            BOOST_CHECK(res);
            tx = res->tx;
        }
        wallet->CommitTransaction(tx, {}, {});
        CMutableTransaction blocktx;
        {
            LOCK(wallet->cs_wallet);
            blocktx = CMutableTransaction(*wallet->mapWallet.at(tx->GetHash()).tx);
        }
        CreateAndProcessBlock({CMutableTransaction(blocktx)}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

        const CBlockIndex* tip{WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Tip())};
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        auto it = wallet->mapWallet.find(tx->GetHash());
        BOOST_CHECK(it != wallet->mapWallet.end());
        it->second.m_state = TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/1};
        return it->second;
    }

    std::unique_ptr<CWallet> wallet;
};

BOOST_FIXTURE_TEST_CASE(BasicOutputTypesTest, AvailableCoinsTestingSetup)
{
    CoinsResult available_coins;
    util::Result<CTxDestination> dest{util::Error{}};

    // Verify our wallet has one usable coinbase UTXO before starting
    // This UTXO is a P2PK, so it should show up in the Other bucket
    {
        LOCK(wallet->cs_wallet);
        available_coins = AvailableCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(available_coins.size(), 1U);
    BOOST_CHECK_EQUAL(available_coins.other.size(), 1U);

    // We will create a self transfer for each of the OutputTypes and
    // verify it is put in the correct bucket after running GetAvailablecoins
    //
    // For each OutputType, We expect 2 UTXOs in our wallet following the self transfer:
    //   1. One UTXO as the recipient
    //   2. One UTXO from the change, due to payment address matching logic

    // Legacy (P2PKH)
    {
        LOCK(wallet->cs_wallet);
        dest = wallet->GetNewDestination("");
    }
    BOOST_ASSERT(dest);
    AddTx(CRecipient{{GetScriptForDestination(*dest)}, 4 * COIN, /*fSubtractFeeFromAmount=*/true});
    {
        LOCK(wallet->cs_wallet);
        available_coins = AvailableCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(available_coins.legacy.size(), 2U);
}

BOOST_FIXTURE_TEST_CASE(AbandonedSpendReleasesItsInputs, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);

    const CoinsResult before{AvailableCoins(*wallet)};
    BOOST_CHECK(before.size() > 0);

    CCoinControl coin_control;
    auto created{CreateTransaction(*wallet, {CRecipient{{GetScriptForRawPubKey(coinbaseKey.GetPubKey())}, 1 * COIN,
                                                        /*fSubtractFeeFromAmount=*/false}},
                                   RANDOM_CHANGE_POSITION, coin_control)};
    BOOST_CHECK(created);
    const CTransactionRef tx{created->tx};
    BOOST_REQUIRE(!tx->vin.empty());
    const CTxIn& input{tx->vin.front()};
    const CAmount input_amount{wallet->mapWallet.at(input.prevout.hash).tx->vout.at(input.prevout.n).nValue};
    const int inputs_before{wallet->CountInputsWithAmount(input_amount)};
    BOOST_CHECK(inputs_before > 0);

    // The transaction is only in the wallet: never broadcast, never mined.
    BOOST_CHECK(wallet->AddToWallet(tx, TxStateInactive{}));
    BOOST_CHECK(AvailableCoins(*wallet).size() < before.size());
    BOOST_CHECK(wallet->CountInputsWithAmount(input_amount) < inputs_before);

    // Abandoning it makes the coins it spent available again, without a reload.
    BOOST_CHECK(wallet->AbandonTransaction(tx->GetHash()));
    const CoinsResult after{AvailableCoins(*wallet)};
    BOOST_CHECK_EQUAL(after.size(), before.size());
    BOOST_CHECK_EQUAL(after.total_amount, before.total_amount);
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(input_amount), inputs_before);

    // The abandoned transaction re-entering the mempool spends the inputs
    // again: the restored outpoints must leave the wallet UTXO set, or
    // functions that trust it directly (CountInputsWithAmount and the
    // CoinJoin rounds accounting) would count spent coins.
    BOOST_CHECK(wallet->AddToWallet(tx, TxStateInMempool{}));
    BOOST_CHECK(wallet->IsSpent(input.prevout));
    BOOST_CHECK(wallet->CountInputsWithAmount(input_amount) < inputs_before);
}

BOOST_FIXTURE_TEST_CASE(ConflictedDescendantReactivationReconcilesInputs, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);

    const CScript wallet_script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
    auto created{CreateTransaction(*wallet, {CRecipient{wallet_script, 1 * COIN, /*fSubtractFeeFromAmount=*/false}},
                                   RANDOM_CHANGE_POSITION, CCoinControl{})};
    BOOST_REQUIRE(created);
    const CTransactionRef parent{created->tx};

    CKey external_key;
    external_key.MakeNewKey(true);
    auto conflict_created{CreateTransaction(*wallet, {CRecipient{GetScriptForRawPubKey(external_key.GetPubKey()), COIN / 4,
                                                                 /*fSubtractFeeFromAmount=*/false}},
                                            RANDOM_CHANGE_POSITION, CCoinControl{})};
    BOOST_REQUIRE(conflict_created);
    const CTransactionRef conflict{conflict_created->tx};
    BOOST_REQUIRE(parent->vin.front().prevout == conflict->vin.front().prevout);

    BOOST_REQUIRE(wallet->AddToWallet(parent, TxStateInactive{}));

    const auto parent_output_it{std::ranges::find_if(parent->vout, [&](const CTxOut& output) {
        return output.nValue == 1 * COIN && output.scriptPubKey == wallet_script;
    })};
    BOOST_REQUIRE(parent_output_it != parent->vout.end());
    const COutPoint parent_outpoint{parent->GetHash(), static_cast<uint32_t>(parent_output_it - parent->vout.begin())};

    CMutableTransaction child_mtx;
    child_mtx.vin.emplace_back(parent_outpoint);
    child_mtx.vout.emplace_back(COIN / 2, wallet_script);
    const CTransactionRef child{MakeTransactionRef(child_mtx)};
    BOOST_REQUIRE(wallet->AddToWallet(child, TxStateInactive{}));
    BOOST_CHECK(wallet->IsSpent(parent_outpoint));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(1 * COIN), 0);

    // A block transaction conflicts the parent and recursively conflicts the
    // child. The child's input is temporarily unspent and returns to the UTXO
    // set, although CountInputsWithAmount() ignores it while its parent is
    // conflicted.
    const CBlock block{CreateAndProcessBlock({CMutableTransaction{*conflict}}, GetScriptForRawPubKey({}))};
    const uint256 block_hash{block.GetHash()};
    const CBlockIndex* tip{WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Tip())};
    BOOST_REQUIRE_EQUAL(tip->GetBlockHash(), block_hash);

    interfaces::BlockInfo block_info{block_hash};
    block_info.prev_hash = &block.hashPrevBlock;
    block_info.height = tip->nHeight;
    block_info.data = &block;
    wallet->blockConnected(block_info);
    BOOST_REQUIRE(wallet->mapWallet.at(child->GetHash()).isConflicted());
    BOOST_CHECK(!wallet->IsSpent(parent_outpoint));

    // Disconnecting the conflicting block makes the parent and descendant
    // inactive again. The child therefore spends parent_outpoint again, and
    // the public CoinJoin counter must not observe a stale UTXO-set entry.
    wallet->blockDisconnected(block_info);
    BOOST_CHECK(!wallet->mapWallet.at(child->GetHash()).isConflicted());
    BOOST_CHECK(wallet->IsSpent(parent_outpoint));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(1 * COIN), 0);
}

BOOST_FIXTURE_TEST_CASE(AbandonedSpendRestoresDustLock, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);
    wallet->m_dust_protection_threshold = 1 * COIN;

    const auto dest{wallet->GetNewDestination("")};
    BOOST_ASSERT(dest);

    // An external transaction (no input is ours) pays us a dust-protection
    // target; AddToWallet() locks the output on insertion.
    CMutableTransaction dust_mtx;
    dust_mtx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    dust_mtx.vout.emplace_back(COIN / 100, GetScriptForDestination(*dest));
    const CTransactionRef dust_tx{MakeTransactionRef(dust_mtx)};
    const COutPoint dust_outpoint{dust_tx->GetHash(), 0};
    BOOST_CHECK(wallet->AddToWallet(dust_tx, TxStateInMempool{}));
    BOOST_CHECK(wallet->IsLockedCoin(dust_outpoint));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(COIN / 100), 1);

    // A wallet transaction spending it unlocks it and removes it from the
    // wallet UTXO set.
    CMutableTransaction spend_mtx;
    spend_mtx.vin.emplace_back(dust_outpoint);
    spend_mtx.vout.emplace_back(COIN / 200, GetScriptForDestination(*dest));
    const CTransactionRef spend_tx{MakeTransactionRef(spend_mtx)};
    BOOST_CHECK(wallet->AddToWallet(spend_tx, TxStateInactive{}));
    BOOST_CHECK(!wallet->IsLockedCoin(dust_outpoint));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(COIN / 100), 0);

    // Abandoning the spend restores the outpoint together with the automatic
    // dust lock a wallet reload would apply.
    BOOST_CHECK(wallet->AbandonTransaction(spend_tx->GetHash()));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(COIN / 100), 1);
    BOOST_CHECK(wallet->IsLockedCoin(dust_outpoint));

    BOOST_CHECK(wallet->AddToWallet(spend_tx, TxStateInMempool{}));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(COIN / 100), 0);
    BOOST_CHECK(!wallet->IsLockedCoin(dust_outpoint));
}

BOOST_FIXTURE_TEST_CASE(AbandonedSpendRestoresActiveMasternodeCollateralLock, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);

    const CScript wallet_script{GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()))};
    while (WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Height()) <
           Params().GetConsensus().DIP0003Height) {
        CreateAndProcessBlock({}, wallet_script);
    }

    CKey owner_key;
    CBLSSecretKey operator_key;
    auto utxos{BuildSimpleUtxoMap(m_coinbase_txns)};
    CMutableTransaction pro_reg_mtx{CreateProRegTx(*m_node.chainman, utxos, /*port=*/1, wallet_script,
                                                   coinbaseKey, owner_key, operator_key)};
    const CTransactionRef pro_reg_tx{MakeTransactionRef(pro_reg_mtx)};
    const CBlock block{CreateAndProcessBlock({pro_reg_mtx}, wallet_script)};
    const CBlockIndex* tip{WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Tip())};
    {
        LOCK(::cs_main);
        m_node.dmnman->UpdatedBlockTip(tip);
        BOOST_REQUIRE(m_node.dmnman->GetListAtChainTip().HasMN(pro_reg_tx->GetHash()));
    }

    const uint256 block_hash{block.GetHash()};
    interfaces::BlockInfo block_info{block_hash};
    block_info.prev_hash = &block.hashPrevBlock;
    block_info.height = tip->nHeight;
    block_info.data = &block;
    wallet->blockConnected(block_info);

    const COutPoint collateral{pro_reg_tx->GetHash(), 0};
    BOOST_CHECK(wallet->IsLockedCoin(collateral));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(dmn_types::Regular.collat_amount), 1);

    CMutableTransaction spend_mtx;
    spend_mtx.vin.emplace_back(collateral);
    spend_mtx.vout.emplace_back(1 * COIN, wallet_script);
    const CTransactionRef spend_tx{MakeTransactionRef(spend_mtx)};
    BOOST_REQUIRE(wallet->AddToWallet(spend_tx, TxStateInactive{}));
    BOOST_CHECK(!wallet->IsLockedCoin(collateral));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(dmn_types::Regular.collat_amount), 0);

    BOOST_REQUIRE(wallet->AbandonTransaction(spend_tx->GetHash()));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(dmn_types::Regular.collat_amount), 1);
    BOOST_CHECK(wallet->IsLockedCoin(collateral));

    BOOST_CHECK(wallet->AddToWallet(spend_tx, TxStateInMempool{}));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(dmn_types::Regular.collat_amount), 0);
    BOOST_CHECK(!wallet->IsLockedCoin(collateral));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
