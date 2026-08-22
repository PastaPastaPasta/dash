// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sharedmndialogs.h>

#include <chainparams.h>
#include <coins.h>
#include <core_io.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <messagesigner.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/masternodemodel.h>
#include <qt/masternodewidgets.h>
#include <qt/mnsharesession.h>
#include <qt/protxsender.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace {

constexpr const char* SIG_ENVELOPE_TYPE{"dash-shared-mn-sigs"};
constexpr int SIG_ENVELOPE_VERSION{1};
//! Duffs; mirrors the RPC-side default of "protx dissolve" / "protx dissolve_prepare"
constexpr int DEFAULT_DISSOLVE_FEE{100000};
//! ~14 days of 2.5-minute blocks: below this distance to the penalty-free
//! boundary the unilateral tab suggests waiting instead
constexpr int NUDGE_BOUNDARY_BLOCKS{14 * 24 * 24};
constexpr qint64 SECONDS_PER_BLOCK{150};
constexpr qint64 MAX_ENVELOPE_FILE_BYTES{2 * 1024 * 1024};

QString OwnerAddress(const interfaces::MnShare& share)
{
    return QString::fromStdString(EncodeDestination(PKHash(share.keyIDOwner)));
}

QString ScriptAddress(const CScript& script)
{
    CTxDestination dest;
    if (!ExtractDestination(script, dest)) return {};
    return QString::fromStdString(EncodeDestination(dest));
}

QString FormatDash(CAmount amount)
{
    return BitcoinUnits::formatWithUnit(BitcoinUnits::Unit::DASH, amount);
}

void ShowStatusLabel(QLabel* label, const QString& message, bool error)
{
    label->setStyleSheet(GUIUtil::getThemedStyleQString(error ? GUIUtil::ThemedStyle::TS_ERROR
                                                              : GUIUtil::ThemedStyle::TS_SUCCESS));
    label->setText(message);
    label->setVisible(!message.isEmpty());
}

//! Fill `combo` with the shares whose owner key `wallet_model` can sign with;
//! the item data is the share index
void FillMyShareCombo(QComboBox* combo, const std::vector<interfaces::MnShare>& shares, WalletModel* wallet_model)
{
    if (!wallet_model || wallet_model->wallet().privateKeysDisabled()) return;
    for (size_t i = 0; i < shares.size(); ++i) {
        if (!wallet_model->wallet().isSpendable(PKHash(shares[i].keyIDOwner))) continue;
        combo->addItem(QCoreApplication::translate("SharedMnDialog", "Share #%1 — %2 — %3")
                           .arg(i)
                           .arg(FormatDash(shares[i].amount), OwnerAddress(shares[i])),
                       static_cast<int>(i));
    }
}

//! "block N (~date)" for a block `blocks_ahead` past the current tip
QString ApproxBlockDate(int blocks_ahead)
{
    const QDateTime when{QDateTime::currentDateTime().addSecs(static_cast<qint64>(blocks_ahead) * SECONDS_PER_BLOCK)};
    return QLocale().toString(when, QLocale::ShortFormat);
}

//! Recompute the digest the share owners of `pro_tx_hash` must sign for
//! `tx_hex`, mirroring the resolution in "protx shared_sign"
bool ComputeSharedSignHash(SharedSigCollector::Kind kind, const QString& pro_tx_hash, size_t share_count,
                           const QString& tx_hex, uint256& hash, QString& error)
{
    const auto Tr = [](const char* text) { return QCoreApplication::translate("SharedSigCollector", text); };

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, tx_hex.toStdString())) {
        error = Tr("The transaction is not valid transaction hex.");
        return false;
    }
    if (kind == SharedSigCollector::Kind::Dissolve) {
        if (tx.nType != TRANSACTION_PROVIDER_DISSOLVE) {
            error = Tr("The transaction is not a dissolution transaction.");
            return false;
        }
        const auto opt_ptx = GetTxPayload<CProDisTx>(tx);
        if (!opt_ptx) {
            error = Tr("The transaction payload is not deserializable.");
            return false;
        }
        if (QString::fromStdString(opt_ptx->proTxHash.ToString()).compare(pro_tx_hash, Qt::CaseInsensitive) != 0) {
            error = Tr("The transaction dissolves a different masternode.");
            return false;
        }
        if (!opt_ptx->vchSigs.empty()) {
            error = Tr("The transaction already carries signatures; expected the unsigned prepared transaction.");
            return false;
        }
        // Unanimous mode: the digest commits to one signature per share
        hash = opt_ptx->MakeSignHash(CTransaction(tx), static_cast<uint8_t>(share_count));
        return true;
    }
    if (tx.nType != TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR) {
        error = Tr("The transaction is not a shared registrar update.");
        return false;
    }
    const auto opt_ptx = GetTxPayload<CProUpSharedRegTx>(tx);
    if (!opt_ptx) {
        error = Tr("The transaction payload is not deserializable.");
        return false;
    }
    if (QString::fromStdString(opt_ptx->proTxHash.ToString()).compare(pro_tx_hash, Qt::CaseInsensitive) != 0) {
        error = Tr("The transaction updates a different masternode.");
        return false;
    }
    // SER_GETHASH serialization excludes the signature vector
    hash = ::SerializeHash(*opt_ptx);
    return true;
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// UpdateShareDialog
// ----------------------------------------------------------------------------

UpdateShareDialog::UpdateShareDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    MasternodeActionDialog(node, wallet_model, entry, parent),
    m_v24_active{node.isV24Active()},
    m_sender{new ProTxSender(node, this)}
{
    connect(m_sender, &ProTxSender::finished, this, [this](const ProTxResult& res) {
        setBusy(false);
        if (res.ok) {
            accept();
        } else {
            showError(res.message);
        }
    });
    auto* form = new QFormLayout();

    m_share_combo = new QComboBox(this);
    FillMyShareCombo(m_share_combo, entry.shares(), wallet_model);
    connect(m_share_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, &UpdateShareDialog::validate);
    form->addRow(tr("My share:"), m_share_combo);

    m_reward_edit = new QValidatedLineEdit(this);
    GUIUtil::setupAddressWidget(m_reward_edit, this);
    m_reward_edit->setPlaceholderText(tr("Leave blank to pay rewards to the immutable refund address again"));
    connect(m_reward_edit, &QLineEdit::textChanged, this, &UpdateShareDialog::validate);
    form->addRow(tr("New reward address:"), m_reward_edit);

    m_fee_source = new FeeSourcePicker(this);
    m_fee_source->setWalletModel(wallet_model);
    m_fee_source->refresh();
    connect(m_fee_source, qOverload<int>(&QComboBox::currentIndexChanged), this, &UpdateShareDialog::validate);
    form->addRow(tr("Fee source:"), m_fee_source);

    setupUi(tr("Update Share Reward Address"),
            tr("Change where one collateral share of this shared masternode receives its owner rewards. Only the share's owner key (held by this wallet) can change it; the share amount and refund address are immutable."),
            form, tr("Send Update"));

    if (!entry.isShared()) {
        showError(tr("This masternode is not shared and has no share table."));
    } else if (!m_v24_active) {
        showError(tr("Shared masternode maintenance requires the v24 hard fork to be active."));
    } else if (m_share_combo->count() == 0) {
        showError(tr("This wallet does not hold any of this masternode's share owner keys."));
    }
    validate();
}

