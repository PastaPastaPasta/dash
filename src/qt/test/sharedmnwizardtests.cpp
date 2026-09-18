// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/sharedmnwizardtests.h>

#include <qt/bitcoinamountfield.h>
#include <qt/clientmodel.h>
#include <qt/mnsharesession.h>
#include <qt/optionsmodel.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sharedmncreatedialog.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <evo/sharedcollateral.h>
#include <evo/specialtx.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <key_io.h>
#include <messagesigner.h>
#include <primitives/transaction.h>
#include <node/context.h>
#include <script/descriptor.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <wallet/context.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTest>

#include <memory>
#include <algorithm>
#include <string>
#include <vector>

using wallet::AddWallet;
using wallet::CWallet;
using wallet::CreateMockWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WalletContext;
using wallet::WalletDescriptor;

namespace {
QString FreshP2PKHAddress(CKey& key_out)
{
    key_out.MakeNewKey(/*fCompressed=*/true);
    return QString::fromStdString(EncodeDestination(PKHash(key_out.GetPubKey())));
}

QString FakeTxid(char c)
{
    return QString(64, QChar::fromLatin1(c));
}

//! A named descriptor wallet that can spend from `key`, registered in the
//! node's wallet context. The role check only asks the wallet what it can
//! spend and what it is called, so no chain scan is needed.
std::shared_ptr<CWallet> MakeSpendingWallet(interfaces::Node& node, WalletContext& context, const std::string& name,
                                           const CKey& key)
{
    const auto wallet{std::make_shared<CWallet>(node.context()->chain.get(), node.context()->coinjoin_loader.get(),
                                                name, gArgs, CreateMockWalletDatabase())};
    wallet->LoadWallet();
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    {
        LOCK(wallet->cs_wallet);
        wallet->SetupDescriptorScriptPubKeyMans("", "");
        FlatSigningProvider provider;
        std::string error;
        std::unique_ptr<Descriptor> descriptor{
            Parse("combo(" + EncodeSecret(key) + ")", provider, error, /*require_checksum=*/false)};
        if (!descriptor) return nullptr;
        WalletDescriptor wallet_descriptor(std::move(descriptor), 0, 0, 1, 1);
        if (!wallet->AddWalletDescriptor(wallet_descriptor, provider, "", false)) return nullptr;
    }
    AddWallet(context, wallet);
    return wallet;
}

//! Unregisters the wallet even when a QVERIFY returns early: a wallet left in
//! the context makes the fixture teardown hang
class WalletGuard
{
public:
    WalletGuard(WalletContext& context, std::shared_ptr<CWallet> wallet) :
        m_context{context},
        m_wallet{std::move(wallet)}
    {
    }
    ~WalletGuard()
    {
        if (m_wallet) RemoveWallet(m_context, m_wallet, /*load_on_start=*/std::nullopt);
    }

private:
    WalletContext& m_context;
    std::shared_ptr<CWallet> m_wallet;
};

QString FreshOperatorPubKey()
{
    CBLSSecretKey secret;
    secret.MakeNewKey();
    return QString::fromStdString(secret.GetPublicKey().ToString(/*specificLegacyScheme=*/false));
}


//! A coordinator's invitation: three named shares with amounts and terms, but
//! no addresses and no funding yet
MnShareSession InvitationSession()
{
    MnShareSession session;
    CKey voting_key;
    const std::pair<const char*, CAmount> rows[]{
        {"alice", 400 * COIN},
        {"bob", 350 * COIN},
        {"carol", 250 * COIN},
    };
    for (const auto& [name, amount] : rows) {
        MnShareSession::Share share;
        share.label = QString::fromLatin1(name);
        share.amount = amount;
        session.shares().push_back(share);
    }
    session.terms().votingAddress = FreshP2PKHAddress(voting_key);
    session.terms().earlyPeriodBlocks = 5000;
    session.terms().earlyPenalty = 5 * COIN;
    session.setCoordinatorLabel(QStringLiteral("alice"));
    return session;
}

//! One participant's reply: their own share row filled in and their own
//! funding contribution recorded, at the revision the invitation was sent at
MnShareSession DraftReply(const MnShareSession& invitation, int share_index, const QString& txid,
                          CKey* owner_key_out = nullptr)
{
    MnShareSession reply{invitation};
    CKey owner_key;
    CKey refund_key;
    CKey change_key;
    reply.shares()[share_index].ownerAddress = FreshP2PKHAddress(owner_key);
    reply.shares()[share_index].refundAddress = FreshP2PKHAddress(refund_key);
    if (owner_key_out != nullptr) *owner_key_out = owner_key;

    MnShareSession::Contribution contribution;
    contribution.label = invitation.shares()[share_index].label;
    MnShareSession::Input input;
    input.txid = txid;
    input.vout = 0;
    contribution.inputs.push_back(input);
    contribution.hasChange = true;
    contribution.changeAddress = FreshP2PKHAddress(change_key);
    contribution.changeAmount = COIN;
    QString error;
    if (!reply.addContribution(contribution, error)) return invitation;

    // A reply answers the invitation rather than starting a new draft, so it
    // carries back exactly the revision it was sent at.
    MnShareSession normalised{invitation};
    UniValue json{reply.toJson()};
    json.pushKV("revision", invitation.revision());
    QString parse_error;
    if (!normalised.fromJson(json, parse_error)) return invitation;
    return normalised;
}

//! `draft` locked the way shared_register_prepare would lock it, with every
//! share owner's consent signature already collected
MnShareSession FrozenSession(const MnShareSession& draft, const std::vector<CKey>& owner_keys, bool sign_all)
{
    MnShareSession session{draft};
    CBLSSecretKey operator_secret;
    operator_secret.MakeNewKey();
    session.terms().operatorPubKey =
        QString::fromStdString(operator_secret.GetPublicKey().ToString(/*specificLegacyScheme=*/false));

    CProRegTx payload;
    payload.nVersion = ProTxVersion::ExtAddr;
    payload.nType = MnType::Regular;
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    for (const auto& share : session.shares()) {
        const CTxDestination owner{DecodeDestination(share.ownerAddress.toStdString())};
        const CTxDestination refund{DecodeDestination(share.refundAddress.toStdString())};
        payload.shares.emplace_back(share.amount, GetScriptForDestination(refund), CScript(),
                                    ToKeyID(std::get<PKHash>(owner)));
    }
    payload.vchJoinSigs.assign(payload.shares.size(), std::vector<unsigned char>(CPubKey::COMPACT_SIGNATURE_SIZE));
    payload.keyIDVoting =
        ToKeyID(std::get<PKHash>(DecodeDestination(session.terms().votingAddress.toStdString())));
    payload.pubKeyOperator.Set(operator_secret.GetPublicKey(), /*bls_legacy_scheme=*/false);
    payload.nOperatorReward = session.terms().operatorReward;
    payload.nEarlyPeriodBlocks = session.terms().earlyPeriodBlocks;
    payload.nEarlyPenalty = session.terms().earlyPenalty;

    // Exactly what shared_register_prepare builds: the draft's own funding
    // transaction with the shared collateral output appended
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, session.fundingTxHex().toStdString())) return draft;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    tx.vout.emplace_back(GetMnType(MnType::Regular).collat_amount, SharedCollateralScript());
    const int collateral_index{static_cast<int>(tx.vout.size() - 1)};
    payload.collateralOutpoint = COutPoint(uint256(), static_cast<uint32_t>(collateral_index));
    SetTxPayload(tx, payload);

    const uint256 consent_hash{payload.MakeSharedRegConsentHash(CTransaction(tx))};
    QString error;
    if (!session.freeze(QString::fromStdString(EncodeHexTx(CTransaction(tx))),
                        QString::fromStdString(consent_hash.ToString()), collateral_index, error)) {
        return draft;
    }
    if (!sign_all) return session;
    for (size_t i = 0; i < owner_keys.size() && i < session.shares().size(); ++i) {
        std::vector<unsigned char> signature;
        if (!CHashSigner::SignHash(consent_hash, owner_keys[i], signature)) return session;
        session.addSignature(static_cast<int>(i), QString::fromStdString(EncodeBase64(signature)), error);
    }
    return session;
}
} // anonymous namespace

