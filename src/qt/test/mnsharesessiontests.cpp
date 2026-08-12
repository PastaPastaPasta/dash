// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/mnsharesessiontests.h>

#include <qt/mnsharesession.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <key.h>
#include <key_io.h>
#include <messagesigner.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <string>
#include <vector>

namespace {
QString FreshP2PKHAddress(CKey& key_out)
{
    key_out.MakeNewKey(/*fCompressed=*/true);
    return QString::fromStdString(EncodeDestination(PKHash(key_out.GetPubKey())));
}

MnShareSession::Share MakeShare(CAmount amount, const QString& owner, const QString& refund,
                                const QString& reward = {})
{
    MnShareSession::Share share;
    share.amount = amount;
    share.ownerAddress = owner;
    share.refundAddress = refund;
    share.rewardAddress = reward;
    return share;
}

//! A session whose share table passes every draft-side check
MnShareSession ValidSession(std::vector<CKey>& owner_keys)
{
    MnShareSession session;
    owner_keys.resize(3);
    CKey dummy;
    session.shares().push_back(MakeShare(400 * COIN, FreshP2PKHAddress(owner_keys[0]), FreshP2PKHAddress(dummy)));
    session.shares().push_back(MakeShare(350 * COIN, FreshP2PKHAddress(owner_keys[1]), FreshP2PKHAddress(dummy)));
    session.shares().push_back(MakeShare(250 * COIN, FreshP2PKHAddress(owner_keys[2]), FreshP2PKHAddress(dummy)));
    CKey voting_key;
    session.terms().votingAddress = FreshP2PKHAddress(voting_key);
    session.terms().earlyPeriodBlocks = 5000;
    session.terms().earlyPenalty = 5 * COIN;
    return session;
}
} // anonymous namespace

void MnShareSessionTests::validateSharesMirror()
{
    BasicTestingSetup setup{CBaseChainParams::REGTEST};
    std::vector<CKey> keys;
    MnShareSession session{ValidSession(keys)};
    QVERIFY(session.validateShares().isEmpty());

    // Too few shares
    MnShareSession too_few{ValidSession(keys)};
    too_few.shares().resize(1);
    QVERIFY(!too_few.validateShares().isEmpty());

    // Sum must be exactly the collateral amount
    MnShareSession bad_sum{ValidSession(keys)};
    bad_sum.shares()[0].amount += COIN;
    QVERIFY(!bad_sum.validateShares().isEmpty());

    // Minimum share amount
    MnShareSession too_small{ValidSession(keys)};
    too_small.shares()[0].amount = 950 * COIN;
    too_small.shares()[1].amount = 25 * COIN; // below the 100 DASH minimum
    too_small.shares()[2].amount = 25 * COIN;
    QVERIFY(!too_small.validateShares().isEmpty());

    // Duplicate owner keys
    MnShareSession dup_owner{ValidSession(keys)};
    dup_owner.shares()[1].ownerAddress = dup_owner.shares()[0].ownerAddress;
    QVERIFY(!dup_owner.validateShares().isEmpty());

    // Duplicate refund scripts
    MnShareSession dup_refund{ValidSession(keys)};
    dup_refund.shares()[1].refundAddress = dup_refund.shares()[0].refundAddress;
    QVERIFY(!dup_refund.validateShares().isEmpty());

    // A reward script may not reuse a share owner address
    MnShareSession payee_reuse{ValidSession(keys)};
    payee_reuse.shares()[1].rewardAddress = payee_reuse.shares()[0].ownerAddress;
    QVERIFY(!payee_reuse.validateShares().isEmpty());

    // The early penalty must stay below the smallest share
    MnShareSession bad_penalty{ValidSession(keys)};
    bad_penalty.terms().earlyPenalty = 250 * COIN;
    QVERIFY(!bad_penalty.validateShares().isEmpty());

    // The early period is capped
    MnShareSession bad_period{ValidSession(keys)};
    bad_period.terms().earlyPeriodBlocks = CProRegTx::MAX_EARLY_PERIOD_BLOCKS + 1;
    QVERIFY(!bad_period.validateShares().isEmpty());
}