void UpdateShareDialog::validate()
{
    bool ok{m_v24_active && m_share_combo->count() > 0};
    ok &= m_share_combo->currentIndex() >= 0;
    ok &= m_reward_edit->text().isEmpty() || m_reward_edit->isValid();
    ok &= !m_fee_source->selectedAddress().isEmpty();
    setOkValid(ok);
}

void UpdateShareDialog::submit()
{
    if (m_share_combo->currentIndex() < 0) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", QString::fromStdString(m_protx_hash.ToString()).toStdString());
    params.pushKV("shareIndex", m_share_combo->currentData().toInt());
    // An empty string reverts rewards to the refund script
    params.pushKV("rewardAddress", m_reward_edit->text().trimmed().toStdString());
    params.pushKV("feeSourceAddress", m_fee_source->selectedAddress().toStdString());

    clearError();
    setBusy(true);
    m_sender->execute("protx update_share", params, m_wallet_model);
}

// ----------------------------------------------------------------------------
// SharedSigCollector
// ----------------------------------------------------------------------------

SharedSigCollector::SharedSigCollector(Kind kind, const QString& pro_tx_hash,
                                       const std::vector<interfaces::MnShare>& shares, QWidget* parent) :
    QWidget(parent),
    m_kind{kind},
    m_protx_hash{pro_tx_hash},
    m_shares{shares}
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* review_hint = new QLabel(
        tr("Review the prepared action and its exact terms before signing. Your signature approves that action for "
           "your share."),
        this);
    review_hint->setWordWrap(true);
    layout->addWidget(review_hint);

    m_tx_view = new QPlainTextEdit(this);
    m_tx_view->setReadOnly(true);
    m_tx_view->setFont(GUIUtil::fixedPitchFont());
    m_tx_view->setMaximumHeight(90);
    m_tx_view->setPlaceholderText(tr("No prepared transaction yet — start the flow on one wallet, or import its session here."));
    layout->addWidget(m_tx_view);

    auto* buttons = new QHBoxLayout();
    m_copy_button = new QPushButton(tr("Copy session"), this);
    connect(m_copy_button, &QPushButton::clicked, this, &SharedSigCollector::copyEnvelope);
    buttons->addWidget(m_copy_button);
    m_save_button = new QPushButton(tr("Save session…"), this);
    connect(m_save_button, &QPushButton::clicked, this, &SharedSigCollector::saveEnvelope);
    buttons->addWidget(m_save_button);
    auto* paste_button = new QPushButton(tr("Import from clipboard"), this);
    connect(paste_button, &QPushButton::clicked, this, &SharedSigCollector::pasteEnvelope);
    buttons->addWidget(paste_button);
    auto* load_button = new QPushButton(tr("Import from file…"), this);
    connect(load_button, &QPushButton::clicked, this, &SharedSigCollector::loadEnvelope);
    buttons->addWidget(load_button);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_checklist_label = new QLabel(this);
    m_checklist_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_checklist_label);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    layout->addWidget(m_status_label);

    refreshUi();
}

QString SharedSigCollector::signHashHex() const
{
    if (m_tx_hex.isEmpty()) return {};
    uint256 hash;
    QString error;
    if (!ComputeSharedSignHash(m_kind, m_protx_hash, m_shares.size(), m_tx_hex, hash, error)) return {};
    return QString::fromStdString(hash.ToString());
}

bool SharedSigCollector::setTransaction(const QString& tx_hex, QString& error)
{
    const QString trimmed{tx_hex.trimmed()};
    uint256 hash;
    if (!ComputeSharedSignHash(m_kind, m_protx_hash, m_shares.size(), trimmed, hash, error)) {
        return false;
    }
    if (!m_tx_hex.isEmpty() && m_tx_hex != trimmed) {
        error = tr("The imported session carries a different transaction than the one already adopted — one of the two copies is stale. Discard the stale one and re-import.");
        return false;
    }
    if (m_tx_hex == trimmed) return true;
    m_tx_hex = trimmed;
    refreshUi();
    Q_EMIT changed();
    return true;
}

int SharedSigCollector::addSignatures(const UniValue& entries, QString& error)
{
    QStringList problems;
    if (m_tx_hex.isEmpty()) {
        error = tr("Import the session with its prepared transaction before importing signatures.");
        return 0;
    }
    uint256 sign_hash;
    if (QString hash_err; !ComputeSharedSignHash(m_kind, m_protx_hash, m_shares.size(), m_tx_hex, sign_hash, hash_err)) {
        error = hash_err;
        return 0;
    }
    if (!entries.isArray()) {
        error = tr("Expected an array of {shareIndex, signature} entries.");
        return 0;
    }

    int absorbed{0};
    for (const UniValue& entry : entries.getValues()) {
        if (!entry.isObject() || !entry.exists("shareIndex") || !entry.exists("signature")) {
            problems << tr("An entry is missing shareIndex or signature.");
            continue;
        }
        int index{-1};
        try {
            index = entry.find_value("shareIndex").getInt<int>();
        } catch (const std::exception&) {
            problems << tr("An entry has a non-numeric shareIndex.");
            continue;
        }
        if (index < 0 || static_cast<size_t>(index) >= m_shares.size()) {
            problems << tr("Share index %1 is out of range.").arg(index);
            continue;
        }
        const QString sig_b64{QString::fromStdString(entry.find_value("signature").isStr() ? entry.find_value("signature").get_str() : std::string{})};
        const auto sig{DecodeBase64(sig_b64.toStdString())};
        if (!sig || sig->size() != 65) {
            problems << tr("Share #%1: the signature is not a base64 65-byte compact signature.").arg(index);
            continue;
        }
        if (std::string str_error; !CHashSigner::VerifyHashCanonical(sign_hash, m_shares[index].keyIDOwner, *sig, str_error)) {
            problems << tr("Share #%1: the signature does not verify against the share owner key (%2) — it was probably made for an older version of the transaction. Ask the owner to re-sign the current one.")
                            .arg(index)
                            .arg(OwnerAddress(m_shares[index]));
            continue;
        }
        const auto it = m_sigs.find(index);
        if (it != m_sigs.end()) {
            if (DecodeBase64(it->second.toStdString()) == sig) continue; // byte-identical duplicate
            problems << tr("Share #%1: a different valid signature is already stored; keeping the existing one.").arg(index);
            continue;
        }
        m_sigs.emplace(index, sig_b64);
        ++absorbed;
    }
    error = problems.join(QLatin1Char('\n'));
    if (absorbed > 0) {
        refreshUi();
        Q_EMIT changed();
    }
    return absorbed;
}