void SharedMnWizardTests::coordinatorPageOrder()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    SharedMnCreateDialog dialog(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);

    // Nothing has been started, so only the landing page is reachable
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageLanding));
    QCOMPARE(dialog.m_order.size(), 1);

    dialog.m_role = SharedMnCreateDialog::Role::Coordinator;
    dialog.rebuildOrder();
    const QVector<SharedMnCreateDialog::Page> expected{
        SharedMnCreateDialog::PageLanding,    SharedMnCreateDialog::PageParticipants,
        SharedMnCreateDialog::PageSettings,   SharedMnCreateDialog::PageExitTerms,
        SharedMnCreateDialog::PageContribution, SharedMnCreateDialog::PageSecret,
        SharedMnCreateDialog::PageInvite,     SharedMnCreateDialog::PageApprovals,
        SharedMnCreateDialog::PageSignatures, SharedMnCreateDialog::PageComplete};
    QCOMPARE(dialog.m_order, expected);

    // The five pages that build the invitation are the numbered ones; what
    // comes after them depends on how fast the others answer
    dialog.goToPage(SharedMnCreateDialog::PageParticipants);
    QVERIFY(dialog.m_progress_label->text().contains(QStringLiteral("1")));
    QVERIFY(dialog.m_progress_label->text().contains(dialog.pageTitle(SharedMnCreateDialog::PageParticipants)));
    dialog.goToPage(SharedMnCreateDialog::PageInvite);
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageInvite));
    QVERIFY(dialog.m_progress_label->text().contains(QStringLiteral("5")));
    dialog.goToPage(SharedMnCreateDialog::PageApprovals);
    QCOMPARE(dialog.m_progress_label->text(), dialog.pageTitle(SharedMnCreateDialog::PageApprovals));

    // Terms are not locked, so the invitation cannot go out yet
    QVERIFY(!dialog.allDetailsCollected());
}