void MnShareSessionTests::envelopeRoundTrip()
{
    BasicTestingSetup setup{CBaseChainParams::REGTEST};
    std::vector<CKey> keys;
    MnShareSession session{ValidSession(keys)};
    session.shares()[0].label = "alice";
    session.setOperatorSecretHolder("bob");

    MnShareSession restored;
    QString error;
    QVERIFY2(restored.fromJson(session.toJsonString().toStdString(), error), qPrintable(error));
    QCOMPARE(restored.sessionId(), session.sessionId());
    QCOMPARE(restored.revision(), session.revision());
    QCOMPARE(int(restored.stage()), int(MnShareSession::Stage::Draft));
    QCOMPARE(restored.shares().size(), session.shares().size());
    QCOMPARE(restored.shares()[0].label, QString("alice"));
    QCOMPARE(restored.shares()[2].amount, 250 * COIN);
    QCOMPARE(restored.terms().earlyPenalty, 5 * COIN);
    QCOMPARE(restored.operatorSecretHolder(), QString("bob"));

    // A different network is a hard error
    UniValue json{session.toJson()};
    json.pushKV("network", "main");
    MnShareSession wrong_network;
    QVERIFY(!wrong_network.fromJson(json, error));
    QVERIFY(error.contains("main"));

    // Unknown stages are rejected
    json = session.toJson();
    json.pushKV("network", QString::fromStdString(Params().NetworkIDString()).toStdString());
    json.pushKV("stage", "warp");
    MnShareSession wrong_stage;
    QVERIFY(!wrong_stage.fromJson(json, error));
}

void MnShareSessionTests::penaltyPreviewMath()
{
    BasicTestingSetup setup{CBaseChainParams::REGTEST};
    const std::vector<CAmount> amounts{400 * COIN, 350 * COIN, 250 * COIN};
    const CAmount penalty{5 * COIN};
    const CAmount fee{100000};
    const uint32_t early_period{5000};
    const int registered{1000};

    // Early: the actor pays the penalty and fee, the others split the penalty
    // pro-rata with the remainder on the last non-actor share
    auto preview{MnShareSession::PenaltyPreviewFor(amounts, /*actor_index=*/0, penalty, early_period, fee,
                                                   /*at_height=*/2000, registered)};
    QVERIFY2(preview.valid, qPrintable(preview.error));
    QVERIFY(preview.early);
    QCOMPARE(preview.penalty, penalty);
    QCOMPARE(preview.penaltyFreeHeight, registered + int(early_period));
    QCOMPARE(preview.payouts.size(), amounts.size());
    QCOMPARE(preview.payouts[0], 400 * COIN - penalty - fee);
    // 350:250 pro-rata split of 5 DASH = 2.916... : 2.083...; floor to the
    // first share, remainder to the last
    const CAmount bonus_1{penalty * 350 / 600};
    QCOMPARE(preview.payouts[1], 350 * COIN + bonus_1);
    QCOMPARE(preview.payouts[2], 250 * COIN + (penalty - bonus_1));
    QCOMPARE(preview.payouts[0] + preview.payouts[1] + preview.payouts[2], 1000 * COIN - fee);

    // Past the boundary: no penalty
    preview = MnShareSession::PenaltyPreviewFor(amounts, /*actor_index=*/0, penalty, early_period, fee,
                                                /*at_height=*/registered + int(early_period), registered);
    QVERIFY2(preview.valid, qPrintable(preview.error));
    QVERIFY(!preview.early);
    QCOMPARE(preview.payouts[0], 400 * COIN - fee);
    QCOMPARE(preview.payouts[1], 350 * COIN);
    QCOMPARE(preview.payouts[2], 250 * COIN);
}