UniValue SharedSigCollector::signaturesArray() const
{
    UniValue arr(UniValue::VARR);
    for (const auto& [index, sig] : m_sigs) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("shareIndex", index);
        entry.pushKV("signature", sig.toStdString());
        arr.push_back(entry);
    }
    return arr;
}

UniValue SharedSigCollector::envelopeJson() const
{
    UniValue json(UniValue::VOBJ);
    json.pushKV("type", SIG_ENVELOPE_TYPE);
    json.pushKV("version", SIG_ENVELOPE_VERSION);
    json.pushKV("network", Params().NetworkIDString());
    json.pushKV("kind", m_kind == Kind::Dissolve ? "dissolve" : "registrar");
    json.pushKV("proTxHash", m_protx_hash.toStdString());
    json.pushKV("tx", m_tx_hex.toStdString());
    json.pushKV("signatures", signaturesArray());
    return json;
}

bool SharedSigCollector::importEnvelope(const QString& text, QString& error)
{
    UniValue json;
    if (!json.read(text.trimmed().toStdString())) {
        error = tr("The import is not valid JSON.");
        return false;
    }
    try {
        return importEnvelopeChecked(json, error);
    } catch (const std::exception&) {
        // UniValue accessors throw on unexpectedly typed fields
        error = tr("The import is not a valid shared masternode signing session.");
        return false;
    }
}

bool SharedSigCollector::importEnvelopeChecked(const UniValue& json, QString& error)
{
    // A bare array is accepted as signature entries for the already-adopted transaction
    if (json.isArray()) {
        QString sig_error;
        const int absorbed{addSignatures(json, sig_error)};
        error = sig_error;
        return absorbed > 0 || sig_error.isEmpty();
    }
    if (!json.isObject() || !json.exists("type") || json.find_value("type").get_str() != SIG_ENVELOPE_TYPE) {
        error = tr("The import is not a shared masternode signing session.");
        return false;
    }
    const std::string our_network{Params().NetworkIDString()};
    const std::string their_network{json.exists("network") ? json.find_value("network").get_str() : std::string{}};
    if (their_network != our_network) {
        error = tr("The session belongs to the %1 network but this node runs %2.")
                    .arg(QString::fromStdString(their_network), QString::fromStdString(our_network));
        return false;
    }
    const std::string expected_kind{m_kind == Kind::Dissolve ? "dissolve" : "registrar"};
    if (!json.exists("kind") || json.find_value("kind").get_str() != expected_kind) {
        error = tr("The session is for a different kind of shared masternode transaction.");
        return false;
    }
    if (!json.exists("proTxHash") ||
        QString::fromStdString(json.find_value("proTxHash").get_str()).compare(m_protx_hash, Qt::CaseInsensitive) != 0) {
        error = tr("The session belongs to a different masternode.");
        return false;
    }
    if (json.exists("tx") && !json.find_value("tx").get_str().empty()) {
        if (!setTransaction(QString::fromStdString(json.find_value("tx").get_str()), error)) {
            return false;
        }
    }
    if (json.exists("signatures")) {
        QString sig_error;
        addSignatures(json.find_value("signatures"), sig_error);
        error = sig_error;
    }
    return true;
}

void SharedSigCollector::copyEnvelope()
{
    GUIUtil::setClipboard(QString::fromStdString(envelopeJson().write(/*prettyIndent=*/2)));
    showStatus(tr("Session copied to clipboard. Also save it to a file — the session is the only shared state, and any participant with the latest copy can continue it."), /*error=*/false);
}

void SharedSigCollector::saveEnvelope()
{
    const QString filename{GUIUtil::getSaveFileName(this, tr("Save Signing Session"), QString{},
                                                    tr("Shared masternode session (*.json)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        showStatus(tr("Could not write to %1.").arg(filename), /*error=*/true);
        return;
    }
    QTextStream out(&file);
    out << QString::fromStdString(envelopeJson().write(/*prettyIndent=*/2));
    file.close();
    showStatus(tr("Session saved to %1. Send it to the other participants.").arg(filename), /*error=*/false);
}

void SharedSigCollector::pasteEnvelope()
{
    QString error;
    const bool ok{importEnvelope(QApplication::clipboard()->text(), error)};
    showStatus(ok && error.isEmpty() ? tr("Session imported.") : error, /*error=*/!ok || !error.isEmpty());
}

void SharedSigCollector::loadEnvelope()
{
    const QString filename{GUIUtil::getOpenFileName(this, tr("Import Signing Session"), QString{},
                                                    tr("Shared masternode session (*.json);;All files (*)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MAX_ENVELOPE_FILE_BYTES) {
        showStatus(tr("Could not read %1.").arg(filename), /*error=*/true);
        return;
    }
    const QString text{QString::fromUtf8(file.readAll())};
    file.close();
    QString error;
    const bool ok{importEnvelope(text, error)};
    showStatus(ok && error.isEmpty() ? tr("Session imported.") : error, /*error=*/!ok || !error.isEmpty());
}

void SharedSigCollector::refreshUi()
{
    m_tx_view->setPlainText(m_tx_hex);
    QStringList lines;
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const bool signed_here{m_sigs.count(static_cast<int>(i)) > 0};
        lines << tr("Share #%1 — %2 — %3").arg(i).arg(OwnerAddress(m_shares[i]),
                                                      signed_here ? tr("signed") : tr("waiting for signature"));
    }
    lines << tr("%1 of %2 signatures collected").arg(m_sigs.size()).arg(m_shares.size());
    m_checklist_label->setText(lines.join(QLatin1Char('\n')));

    m_copy_button->setEnabled(hasTransaction());
    m_save_button->setEnabled(hasTransaction());
}

void SharedSigCollector::showStatus(const QString& message, bool error)
{
    ShowStatusLabel(m_status_label, message, error);
}

// ----------------------------------------------------------------------------
// SharedMnDialog
// ----------------------------------------------------------------------------

SharedMnDialog::SharedMnDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    QDialog(parent),
    m_node{node},
    m_wallet_model{wallet_model},
    m_protx_hash{entry.proTxHash()},
    m_shares{entry.shares()},
    m_early_penalty{entry.earlyPenalty()},
    m_early_period_blocks{entry.earlyPeriodBlocks()},
    m_registered_height{entry.registeredHeight()},
    m_v24_active{node.isV24Active()},
    m_sender{new ProTxSender(node, this)}
{
}

void SharedMnDialog::reject()
{
    // Esc or the window-manager close button must not dismiss the dialog
    // while runCommand() waits on an in-flight command; QDialog::closeEvent
    // keeps the window open when reject() leaves it visible
    if (m_busy) return;
    QDialog::reject();
}

bool SharedMnDialog::canSign() const
{
    return m_wallet_model != nullptr && !m_wallet_model->wallet().privateKeysDisabled();
}

bool SharedMnDialog::gateButton(QPushButton* button) const
{
    if (!m_wallet_model) {
        button->setEnabled(false);
        button->setToolTip(tr("No wallet is available."));
        return false;
    }
    if (m_wallet_model->wallet().privateKeysDisabled()) {
        button->setEnabled(false);
        button->setToolTip(tr("This wallet is watch-only and cannot sign transactions."));
        return false;
    }
    if (!m_v24_active) {
        button->setEnabled(false);
        button->setToolTip(tr("Shared masternode maintenance requires the v24 hard fork to be active."));
        return false;
    }
    return true;
}

bool SharedMnDialog::runCommand(const QString& method, const UniValue& params, ProTxResult& result, QString& error)
{
    if (m_busy) {
        error = tr("Another command is still running.");
        return false;
    }
    if (!canSign()) {
        error = tr("No wallet able to sign transactions is available.");
        return false;
    }

    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());
    if (!ctx.isValid()) {
        error = tr("Wallet unlock was cancelled.");
        return false;
    }

    m_busy = true;
    setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    ProTxResult outcome;
    QEventLoop loop;
    connect(m_sender, &ProTxSender::finished, &loop, [&](const ProTxResult& r) {
        outcome = r;
        loop.quit();
    });
    const bool started{m_sender->execute(method, params, m_wallet_model)};
    if (started) {
        // The unlock context cannot be moved, so wait here on the GUI thread to
        // keep it alive until the worker thread reports back
        loop.exec();
    }

    QApplication::restoreOverrideCursor();
    setEnabled(true);
    m_busy = false;

    if (!started) {
        error = tr("Another command is still running.");
        return false;
    }
    if (!outcome.ok) {
        error = outcome.message;
        return false;
    }
    result = outcome;
    return true;
}

bool SharedMnDialog::txInputsUnspent(const QString& tx_hex, QString& error)
{
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, tx_hex.toStdString())) {
        error = tr("The transaction is not valid transaction hex.");
        return false;
    }
    for (const CTxIn& txin : tx.vin) {
        Coin coin;
        if (m_node.getUnspentOutput(txin.prevout, coin)) continue;
        // Not in the confirmed UTXO set. A fee input funded from a
        // still-unconfirmed output of this wallet (e.g. fresh change selected
        // by the prepare call) is unspent and merely waiting for a
        // confirmation — only a positively spent input kills the session.
        if (m_wallet_model) {
            const auto coins{m_wallet_model->wallet().getCoins({txin.prevout})};
            if (coins.size() == 1 && !coins[0].is_spent && coins[0].depth_in_main_chain >= 0) continue;
        }
        error = tr("Input %1:%2 of the prepared transaction has been spent — this session is dead and cannot be continued. Start a new one.")
                    .arg(QString::fromStdString(txin.prevout.hash.ToString()))
                    .arg(txin.prevout.n);
        return false;
    }
    return true;
}