void SharedMnWizardTests::participantPagesAndRoleInference()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    SharedMnCreateDialog dialog(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);

    // An invitation where every row but one is already answered can only be
    // about the one row that is left
    MnShareSession invitation{InvitationSession()};
    MnShareSession partial{invitation};
    QString error;
    QVERIFY(partial.absorbDraftReply(DraftReply(invitation, 0, FakeTxid('1')), error) ==
            MnShareSession::MergeResult::Merged);
    QVERIFY(partial.absorbDraftReply(DraftReply(invitation, 2, FakeTxid('2')), error) ==
            MnShareSession::MergeResult::Merged);

    dialog.handleImportedText(partial.toJsonString());
    QCOMPARE(int(dialog.m_role), int(SharedMnCreateDialog::Role::Participant));
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageContribution));
    QCOMPARE(dialog.myShareIndex(), 1);
    QCOMPARE(dialog.m_session.sessionId(), invitation.sessionId());

    // A participant never sees the coordinator's drafting pages, but does get
    // the three waiting pages
    const QVector<SharedMnCreateDialog::Page> expected{
        SharedMnCreateDialog::PageLanding,     SharedMnCreateDialog::PageContribution,
        SharedMnCreateDialog::PageWaitTerms,   SharedMnCreateDialog::PageApprovals,
        SharedMnCreateDialog::PageWaitSigning, SharedMnCreateDialog::PageSignatures,
        SharedMnCreateDialog::PageWaitBroadcast, SharedMnCreateDialog::PageComplete};
    QCOMPARE(dialog.m_order, expected);

    // The masternode-wide terms are the coordinator's, and stay read-only here
    QVERIFY(!dialog.m_voting_edit->isEnabled());
    QVERIFY(!dialog.m_early_penalty_field->isEnabled());
    QCOMPARE(dialog.m_voting_edit->text(), invitation.terms().votingAddress);

    // Waiting pages name the coordinator the invitation identified
    QVERIFY(dialog.pageTitle(SharedMnCreateDialog::PageWaitTerms).contains(QStringLiteral("alice")));
}