void MnShareSessionTests::signatureVerification()
{
    BasicTestingSetup setup{CBaseChainParams::REGTEST};
    std::vector<CKey> owner_keys;
    MnShareSession session{ValidSession(owner_keys)};
    // The operator key is a first-class term; register_shared_prepare would put
    // it in the payload, so the session must carry the matching pubkey
    CBLSSecretKey operator_secret;
    operator_secret.MakeNewKey();
    session.terms().operatorPubKey = QString::fromStdString(operator_secret.GetPublicKey().ToString(/*specificLegacyScheme=*/false));

    // Build the shared registration the way register_shared_prepare would:
    // the transaction spends the funding inputs directly and carries the
    // share table in its payload
    CProRegTx payload;
    payload.nVersion = ProTxVersion::ExtAddr;
    payload.nType = MnType::Regular;
    payload.collateralOutpoint = COutPoint(uint256(), 0);
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    for (const auto& share : session.shares()) {
        CTxDestination owner_dest{DecodeDestination(share.ownerAddress.toStdString())};
        CTxDestination refund_dest{DecodeDestination(share.refundAddress.toStdString())};
        payload.shares.emplace_back(share.amount, GetScriptForDestination(refund_dest), CScript(),
                                    ToKeyID(std::get<PKHash>(owner_dest)));
    }
    payload.vchJoinSigs.assign(payload.shares.size(), std::vector<unsigned char>(CPubKey::COMPACT_SIGNATURE_SIZE));
    payload.keyIDVoting = ToKeyID(std::get<PKHash>(DecodeDestination(session.terms().votingAddress.toStdString())));
    payload.pubKeyOperator.Set(operator_secret.GetPublicKey(), /*bls_legacy_scheme=*/false);
    payload.nOperatorReward = session.terms().operatorReward;
    payload.nEarlyPeriodBlocks = session.terms().earlyPeriodBlocks;
    payload.nEarlyPenalty = session.terms().earlyPenalty;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    tx.vin.emplace_back(COutPoint(uint256::ONE, 1));
    tx.vout.emplace_back(1000 * COIN, CScript() << std::vector<unsigned char>{'D', 'S', 'H', 'C'} << OP_DROP << OP_TRUE);
    SetTxPayload(tx, payload);

    const uint256 consent_hash{payload.MakeSharedRegConsentHash(CTransaction(tx))};
    const QString tx_hex{QString::fromStdString(EncodeHexTx(CTransaction(tx)))};

    QString error;
    QVERIFY2(session.freeze(tx_hex, QString::fromStdString(consent_hash.ToString()), 0, error), qPrintable(error));
    QCOMPARE(int(session.stage()), int(MnShareSession::Stage::Frozen));

    // freeze() rejects a consent hash that does not match the transaction
    std::vector<CKey> other_keys;
    MnShareSession session2{ValidSession(other_keys)};
    QVERIFY(!session2.freeze(tx_hex, QString::fromStdString(uint256::ONE.ToString()), 0, error));

    // A valid signature for share 1 is accepted
    std::vector<unsigned char> sig;
    QVERIFY(CHashSigner::SignHash(consent_hash, owner_keys[1], sig));
    const QString sig_b64{QString::fromStdString(EncodeBase64(sig))};
    QVERIFY2(session.addSignature(1, sig_b64, error), qPrintable(error));
    QCOMPARE(session.signedCount(), 1);
    QCOMPARE(session.missingIndexes(), (std::vector<int>{0, 2}));

    // A byte-identical duplicate is a silent success, and does not double-count
    QVERIFY(session.addSignature(1, sig_b64, error));
    QCOMPARE(session.signedCount(), 1);

    // The same signature under the wrong index is rejected
    QVERIFY(!session.addSignature(0, sig_b64, error));

    // A signature over a different digest (a stale draft) is rejected
    std::vector<unsigned char> stale_sig;
    QVERIFY(CHashSigner::SignHash(uint256::ONE, owner_keys[0], stale_sig));
    QVERIFY(!session.addSignature(0, QString::fromStdString(EncodeBase64(stale_sig)), error));
    QCOMPARE(session.signedCount(), 1);

    // Unfreezing discards the collected signatures and bumps the revision
    const int revision{session.revision()};
    session.unfreeze();
    QCOMPARE(int(session.stage()), int(MnShareSession::Stage::Draft));
    QCOMPARE(session.signedCount(), 0);
    QVERIFY(session.revision() > revision);

    // A payload that disagrees with the displayed share table must never freeze
    // or verify: the participant reviews m_shares but the wallet signs the
    // payload digest, so a mismatch is a money-loss trap
    MnShareSession tampered{ValidSession(owner_keys)};
    // The displayed table shows share 0 refunding to its own fresh address,
    // but the payload refunds share 0 to an attacker address
    CProRegTx evil{payload};
    CKey attacker;
    attacker.MakeNewKey(true);
    evil.shares[0].scriptRefund = GetScriptForDestination(PKHash(attacker.GetPubKey()));
    CMutableTransaction evil_tx{tx};
    SetTxPayload(evil_tx, evil);
    const uint256 evil_hash{evil.MakeSharedRegConsentHash(CTransaction(evil_tx))};
    const QString evil_hex{QString::fromStdString(EncodeHexTx(CTransaction(evil_tx)))};
    QVERIFY(!tampered.freeze(evil_hex, QString::fromStdString(evil_hash.ToString()), 0, error));

    // The same tampered payload delivered through a frozen envelope is rejected
    // on import. Re-freeze the matching session to get a well-formed envelope,
    // then swap in the attacker's payload.
    QVERIFY2(session.freeze(tx_hex, QString::fromStdString(consent_hash.ToString()), 0, error), qPrintable(error));
    UniValue envelope{session.toJson()};
    envelope.pushKV("protx", evil_hex.toStdString());
    envelope.pushKV("consentHash", evil_hash.ToString());
    MnShareSession imported;
    QVERIFY(!imported.fromJson(envelope, error));
}