QComboBox* SharedMnDialog::makeMyShareCombo(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    FillMyShareCombo(combo, m_shares, m_wallet_model);
    if (combo->count() == 0) {
        combo->setToolTip(tr("This wallet does not hold any of this masternode's share owner keys."));
    }
    return combo;
}

bool SharedMnDialog::adoptPreparedTx(SharedSigCollector* collector, const ProTxResult& result, QLabel* status)
{
    QString error;
    const QString tx_hex{QString::fromStdString(result.value.find_value("tx").get_str())};
    if (!collector->setTransaction(tx_hex, error)) {
        ShowStatusLabel(status, error, /*error=*/true);
        return false;
    }
    // Cross-check our native sign-hash mirror against the node's answer, so a
    // drifted mirror can never let a bad signature through verification
    const QString rpc_hash{QString::fromStdString(result.value.find_value("signHash").get_str())};
    if (collector->signHashHex().compare(rpc_hash, Qt::CaseInsensitive) != 0) {
        ShowStatusLabel(status, tr("Internal error: the locally computed signing digest does not match the node's. Do not sign; please report this."), /*error=*/true);
        return false;
    }
    return true;
}

void SharedMnDialog::signWithWallet(SharedSigCollector* collector, QLabel* status)
{
    if (!collector->hasTransaction()) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", collector->txHex().toStdString());

    ProTxResult result;
    QString error;
    if (!runCommand(QStringLiteral("protx shared_sign"), params, result, error)) {
        ShowStatusLabel(status, error, /*error=*/true);
        return;
    }

    QString sig_error;
    const int absorbed{collector->addSignatures(result.value, sig_error)};
    const bool problems{!sig_error.isEmpty()};
    ShowStatusLabel(status,
                    problems ? sig_error
                             : tr("Signed %n share(s). Now copy or save the session and send it to the other participants.",
                                  nullptr, absorbed),
                    problems);
}

void SharedMnDialog::combineAndSubmit(SharedSigCollector* collector, QLabel* status, QLineEdit* result_edit,
                                      QPushButton* sign_button, QPushButton* combine_button,
                                      const QString& success_text, const QString& failure_hint)
{
    if (!collector->complete()) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", collector->txHex().toStdString());
    params.pushKV("signatures", collector->signaturesArray());
    params.pushKV("submit", true);

    ProTxResult result;
    QString error;
    if (!runCommand(QStringLiteral("protx shared_combine"), params, result, error)) {
        ShowStatusLabel(status, failure_hint.isEmpty() ? error : error + QLatin1Char('\n') + failure_hint,
                        /*error=*/true);
        return;
    }

    result_edit->setText(QString::fromStdString(result.value.get_str()));
    result_edit->setVisible(true);
    ShowStatusLabel(status, success_text, /*error=*/false);
    sign_button->setEnabled(false);
    combine_button->setEnabled(false);
}