void SharedMnWizardTests::coordinatorResumesSavedSession()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    WalletContext& context{*m_node.walletLoader().context()};

    CKey coordinator_key;
    coordinator_key.MakeNewKey(/*fCompressed=*/true);
    const auto wallet{MakeSpendingWallet(m_node, context, "coord", coordinator_key)};
    QVERIFY(wallet != nullptr);
    WalletGuard guard{context, wallet};

    OptionsModel options_model(m_node);
    bilingual_str options_error;
    QVERIFY2(options_model.Init(options_error), qPrintable(QString::fromStdString(options_error.translated)));
    ClientModel client_model(m_node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model);
    QCOMPARE(wallet_model.getWalletName(), QStringLiteral("coord"));

    // A session as the coordinator would have saved it from the invite page:
    // prepared by this wallet, alice's row is theirs, and bob and carol have
    // already answered
    MnShareSession invitation{InvitationSession()};
    invitation.setPrepareWallet(QStringLiteral("coord"));
    invitation.terms().operatorPubKey = FreshOperatorPubKey();
    CKey refund_key;
    invitation.shares()[0].ownerAddress =
        QString::fromStdString(EncodeDestination(PKHash(coordinator_key.GetPubKey())));
    invitation.shares()[0].refundAddress = FreshP2PKHAddress(refund_key);
    MnShareSession::Contribution mine;
    mine.label = QStringLiteral("alice");
    MnShareSession::Input input;
    input.txid = FakeTxid('1');
    mine.inputs.push_back(input);
    QString error;
    QVERIFY2(invitation.addContribution(mine, error), qPrintable(error));

    MnShareSession saved{invitation};
    for (const int share : {1, 2}) {
        QCOMPARE(int(saved.absorbDraftReply(DraftReply(invitation, share, FakeTxid('2' + share)), error)),
                 int(MnShareSession::MergeResult::Merged));
    }

    // Reopening it must not turn the coordinator into a participant: only the
    // coordinator can lock the terms, combine the approvals and broadcast
    SharedMnCreateDialog dialog(m_node, &wallet_model, /*parent=*/nullptr);
    dialog.handleImportedText(saved.toJsonString());
    QVERIFY2(dialog.m_error_label->text().isEmpty(), qPrintable(dialog.m_error_label->text()));
    QCOMPARE(int(dialog.m_role), int(SharedMnCreateDialog::Role::Coordinator));
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageInvite));
    QCOMPARE(dialog.myShareIndex(), 0);
    QVERIFY(dialog.allDetailsCollected());
    QCOMPARE(dialog.m_next_button->text(), QStringLiteral("Lock Terms"));

    // The operator key everybody was invited to is kept: a fresh one from the
    // key widget would invalidate the terms the others are answering
    QVERIFY(dialog.m_operator_key_from_import);
    dialog.syncTermsToSession();
    QCOMPARE(dialog.m_session.terms().operatorPubKey, saved.terms().operatorPubKey);

    // The same file in a wallet that cannot spend the coordinator's share is
    // still a participant's copy
    SharedMnCreateDialog participant(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);
    participant.handleImportedText(saved.toJsonString());
    QCOMPARE(int(participant.m_role), int(SharedMnCreateDialog::Role::Participant));
    QCOMPARE(int(participant.currentPage()), int(SharedMnCreateDialog::PageContribution));
}

void SharedMnWizardTests::coordinatorCanRetryOwnApproval()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    WalletContext& context{*m_node.walletLoader().context()};

    MnShareSession invitation{InvitationSession()};
    MnShareSession draft{invitation};
    std::vector<CKey> owner_keys(3);
    QString error;
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(int(draft.absorbDraftReply(DraftReply(invitation, i, FakeTxid('1' + i), &owner_keys[i]), error)),
                 int(MnShareSession::MergeResult::Merged));
    }
    // The terms are locked but the automatic approval at lock did not happen:
    // a cancelled unlock, or a "protx shared_sign" that failed
    const MnShareSession frozen{FrozenSession(draft, owner_keys, /*sign_all=*/false)};
    QCOMPARE(int(frozen.stage()), int(MnShareSession::Stage::Frozen));

    const auto wallet{MakeSpendingWallet(m_node, context, "coord", owner_keys[0])};
    QVERIFY(wallet != nullptr);
    WalletGuard guard{context, wallet};
    OptionsModel options_model(m_node);
    bilingual_str options_error;
    QVERIFY2(options_model.Init(options_error), qPrintable(QString::fromStdString(options_error.translated)));
    ClientModel client_model(m_node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model);

    SharedMnCreateDialog dialog(m_node, &wallet_model, /*parent=*/nullptr);
    dialog.m_session = frozen;
    dialog.m_role = SharedMnCreateDialog::Role::Coordinator;
    dialog.m_my_share = 0;
    dialog.rebuildOrder();
    dialog.goToPage(SharedMnCreateDialog::PageApprovals);

    // Approving again must stay offered: "Unlock terms" is the only other way
    // on, and it discards every approval already collected
    QVERIFY(dialog.needsOwnApproval());
    QVERIFY(!dialog.m_next_button->isHidden());
    QVERIFY2(dialog.m_next_button->isEnabled(), qPrintable(dialog.m_next_button->toolTip()));
    QCOMPARE(dialog.m_next_button->text(), QStringLiteral("Approve"));

    // Once our own approval is in, the page waits for the others again
    std::vector<unsigned char> signature;
    QVERIFY(CHashSigner::SignHash(uint256S(dialog.m_session.consentHash().toStdString()), owner_keys[0], signature));
    QVERIFY2(dialog.m_session.addSignature(0, QString::fromStdString(EncodeBase64(signature)), error),
             qPrintable(error));
    dialog.updateButtons();
    QVERIFY(!dialog.needsOwnApproval());
    QVERIFY(dialog.m_next_button->isHidden());

    // With every approval in, the button is the combine/sign retry again
    for (int i = 1; i < 3; ++i) {
        std::vector<unsigned char> reply;
        QVERIFY(CHashSigner::SignHash(uint256S(dialog.m_session.consentHash().toStdString()), owner_keys[i], reply));
        QVERIFY2(dialog.m_session.addSignature(i, QString::fromStdString(EncodeBase64(reply)), error),
                 qPrintable(error));
    }
    dialog.updateButtons();
    QVERIFY(dialog.allApproved());
    QVERIFY(!dialog.m_next_button->isHidden());
    QCOMPARE(dialog.m_next_button->text(), QStringLiteral("Retry"));
}