bool SharedMnDialog::gateSessionButtons(SharedSigCollector* collector, QLabel* status, QPushButton* primary_button,
                                        QPushButton* sign_button, QPushButton* combine_button)
{
    // A8 liveness: a spent input makes the prepared transaction permanently
    // unbroadcastable, so the session is dead
    if (collector->hasTransaction()) {
        if (QString liveness_error; !txInputsUnspent(collector->txHex(), liveness_error)) {
            ShowStatusLabel(status, liveness_error, /*error=*/true);
            primary_button->setEnabled(false);
            sign_button->setEnabled(false);
            combine_button->setEnabled(false);
            return false;
        }
    }
    if (gateButton(sign_button)) {
        sign_button->setEnabled(collector->hasTransaction());
    }
    if (gateButton(combine_button)) {
        combine_button->setEnabled(collector->complete());
        if (!collector->complete()) {
            combine_button->setToolTip(tr("Every share must be signed first (%1 of %2 so far).")
                                           .arg(collector->signedCount())
                                           .arg(m_shares.size()));
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// DissolveDialog
// ----------------------------------------------------------------------------

DissolveDialog::DissolveDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry,
                               int current_height, QWidget* parent) :
    SharedMnDialog(node, wallet_model, entry, parent),
    m_current_height{current_height}
{
    setWindowTitle(tr("Dissolve Shared Masternode"));
    setMinimumWidth(760);

    auto* layout = new QVBoxLayout(this);

    auto* protx_label = new QLabel(tr("Masternode: %1").arg(m_protx_hash), this);
    protx_label->setWordWrap(true);
    protx_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(protx_label);

    // A ProDisTx spending the collateral in the block that created it would be
    // rejected; every flow below builds against the confirmed masternode list
    if (m_current_height <= m_registered_height) {
        auto* wait_label = new QLabel(tr("This masternode registered at height %1 and the chain is still at height %2. Wait for one confirmation of the registration, then reopen this dialog.")
                                          .arg(m_registered_height)
                                          .arg(m_current_height),
                                      this);
        wait_label->setWordWrap(true);
        layout->addWidget(wait_label);
    } else {
        auto* tabs = new QTabWidget(this);
        tabs->addTab(buildNowTab(), tr("Dissolve now"));
        tabs->addTab(buildUnanimousTab(), tr("Unanimous"));
        tabs->addTab(buildStandbyTab(), tr("Standby"));
        layout->addWidget(tabs);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    GUIUtil::updateFonts();
    if (m_now_actor) updateNowPreview();
    refreshUnanimousUi();
}

bool DissolveDialog::StandbyStored(const QString& pro_tx_hash)
{
    return QSettings().value(QStringLiteral("mnStandbyStored/") + pro_tx_hash, false).toBool();
}

QWidget* DissolveDialog::buildNowTab()
{
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);

    auto* intro = new QLabel(tr("Unilaterally dissolve this masternode right now: every participant's principal returns to its immutable refund address in one transaction. During the early period the dissolving participant pays the agreed penalty, which is redistributed pro-rata to the other shares."), tab);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    m_now_actor = makeMyShareCombo(tab);
    connect(m_now_actor, qOverload<int>(&QComboBox::currentIndexChanged), this, &DissolveDialog::updateNowPreview);
    form->addRow(tr("Dissolving share:"), m_now_actor);

    m_now_fee = new QSpinBox(tab);
    m_now_fee->setRange(1000, 10000000);
    m_now_fee->setValue(DEFAULT_DISSOLVE_FEE);
    m_now_fee->setSuffix(QLatin1Char(' ') + tr("duffs"));
    m_now_fee->setToolTip(tr("Transaction fee, paid from the dissolving share's refund."));
    connect(m_now_fee, qOverload<int>(&QSpinBox::valueChanged), this, &DissolveDialog::updateNowPreview);
    form->addRow(tr("Fee:"), m_now_fee);
    layout->addLayout(form);

    m_now_early_label = new QLabel(tab);
    m_now_early_label->setWordWrap(true);
    layout->addWidget(m_now_early_label);

    m_now_nudge_label = new QLabel(tab);
    m_now_nudge_label->setWordWrap(true);
    m_now_nudge_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_now_nudge_label->setVisible(false);
    layout->addWidget(m_now_nudge_label);

    m_now_table = new QTableWidget(0, 3, tab);
    m_now_table->setHorizontalHeaderLabels({tr("Share"), tr("Refund address"), tr("Payout")});
    m_now_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_now_table->verticalHeader()->setVisible(false);
    m_now_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_now_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_now_table->setMinimumHeight(120);
    layout->addWidget(m_now_table);

    m_now_error_label = new QLabel(tab);
    m_now_error_label->setWordWrap(true);
    m_now_error_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
    m_now_error_label->setVisible(false);
    layout->addWidget(m_now_error_label);

    m_now_accept = new QCheckBox(tab);
    m_now_accept->setVisible(false);
    connect(m_now_accept, &QCheckBox::toggled, this, &DissolveDialog::updateNowPreview);
    layout->addWidget(m_now_accept);

    auto* submit_row = new QHBoxLayout();
    m_now_submit = new QPushButton(tr("Dissolve now"), tab);
    connect(m_now_submit, &QPushButton::clicked, this, &DissolveDialog::submitNow);
    submit_row->addWidget(m_now_submit);
    submit_row->addStretch();
    layout->addLayout(submit_row);

    m_now_result = new QLineEdit(tab);
    m_now_result->setReadOnly(true);
    m_now_result->setPlaceholderText(tr("Transaction ID"));
    m_now_result->setVisible(false);
    layout->addWidget(m_now_result);

    m_now_status = new QLabel(tab);
    m_now_status->setWordWrap(true);
    m_now_status->setVisible(false);
    layout->addWidget(m_now_status);

    layout->addStretch();
    return tab;
}

void DissolveDialog::updateNowPreview()
{
    const bool usable{gateButton(m_now_submit) && m_now_actor->count() > 0};
    if (m_now_actor->count() == 0) {
        m_now_submit->setEnabled(false);
        m_now_submit->setToolTip(tr("This wallet does not hold any of this masternode's share owner keys."));
    }

    const int actor{m_now_actor->currentIndex() >= 0 ? m_now_actor->currentData().toInt() : -1};
    if (actor < 0) {
        m_now_table->setRowCount(0);
        return;
    }

    std::vector<CAmount> amounts;
    amounts.reserve(m_shares.size());
    for (const auto& share : m_shares) {
        amounts.push_back(share.amount);
    }
    const auto preview{MnShareSession::PenaltyPreviewFor(amounts, actor, m_early_penalty, m_early_period_blocks,
                                                         m_now_fee->value(), m_current_height, m_registered_height)};

    m_now_error_label->setVisible(!preview.valid);
    if (!preview.valid) {
        m_now_error_label->setText(preview.error);
        m_now_table->setRowCount(0);
        m_now_accept->setVisible(false);
        m_now_nudge_label->setVisible(false);
        m_now_early_label->clear();
        m_now_submit->setEnabled(false);
        return;
    }

    // Penalty-free once the transaction would confirm at the boundary height
    const int blocks_left{preview.penaltyFreeHeight - m_current_height - 1};
    if (preview.early) {
        m_now_early_label->setText(tr("Early exit: dissolving now pays the %1 penalty. A unilateral dissolution is penalty-free after block %2 (%3 from now, ~%4).")
                                       .arg(FormatDash(preview.penalty))
                                       .arg(preview.penaltyFreeHeight)
                                       .arg(MnShareSession::HumanEarlyPeriod(static_cast<uint32_t>(std::max(blocks_left, 0))),
                                            ApproxBlockDate(std::max(blocks_left, 0))));
        m_now_accept->setText(tr("I accept paying the %1 early-exit penalty").arg(FormatDash(preview.penalty)));
        m_now_accept->setVisible(true);
        m_now_nudge_label->setVisible(blocks_left <= NUDGE_BOUNDARY_BLOCKS);
        m_now_nudge_label->setText(tr("The penalty-free boundary is only %1 away. Consider waiting, or use the Unanimous tab: with every participant's signature the masternode dissolves penalty-free at any height.")
                                       .arg(MnShareSession::HumanEarlyPeriod(static_cast<uint32_t>(std::max(blocks_left, 0)))));
    } else {
        m_now_early_label->setText(tr("The early period has ended — dissolving now is penalty-free."));
        m_now_accept->setVisible(false);
        m_now_accept->setChecked(false);
        m_now_nudge_label->setVisible(false);
    }

    m_now_table->setRowCount(static_cast<int>(m_shares.size()));
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const bool is_actor{static_cast<int>(i) == actor};
        auto* share_item = new QTableWidgetItem(is_actor ? tr("Share #%1 (dissolving)").arg(i) : tr("Share #%1").arg(i));
        auto* refund_item = new QTableWidgetItem(ScriptAddress(m_shares[i].scriptRefund));
        auto* payout_item = new QTableWidgetItem(FormatDash(preview.payouts[i]));
        if (is_actor) {
            QFont bold{share_item->font()};
            bold.setBold(true);
            share_item->setFont(bold);
            refund_item->setFont(bold);
            payout_item->setFont(bold);
        }
        m_now_table->setItem(static_cast<int>(i), 0, share_item);
        m_now_table->setItem(static_cast<int>(i), 1, refund_item);
        m_now_table->setItem(static_cast<int>(i), 2, payout_item);
    }

    if (usable) {
        m_now_submit->setEnabled(!preview.early || m_now_accept->isChecked());
        m_now_submit->setToolTip(preview.early && !m_now_accept->isChecked() ? tr("Accept the early-exit penalty first.") : QString{});
    }
}

void DissolveDialog::submitNow()
{
    if (m_now_actor->currentIndex() < 0) return;
    const int actor{m_now_actor->currentData().toInt()};

    // Re-derive the early decision the RPC will make so payPenalty is
    // explicit — from a fresh tip height, not the height captured at dialog
    // open: crossing the penalty-free boundary while the dialog was open must
    // never submit an avoidable payPenalty=true.
    const auto is_early = [this](int height) -> bool {
        return static_cast<int64_t>(height) + 1 - m_registered_height < static_cast<int64_t>(m_early_period_blocks);
    };
    const auto refresh_on_boundary_crossing = [this, &is_early](int fresh_height) {
        const bool crossed{is_early(fresh_height) != is_early(m_current_height)};
        if (fresh_height != m_current_height) {
            m_current_height = fresh_height;
            updateNowPreview();
        }
        if (crossed) {
            ShowStatusLabel(m_now_status,
                            tr("The chain crossed the penalty-free boundary while this dialog was open. The preview above has been refreshed — review it and click Dissolve now again."),
                            /*error=*/false);
        }
        return crossed;
    };
    if (refresh_on_boundary_crossing(m_node.getNumBlocks())) return;
    const bool early{is_early(m_current_height)};
    if (early && !m_now_accept->isChecked()) return;

    if (QMessageBox::question(this, windowTitle(),
                              tr("Dissolve this shared masternode now? Every participant's principal returns to its refund address; the masternode stops earning immediately. This cannot be undone.")) !=
        QMessageBox::Yes) {
        return;
    }

    // The confirmation prompt can also stay open across the boundary
    if (refresh_on_boundary_crossing(m_node.getNumBlocks())) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("actorIndex", actor);
    params.pushKV("fee", static_cast<int64_t>(m_now_fee->value()));
    params.pushKV("submit", true);
    params.pushKV("payPenalty", early);

    ProTxResult result;
    QString error;
    if (!runCommand(QStringLiteral("protx dissolve"), params, result, error)) {
        ShowStatusLabel(m_now_status, error, /*error=*/true);
        return;
    }

    m_now_result->setText(QString::fromStdString(result.value.get_str()));
    m_now_result->setVisible(true);
    ShowStatusLabel(m_now_status, tr("The dissolution was sent successfully."), /*error=*/false);
    m_now_submit->setEnabled(false);
    m_now_actor->setEnabled(false);
    m_now_fee->setEnabled(false);
    m_now_accept->setEnabled(false);
}

QWidget* DissolveDialog::buildUnanimousTab()
{
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);

    auto* intro = new QLabel(tr("A unanimous dissolution is penalty-free at any height, even during the early period, but needs every participant's signature. One participant starts it here; the session below is then passed around (file or clipboard) until every share has signed, and anyone can submit the result."), tab);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    m_un_actor = makeMyShareCombo(tab);
    m_un_actor->setToolTip(tr("The share the transaction fee is deducted from."));
    form->addRow(tr("Fee-paying share:"), m_un_actor);
    m_un_fee = new QSpinBox(tab);
    m_un_fee->setRange(1000, 10000000);
    m_un_fee->setValue(DEFAULT_DISSOLVE_FEE);
    m_un_fee->setSuffix(QLatin1Char(' ') + tr("duffs"));
    form->addRow(tr("Fee:"), m_un_fee);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout();
    m_un_start = new QPushButton(tr("Start unanimous dissolution"), tab);
    connect(m_un_start, &QPushButton::clicked, this, &DissolveDialog::startUnanimous);
    buttons->addWidget(m_un_start);
    m_un_sign = new QPushButton(tr("Sign with this wallet"), tab);
    connect(m_un_sign, &QPushButton::clicked, this, &DissolveDialog::signUnanimous);
    buttons->addWidget(m_un_sign);
    m_un_combine = new QPushButton(tr("Combine && submit"), tab);
    connect(m_un_combine, &QPushButton::clicked, this, &DissolveDialog::combineUnanimous);
    buttons->addWidget(m_un_combine);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_un_collector = new SharedSigCollector(SharedSigCollector::Kind::Dissolve, m_protx_hash, m_shares, tab);
    connect(m_un_collector, &SharedSigCollector::changed, this, &DissolveDialog::refreshUnanimousUi);
    layout->addWidget(m_un_collector);

    m_un_result = new QLineEdit(tab);
    m_un_result->setReadOnly(true);
    m_un_result->setPlaceholderText(tr("Transaction ID"));
    m_un_result->setVisible(false);
    layout->addWidget(m_un_result);

    m_un_status = new QLabel(tab);
    m_un_status->setWordWrap(true);
    m_un_status->setVisible(false);
    layout->addWidget(m_un_status);

    layout->addStretch();
    return tab;
}

void DissolveDialog::refreshUnanimousUi()
{
    if (!m_un_start) return;
    if (!gateSessionButtons(m_un_collector, m_un_status, m_un_start, m_un_sign, m_un_combine)) return;
    if (gateButton(m_un_start)) {
        m_un_start->setEnabled(!m_un_collector->hasTransaction() && m_un_actor->count() > 0);
        if (m_un_actor->count() == 0) m_un_start->setToolTip(tr("This wallet does not hold any of this masternode's share owner keys."));
    }
}

void DissolveDialog::startUnanimous()
{
    if (m_un_actor->currentIndex() < 0) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("actorIndex", m_un_actor->currentData().toInt());
    params.pushKV("fee", static_cast<int64_t>(m_un_fee->value()));

    ProTxResult result;
    QString error;
    if (!runCommand(QStringLiteral("protx dissolve_prepare"), params, result, error)) {
        ShowStatusLabel(m_un_status, error, /*error=*/true);
        return;
    }

    if (!adoptPreparedTx(m_un_collector, result, m_un_status)) return;
    ShowStatusLabel(m_un_status, tr("Prepared. Sign with this wallet, then pass the session to the other participants."), /*error=*/false);
    m_un_fee->setEnabled(false);
    m_un_actor->setEnabled(false);
    refreshUnanimousUi();
}