void SharedMnWizardTests::participantsPageGating()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    SharedMnCreateDialog dialog(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);
    dialog.m_role = SharedMnCreateDialog::Role::Coordinator;
    dialog.m_session = InvitationSession();
    dialog.m_my_share = -1;
    dialog.rebuildOrder();
    dialog.goToPage(SharedMnCreateDialog::PageParticipants);

    QString error;
    // Everything adds up, but nobody said which row is this wallet
    QVERIFY(!dialog.validatePage(SharedMnCreateDialog::PageParticipants, error));
    QVERIFY(!error.isEmpty());
    dialog.m_my_share = 0;
    QVERIFY2(dialog.validatePage(SharedMnCreateDialog::PageParticipants, error), qPrintable(error));

    // An unnamed participant is named in the error, not silently accepted
    dialog.m_session.shares()[1].label.clear();
    dialog.refreshShareTable();
    QVERIFY(!dialog.validatePage(SharedMnCreateDialog::PageParticipants, error));
    QVERIFY(error.contains(QStringLiteral("2")));

    // Two participants cannot share a name: the contributions are keyed by it
    dialog.m_session.shares()[1].label = dialog.m_session.shares()[0].label;
    dialog.refreshShareTable();
    QVERIFY(!dialog.validatePage(SharedMnCreateDialog::PageParticipants, error));
    QVERIFY(error.contains(dialog.m_session.shares()[0].label));

    // Amounts that do not add up to the collateral are refused with both totals
    dialog.m_session.shares()[1].label = QStringLiteral("bob");
    dialog.m_session.shares()[2].amount = 100 * COIN;
    dialog.refreshShareTable();
    QVERIFY(!dialog.validatePage(SharedMnCreateDialog::PageParticipants, error));
    QVERIFY(!error.isEmpty());
}