void DissolveDialog::signUnanimous()
{
    signWithWallet(m_un_collector, m_un_status);
}

void DissolveDialog::combineUnanimous()
{
    combineAndSubmit(m_un_collector, m_un_status, m_un_result, m_un_sign, m_un_combine,
                     tr("The dissolution was sent successfully."), /*failure_hint=*/QString{});
}

QWidget* DissolveDialog::buildStandbyTab()
{
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);

    auto* intro = new QLabel(tr("A standby dissolution is a fully signed transaction kept offline instead of broadcast. Its validity is monotone: once valid it stays valid forever, so a standby never expires. Store both variants below together with your refund key backup, separately from the share owner key — if the owner key is ever lost, broadcasting a standby recovers your principal without anyone else's cooperation."), tab);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    m_sb_actor = makeMyShareCombo(tab);
    connect(m_sb_actor, qOverload<int>(&QComboBox::currentIndexChanged), tab, [this] {
        m_sb_view_a->clear();
        m_sb_view_b->clear();
    });
    form->addRow(tr("My share:"), m_sb_actor);
    m_sb_fee = new QSpinBox(tab);
    m_sb_fee->setRange(1000, 10000000);
    m_sb_fee->setValue(DEFAULT_DISSOLVE_FEE);
    m_sb_fee->setSuffix(QLatin1Char(' ') + tr("duffs"));
    form->addRow(tr("Fee:"), m_sb_fee);
    layout->addLayout(form);

    const int penalty_free_height{m_registered_height + static_cast<int>(m_early_period_blocks)};
    const int blocks_left{std::max(penalty_free_height - m_current_height - 1, 0)};

    // One column group per standby variant: generate button, transaction hex
    // view, copy/save row
    const auto build_variant = [this, tab, layout](bool pay_penalty, const QString& generate_text,
                                                   const QString& placeholder, const QString& copy_text,
                                                   const QString& save_text) {
        auto* generate = new QPushButton(generate_text, tab);
        connect(generate, &QPushButton::clicked, tab, [this, pay_penalty] { generateStandby(pay_penalty); });
        gateButton(generate);
        layout->addWidget(generate);

        auto* view = new QPlainTextEdit(tab);
        view->setReadOnly(true);
        view->setFont(GUIUtil::fixedPitchFont());
        view->setMaximumHeight(70);
        view->setPlaceholderText(placeholder);
        layout->addWidget(view);

        auto* row = new QHBoxLayout();
        auto* copy = new QPushButton(copy_text, tab);
        connect(copy, &QPushButton::clicked, tab, [view] {
            if (!view->toPlainText().isEmpty()) GUIUtil::setClipboard(view->toPlainText());
        });
        row->addWidget(copy);
        auto* save = new QPushButton(save_text, tab);
        connect(save, &QPushButton::clicked, tab, [this, pay_penalty] { saveStandby(pay_penalty); });
        row->addWidget(save);
        row->addStretch();
        layout->addLayout(row);
        return view;
    };

    m_sb_view_a = build_variant(/*pay_penalty=*/true,
                                tr("Generate Standby A — pays the %1 penalty, broadcastable immediately (it pays the penalty forever, even after the early period ends)")
                                    .arg(FormatDash(m_early_penalty)),
                                tr("Standby A transaction hex"), tr("Copy A"), tr("Save A to file…"));

    const QString b_when{blocks_left > 0 ? tr("valid only from block %1 (~%2)").arg(penalty_free_height).arg(ApproxBlockDate(blocks_left)) :
                                           tr("already valid — the early period has ended")};
    m_sb_view_b = build_variant(/*pay_penalty=*/false, tr("Generate Standby B — full principal, %1").arg(b_when),
                                tr("Standby B transaction hex"), tr("Copy B"), tr("Save B to file…"));

    auto* which = new QLabel(tr("Which one to broadcast later: B whenever the early period is over (full principal); A only when you cannot wait for the boundary."), tab);
    which->setWordWrap(true);
    layout->addWidget(which);

    m_sb_stored_label = new QLabel(tab);
    m_sb_stored_label->setWordWrap(true);
    m_sb_stored_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_SUCCESS));
    m_sb_stored_label->setVisible(StandbyStored(m_protx_hash));
    m_sb_stored_label->setText(tr("Both standby dissolutions were saved for this masternode."));
    layout->addWidget(m_sb_stored_label);

    m_sb_status = new QLabel(tab);
    m_sb_status->setWordWrap(true);
    m_sb_status->setVisible(false);
    layout->addWidget(m_sb_status);

    layout->addStretch();
    return tab;
}

void DissolveDialog::generateStandby(bool pay_penalty)
{
    if (m_sb_actor->currentIndex() < 0) {
        ShowStatusLabel(m_sb_status, tr("This wallet does not hold any of this masternode's share owner keys."), /*error=*/true);
        return;
    }

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("actorIndex", m_sb_actor->currentData().toInt());
    params.pushKV("fee", static_cast<int64_t>(m_sb_fee->value()));
    params.pushKV("submit", false);
    params.pushKV("payPenalty", pay_penalty);

    ProTxResult result;
    QString error;
    if (!runCommand(QStringLiteral("protx dissolve"), params, result, error)) {
        ShowStatusLabel(m_sb_status, error, /*error=*/true);
        return;
    }

    (pay_penalty ? m_sb_view_a : m_sb_view_b)->setPlainText(QString::fromStdString(result.value.get_str()));
    ShowStatusLabel(m_sb_status,
                    tr("Standby %1 generated — save it to a file now.").arg(pay_penalty ? QStringLiteral("A") : QStringLiteral("B")),
                    /*error=*/false);
}