void SharedMnWizardTests::statusBoardAbsorbsRepliesInAnyOrder()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    SharedMnCreateDialog dialog(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);

    MnShareSession invitation{InvitationSession()};
    const MnShareSession alice{DraftReply(invitation, 0, FakeTxid('1'))};
    dialog.m_session = alice; // the coordinator answered its own invitation first
    dialog.m_role = SharedMnCreateDialog::Role::Coordinator;
    dialog.m_my_share = 0;
    dialog.rebuildOrder();
    dialog.goToPage(SharedMnCreateDialog::PageInvite);

    QVERIFY(!dialog.m_boards.empty());
    SharedMnStatusBoard* board{dialog.m_boards.front().second};
    QCOMPARE(board->rowCount(), 3);
    QCOMPARE(board->columnCount(), 4);
    QCOMPARE(int(board->cellState(0, 0)), int(SharedMnStatusBoard::State::Done));
    QCOMPARE(int(board->cellState(1, 0)), int(SharedMnStatusBoard::State::Pending));
    QVERIFY(board->rowName(0).contains(QStringLiteral("(you)")));

    // Carol answers before Bob: the board must not care
    dialog.handleImportedText(DraftReply(invitation, 2, FakeTxid('3')).toJsonString());
    QCOMPARE(int(board->cellState(2, 0)), int(SharedMnStatusBoard::State::Done));
    QCOMPARE(int(board->cellState(2, 1)), int(SharedMnStatusBoard::State::Done));
    QCOMPARE(int(board->cellState(1, 0)), int(SharedMnStatusBoard::State::Pending));
    QVERIFY(!dialog.allDetailsCollected());
    // The board names who answered; the line is not repeated under the page
    QVERIFY(board->lastReceived().contains(QStringLiteral("carol")));
    QVERIFY(board->lastReceived().contains(dialog.m_session.fingerprint()));
    QVERIFY(dialog.m_status_label->text().isEmpty());

    const MnShareSession bob{DraftReply(invitation, 1, FakeTxid('2'))};
    dialog.handleImportedText(bob.toJsonString());
    QCOMPARE(int(board->cellState(1, 0)), int(SharedMnStatusBoard::State::Done));
    QVERIFY(dialog.allDetailsCollected());
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageInvite));

    // Re-pasting a reply already in hand changes nothing and is not an error:
    // every other copy of the session would look stale if it bumped the
    // revision
    const int revision{dialog.m_session.revision()};
    dialog.handleImportedText(bob.toJsonString());
    QVERIFY(dialog.m_error_label->text().isEmpty());
    QCOMPARE(dialog.m_session.revision(), revision);

    // A second, different answer for a row that already has one is refused by
    // name rather than silently overwriting what the first reply sent
    dialog.handleImportedText(DraftReply(invitation, 1, FakeTxid('4')).toJsonString());
    QVERIFY(dialog.m_error_label->text().contains(QStringLiteral("bob")));
    QCOMPARE(dialog.m_session.revision(), revision);
}

void SharedMnWizardTests::pastedMessageRouting()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    SharedMnCreateDialog dialog(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);

    // Nothing recognisable: one sentence, and the dialog stays where it was
    dialog.handleImportedText(QStringLiteral("not a shared masternode message"));
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageLanding));
    QVERIFY(!dialog.m_error_label->text().isEmpty());
    QCOMPARE(int(dialog.m_role), int(SharedMnCreateDialog::Role::Undecided));

    // Somebody else's message kind: this dialog says where it belongs rather
    // than trying to parse it
    UniValue sigs(UniValue::VOBJ);
    sigs.pushKV("type", "dash-shared-mn-sigs");
    sigs.pushKV("kind", "dissolve");
    sigs.pushKV("proTxHash", uint256::ONE.ToString());
    dialog.handleImportedText(QString::fromStdString(sigs.write()));
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageLanding));
    QVERIFY(!dialog.m_error_label->text().isEmpty());
    QCOMPARE(int(dialog.m_role), int(SharedMnCreateDialog::Role::Undecided));

    // A message far larger than any envelope is refused before it is parsed
    dialog.handleImportedText(QString(3 * 1024 * 1024, QLatin1Char('x')));
    QVERIFY(dialog.m_error_label->text().contains(QStringLiteral("too large")));
    QCOMPARE(int(dialog.m_role), int(SharedMnCreateDialog::Role::Undecided));

    // Locked terms land on the approvals page, not on the drafting pages
    MnShareSession invitation{InvitationSession()};
    MnShareSession draft{invitation};
    std::vector<CKey> owner_keys(3);
    QString error;
    for (int i = 0; i < 3; ++i) {
        QVERIFY(draft.absorbDraftReply(DraftReply(invitation, i, FakeTxid('1' + i), &owner_keys[i]), error) ==
                MnShareSession::MergeResult::Merged);
    }
    const MnShareSession frozen{FrozenSession(draft, owner_keys, /*sign_all=*/false)};
    QCOMPARE(int(frozen.stage()), int(MnShareSession::Stage::Frozen));
    dialog.handleImportedText(frozen.toJsonString());
    QCOMPARE(int(dialog.currentPage()), int(SharedMnCreateDialog::PageApprovals));
    QCOMPARE(int(dialog.m_role), int(SharedMnCreateDialog::Role::Participant));
    QVERIFY(dialog.m_approvals_terms->text().contains(frozen.sessionCode()));

    // An envelope whose text was edited after it was copied still imports, but
    // the warning survives the page change: it stays on the status line while
    // the "Received …" line lives on the board
    UniValue edited{frozen.toJson()};
    edited.pushKV("fingerprint", "0000-0000");
    SharedMnCreateDialog warned(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);
    warned.handleImportedText(QString::fromStdString(edited.write(/*prettyIndent=*/2)));
    QCOMPARE(int(warned.currentPage()), int(SharedMnCreateDialog::PageApprovals));
    QVERIFY2(warned.m_status_label->text().contains(QStringLiteral("edited after it was copied")),
             qPrintable(warned.m_status_label->text()));
    QVERIFY(warned.m_status_label->isVisible() || !warned.m_status_label->text().isEmpty());
    const auto board{std::find_if(warned.m_boards.begin(), warned.m_boards.end(), [](const auto& entry) {
        return entry.first == SharedMnCreateDialog::PageApprovals;
    })};
    QVERIFY(board != warned.m_boards.end());
    QVERIFY(board->second->lastReceived().contains(QStringLiteral("Received")));
}