void DissolveDialog::saveStandby(bool pay_penalty)
{
    const QString hex{(pay_penalty ? m_sb_view_a : m_sb_view_b)->toPlainText()};
    if (hex.isEmpty()) return;
    const QString variant{pay_penalty ? QStringLiteral("a") : QStringLiteral("b")};
    const QString filename{GUIUtil::getSaveFileName(this, tr("Save Standby Dissolution"),
                                                    QStringLiteral("standby-%1-%2.txt").arg(variant, m_protx_hash.left(8)),
                                                    tr("Text file (*.txt)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ShowStatusLabel(m_sb_status, tr("Could not write to %1.").arg(filename), /*error=*/true);
        return;
    }
    QTextStream out(&file);
    out << hex << '\n';
    file.close();

    // Persist the per-variant flag so saving A and B in different dialog
    // sessions still counts as "both stored"
    QSettings().setValue((pay_penalty ? QStringLiteral("mnStandbySavedA/") : QStringLiteral("mnStandbySavedB/")) + m_protx_hash,
                         true);
    ShowStatusLabel(m_sb_status,
                    tr("Standby %1 saved to %2.").arg(pay_penalty ? QStringLiteral("A") : QStringLiteral("B"), filename),
                    /*error=*/false);
    maybeMarkStandbyStored();
}

void DissolveDialog::maybeMarkStandbyStored()
{
    QSettings settings;
    if (!settings.value(QStringLiteral("mnStandbySavedA/") + m_protx_hash, false).toBool()) return;
    if (!settings.value(QStringLiteral("mnStandbySavedB/") + m_protx_hash, false).toBool()) return;
    settings.setValue(QStringLiteral("mnStandbyStored/") + m_protx_hash, true);
    m_sb_stored_label->setVisible(true);
}

// ----------------------------------------------------------------------------
// RotateSharedKeysDialog
// ----------------------------------------------------------------------------

RotateSharedKeysDialog::RotateSharedKeysDialog(interfaces::Node& node, WalletModel* wallet_model,
                                               const MasternodeEntry& entry, QWidget* parent) :
    SharedMnDialog(node, wallet_model, entry, parent)
{
    setWindowTitle(tr("Rotate Shared Masternode Keys"));
    setMinimumWidth(760);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(tr("Change this shared masternode's operator key and/or voting key. Like the original registration this needs every participant's signature: prepare here, sign, pass the session around, and finish below."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* protx_label = new QLabel(tr("Masternode: %1").arg(m_protx_hash), this);
    protx_label->setWordWrap(true);
    protx_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(protx_label);

    auto* banner = new QLabel(tr("Finish on this wallet: the prepared transaction's fee inputs belong to it, and combining re-signs those inputs, which only this wallet can do. Participants on other wallets only import, sign and export the session."), this);
    banner->setWordWrap(true);
    banner->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    layout->addWidget(banner);

    auto* form = new QFormLayout();
    m_operator_edit = new QLineEdit(this);
    m_operator_edit->setPlaceholderText(tr("Leave blank to keep the current operator key"));
    connect(m_operator_edit, &QLineEdit::textChanged, this, &RotateSharedKeysDialog::validateForm);
    form->addRow(tr("New operator public key:"), m_operator_edit);

    m_pose_warning = new QLabel(tr("Changing the operator key immediately PoSe-bans the masternode. It stays banned until the new operator sends a provider update service transaction."), this);
    m_pose_warning->setWordWrap(true);
    m_pose_warning->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    m_pose_warning->setVisible(false);
    form->addRow(m_pose_warning);

    m_voting_edit = new QValidatedLineEdit(this);
    GUIUtil::setupAddressWidget(m_voting_edit, this);
    m_voting_edit->setPlaceholderText(tr("Leave blank to keep the current voting address"));
    connect(m_voting_edit, &QLineEdit::textChanged, this, &RotateSharedKeysDialog::validateForm);
    form->addRow(tr("New voting address:"), m_voting_edit);

    m_fee_source = new FeeSourcePicker(this);
    m_fee_source->setWalletModel(wallet_model);
    m_fee_source->refresh();
    connect(m_fee_source, qOverload<int>(&QComboBox::currentIndexChanged), this, &RotateSharedKeysDialog::validateForm);
    form->addRow(tr("Fee source:"), m_fee_source);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout();
    m_prepare_button = new QPushButton(tr("Prepare on this wallet"), this);
    connect(m_prepare_button, &QPushButton::clicked, this, &RotateSharedKeysDialog::prepare);
    buttons->addWidget(m_prepare_button);
    m_sign_button = new QPushButton(tr("Sign with this wallet"), this);
    connect(m_sign_button, &QPushButton::clicked, this, &RotateSharedKeysDialog::signMine);
    buttons->addWidget(m_sign_button);
    m_combine_button = new QPushButton(tr("Combine && submit"), this);
    connect(m_combine_button, &QPushButton::clicked, this, &RotateSharedKeysDialog::combineAndSend);
    buttons->addWidget(m_combine_button);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_collector = new SharedSigCollector(SharedSigCollector::Kind::Registrar, m_protx_hash, m_shares, this);
    connect(m_collector, &SharedSigCollector::changed, this, &RotateSharedKeysDialog::validateForm);
    layout->addWidget(m_collector);

    m_result_edit = new QLineEdit(this);
    m_result_edit->setReadOnly(true);
    m_result_edit->setPlaceholderText(tr("Transaction ID"));
    m_result_edit->setVisible(false);
    layout->addWidget(m_result_edit);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    layout->addWidget(m_status_label);

    auto* close_buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close_buttons);

    GUIUtil::updateFonts();
    validateForm();
}

void RotateSharedKeysDialog::validateForm()
{
    m_pose_warning->setVisible(!m_operator_edit->text().trimmed().isEmpty());
    refreshButtons();
}

void RotateSharedKeysDialog::refreshButtons()
{
    if (!gateSessionButtons(m_collector, m_status_label, m_prepare_button, m_sign_button, m_combine_button)) return;
    if (gateButton(m_prepare_button)) {
        const QString operator_key{m_operator_edit->text().trimmed()};
        const bool operator_ok{operator_key.isEmpty() ||
                               (operator_key.size() == 96 &&
                                QRegularExpression(QStringLiteral("^[0-9a-fA-F]+$")).match(operator_key).hasMatch())};
        const bool any_change{!operator_key.isEmpty() || !m_voting_edit->text().isEmpty()};
        const bool voting_ok{m_voting_edit->text().isEmpty() || m_voting_edit->isValid()};
        m_prepare_button->setEnabled(!m_collector->hasTransaction() && any_change && operator_ok && voting_ok &&
                                     !m_fee_source->selectedAddress().isEmpty());
    }
}

void RotateSharedKeysDialog::prepare()
{
    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    // Empty strings keep the current values
    params.pushKV("operatorPubKey", m_operator_edit->text().trimmed().toStdString());
    params.pushKV("votingAddress", m_voting_edit->text().trimmed().toStdString());
    params.pushKV("feeSourceAddress", m_fee_source->selectedAddress().toStdString());

    ProTxResult result;
    QString error;
    if (!runCommand(QStringLiteral("protx update_shared_registrar_prepare"), params, result, error)) {
        ShowStatusLabel(m_status_label, error, /*error=*/true);
        return;
    }

    if (!adoptPreparedTx(m_collector, result, m_status_label)) return;

    m_operator_edit->setEnabled(false);
    m_voting_edit->setEnabled(false);
    m_fee_source->setEnabled(false);
    ShowStatusLabel(m_status_label,
                    tr("Prepared. Sign with this wallet, pass the session to the other participants, then finish here once every share has signed."),
                    /*error=*/false);
    refreshButtons();
}

void RotateSharedKeysDialog::signMine()
{
    signWithWallet(m_collector, m_status_label);
}

void RotateSharedKeysDialog::combineAndSend()
{
    // Combining re-signs the fee inputs, which only the preparing wallet can do
    combineAndSubmit(m_collector, m_status_label, m_result_edit, m_sign_button, m_combine_button,
                     tr("The key rotation was sent successfully."),
                     tr("If the error mentions signing the transaction inputs, this is not the wallet that prepared it — finish on that wallet."));
}