void SharedMnWizardTests::copyPutsFingerprintedEnvelopeOnClipboard()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    SharedMnCreateDialog dialog(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);
    dialog.m_session = DraftReply(InvitationSession(), 0, FakeTxid('1'));
    dialog.m_role = SharedMnCreateDialog::Role::Coordinator;
    dialog.m_my_share = 0;
    dialog.rebuildOrder();
    dialog.goToPage(SharedMnCreateDialog::PageInvite);

    dialog.copySession(QStringLiteral("Invitation"));
    const QString code{dialog.m_session.fingerprint()};
    QVERIFY(!code.isEmpty());
    QCOMPARE(dialog.m_sent_code, code);

    // The line under the buttons names the session and the exact message, so
    // two people can check by voice that they hold the same one
    QVERIFY(dialog.m_status_label->text().contains(dialog.m_session.sessionCode()));
    QVERIFY(dialog.m_status_label->text().contains(code));

    // What was copied is a complete envelope that parses back to this session
    MnShareSession round_trip;
    QString error;
    QVERIFY2(round_trip.fromJson(QApplication::clipboard()->text().toStdString(), error), qPrintable(error));
    QCOMPARE(round_trip.sessionId(), dialog.m_session.sessionId());
    QCOMPARE(round_trip.fingerprint(), code);
    QVERIFY(round_trip.importWarning().isEmpty());
}

void SharedMnWizardTests::unlockingDiscardsApprovals()
{
    TestingSetup test{CBaseChainParams::REGTEST};
    m_node.setContext(&test.m_node);
    SharedMnCreateDialog dialog(m_node, /*wallet_model=*/nullptr, /*parent=*/nullptr);

    MnShareSession invitation{InvitationSession()};
    MnShareSession draft{invitation};
    std::vector<CKey> owner_keys(3);
    QString error;
    for (int i = 0; i < 3; ++i) {
        QVERIFY(draft.absorbDraftReply(DraftReply(invitation, i, FakeTxid('1' + i), &owner_keys[i]), error) ==
                MnShareSession::MergeResult::Merged);
    }
    dialog.m_session = FrozenSession(draft, owner_keys, /*sign_all=*/true);
    dialog.m_role = SharedMnCreateDialog::Role::Coordinator;
    dialog.m_my_share = 0;
    dialog.rebuildOrder();
    dialog.goToPage(SharedMnCreateDialog::PageApprovals);

    QCOMPARE(dialog.m_session.signedCount(), 3);
    QVERIFY(dialog.allApproved());
    SharedMnStatusBoard* board{dialog.m_boards.at(1).second};
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(int(board->cellState(row, 2)), int(SharedMnStatusBoard::State::Done));
    }

    // Unlocking is the one way back, and it costs every approval collected so
    // far: they were made over a transaction that no longer exists
    dialog.m_session.unfreeze();
    dialog.refreshAll();
    QCOMPARE(dialog.m_session.signedCount(), 0);
    QVERIFY(!dialog.allApproved());
    QCOMPARE(int(dialog.m_session.stage()), int(MnShareSession::Stage::Draft));
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(int(board->cellState(row, 2)), int(SharedMnStatusBoard::State::Pending));
        // The details and coins everybody sent survive; only consent is gone
        QCOMPARE(int(board->cellState(row, 0)), int(SharedMnStatusBoard::State::Done));
        QCOMPARE(int(board->cellState(row, 1)), int(SharedMnStatusBoard::State::Done));
    }
    QVERIFY(dialog.allDetailsCollected());
}
