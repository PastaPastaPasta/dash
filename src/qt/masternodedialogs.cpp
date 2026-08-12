// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodedialogs.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <qt/guiutil.h>
#include <qt/masternodemodel.h>
#include <qt/masternodewidgets.h>
#include <qt/protxsender.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <string>

namespace {
//! Split a comma-separated ADDR:PORT list into trimmed, non-empty entries
QStringList SplitAddressList(const QString& text)
{
    QStringList ret;
    for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed{part.trimmed()};
        if (!trimmed.isEmpty()) ret << trimmed;
    }
    return ret;
}

UniValue AddressArray(const QStringList& addresses)
{
    UniValue arr(UniValue::VARR);
    for (const QString& address : addresses) {
        arr.push_back(address.toStdString());
    }
    return arr;
}

bool IsHexString(const QString& text, int length)
{
    if (text.size() != length) return false;
    for (const QChar c : text) {
        const char latin1{c.toLatin1()};
        const bool is_hex_digit{(latin1 >= '0' && latin1 <= '9') || (latin1 >= 'a' && latin1 <= 'f') ||
                                (latin1 >= 'A' && latin1 <= 'F')};
        if (!is_hex_digit) return false;
    }
    return true;
}
} // anonymous namespace

MasternodeActionDialog::MasternodeActionDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    QDialog(parent),
    m_node{node},
    m_wallet_model{wallet_model},
    m_protx_hash{entry.proTxHash()},
    m_is_shared{entry.isShared()},
    m_sender{new ProTxSender(node, this)}
{
}

void MasternodeActionDialog::setupUi(const QString& title, const QString& intro, QLayout* form, const QString& ok_text)
{
    setWindowTitle(title);
    setMinimumWidth(650);

    auto* layout = new QVBoxLayout(this);

    auto* intro_label = new QLabel(intro, this);
    intro_label->setWordWrap(true);
    layout->addWidget(intro_label);

    auto* protx_label = new QLabel(tr("Masternode: %1").arg(m_protx_hash), this);
    protx_label->setWordWrap(true);
    protx_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(protx_label);

    layout->addLayout(form);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
    m_status_label->setVisible(false);
    layout->addWidget(m_status_label);

    m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok_button = m_button_box->button(QDialogButtonBox::Ok);
    m_ok_button->setText(ok_text);
    if (!m_wallet_model) {
        m_ok_button->setToolTip(tr("No wallet is available."));
    } else if (m_wallet_model->wallet().privateKeysDisabled()) {
        m_ok_button->setToolTip(tr("This wallet is watch-only and cannot sign transactions."));
    }
    connect(m_button_box, &QDialogButtonBox::accepted, this, &MasternodeActionDialog::submit);
    connect(m_button_box, &QDialogButtonBox::rejected, this, &MasternodeActionDialog::reject);
    layout->addWidget(m_button_box);

    GUIUtil::updateFonts();
    validate();
}

FeeSourcePicker* MasternodeActionDialog::addFeeSourceRow(QFormLayout* form, const QString& note)
{
    auto* picker = new FeeSourcePicker(this);
    picker->setWalletModel(m_wallet_model);
    auto* fee_note = new QLabel(note, this);
    fee_note->setWordWrap(true);
    auto* fee_box = new QVBoxLayout();
    fee_box->addWidget(picker);
    fee_box->addWidget(fee_note);
    form->addRow(tr("Fee source:"), fee_box);
    return picker;
}

bool MasternodeActionDialog::canSign() const
{
    return m_wallet_model != nullptr && !m_wallet_model->wallet().privateKeysDisabled();
}

void MasternodeActionDialog::setOkValid(bool valid)
{
    m_valid = valid;
    if (m_ok_button) {
        m_ok_button->setEnabled(valid && canSign() && !m_busy);
    }
}

void MasternodeActionDialog::showError(const QString& message)
{
    m_status_label->setText(message);
    m_status_label->setVisible(true);
}

void MasternodeActionDialog::clearError()
{
    m_status_label->clear();
    m_status_label->setVisible(false);
}

void MasternodeActionDialog::reject()
{
    // runProTx pumps events in a nested loop; dismissing the dialog then would
    // suggest the in-flight transaction was cancelled when it is still sent
    if (m_busy) return;
    QDialog::reject();
}

void MasternodeActionDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (busy) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
    } else {
        QApplication::restoreOverrideCursor();
    }
    m_button_box->setEnabled(!busy);
    setOkValid(m_valid);
}

void MasternodeActionDialog::runProTx(const QString& method, const UniValue& params)
{
    if (m_busy || !canSign()) return;

    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());
    if (!ctx.isValid()) return;

    clearError();
    setBusy(true);

    ProTxResult result;
    QEventLoop loop;
    connect(m_sender, &ProTxSender::finished, &loop, [&](const ProTxResult& r) {
        result = r;
        loop.quit();
    });
    if (!m_sender->execute(method, params, m_wallet_model)) {
        setBusy(false);
        return;
    }
    // The unlock context cannot be moved, so wait here on the GUI thread to keep
    // it alive until the worker thread reports back
    loop.exec();
    setBusy(false);

    if (!result.ok) {
        showError(result.message);
        return;
    }

    const QString txid{result.value.isStr() ? QString::fromStdString(result.value.get_str()) :
                                              QString::fromStdString(result.value.write())};
    QMessageBox::information(this, windowTitle(),
                             tr("The transaction was sent successfully.") + "\n\n" + tr("Transaction ID: %1").arg(txid));
    accept();
}

UpdateServiceDialog::UpdateServiceDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    MasternodeActionDialog(node, wallet_model, entry, parent),
    m_type{entry.type()},
    m_operator_reward_pct{entry.operatorRewardPct()},
    m_v24_active{node.isV24Active()}
{
    auto* form = new QFormLayout();

    m_addrs_edit = new QLineEdit(this);
    m_addrs_edit->setPlaceholderText(tr("IP:PORT, comma-separated for multiple entries"));
    connect(m_addrs_edit, &QLineEdit::textChanged, this, &UpdateServiceDialog::validate);
    if (m_v24_active) {
        // Only the primary address is known here; prefilling it could silently
        // drop registered secondary addresses, since the submitted list replaces
        // the masternode's entire address set.
        auto* addrs_note = new QLabel(tr("Enter the full new address list; it replaces every currently registered "
                                         "address. The current addresses (primary: %1) are not prefilled.")
                                          .arg(entry.service()),
                                      this);
        addrs_note->setWordWrap(true);
        auto* addrs_box = new QVBoxLayout();
        addrs_box->addWidget(m_addrs_edit);
        addrs_box->addWidget(addrs_note);
        form->addRow(tr("Service addresses:"), addrs_box);
    } else {
        // Pre-v24 a masternode has exactly one core address, so prefilling is safe
        m_addrs_edit->setText(entry.service());
        m_addrs_edit->setToolTip(tr("Multiple addresses require the v24 hard fork to be active."));
        form->addRow(tr("Service address:"), m_addrs_edit);
    }

    m_operator_key_edit = new QLineEdit(this);
    m_operator_key_edit->setEchoMode(QLineEdit::Password);
    m_operator_key_edit->setPlaceholderText(tr("Operator BLS secret key (64 hex characters)"));
    connect(m_operator_key_edit, &QLineEdit::textChanged, this, &UpdateServiceDialog::validate);
    form->addRow(tr("Operator secret key:"), m_operator_key_edit);

    if (m_type == MnType::Evo) {
        m_platform_node_id_edit = new QLineEdit(this);
        m_platform_node_id_edit->setPlaceholderText(tr("Platform node ID (40 hex characters)"));
        connect(m_platform_node_id_edit, &QLineEdit::textChanged, this, &UpdateServiceDialog::validate);
        form->addRow(tr("Platform node ID:"), m_platform_node_id_edit);

        if (m_v24_active) {
            m_platform_p2p_edit = new QLineEdit(this);
            m_platform_p2p_edit->setPlaceholderText(tr("IP:PORT, comma-separated for multiple entries"));
            connect(m_platform_p2p_edit, &QLineEdit::textChanged, this, &UpdateServiceDialog::validate);
            form->addRow(tr("Platform P2P addresses:"), m_platform_p2p_edit);

            m_platform_https_edit = new QLineEdit(this);
            m_platform_https_edit->setPlaceholderText(tr("IP:PORT, comma-separated for multiple entries"));
            connect(m_platform_https_edit, &QLineEdit::textChanged, this, &UpdateServiceDialog::validate);
            form->addRow(tr("Platform HTTPS addresses:"), m_platform_https_edit);
        } else {
            // Pre-v24 a ProUpServTx only carries bare Platform port numbers,
            // applied to the service address
            m_platform_p2p_port_edit = new QSpinBox(this);
            m_platform_p2p_port_edit->setRange(1, 65535);
            m_platform_p2p_port_edit->setValue(26656);
            form->addRow(tr("Platform P2P port:"), m_platform_p2p_port_edit);

            m_platform_https_port_edit = new QSpinBox(this);
            m_platform_https_port_edit->setRange(1, 65535);
            m_platform_https_port_edit->setValue(443);
            form->addRow(tr("Platform HTTPS port:"), m_platform_https_port_edit);
        }
    }

    if (m_operator_reward_pct > 0) {
        m_operator_payout_edit = new QValidatedLineEdit(this);
        GUIUtil::setupAddressWidget(m_operator_payout_edit, this);
        m_operator_payout_edit->setPlaceholderText(tr("Leave blank to keep the current operator payout address"));
        connect(m_operator_payout_edit, &QLineEdit::textChanged, this, &UpdateServiceDialog::validate);
        form->addRow(tr("Operator payout address:"), m_operator_payout_edit);
    }

    m_fee_source = addFeeSourceRow(form, m_is_shared ?
                                             tr("Required: a shared masternode has no payout address of its own to pay the transaction fee from.") :
                                             tr("Optional: when no address is selected the fee is paid from the operator payout or masternode payout address."));

    setupUi(m_type == MnType::Evo ? tr("Update Evonode Service") : tr("Update Masternode Service"),
            tr("Update the network addresses this masternode is reachable at. Signing requires the operator secret key. A successful update also revives a PoSe-banned masternode."),
            form, tr("Send Update"));
}

void UpdateServiceDialog::validate()
{
    const QStringList addrs{SplitAddressList(m_addrs_edit->text())};
    bool ok{!addrs.isEmpty()};
    if (!m_v24_active && addrs.size() > 1) ok = false;
    ok &= IsHexString(m_operator_key_edit->text().trimmed(), 64);
    if (m_operator_payout_edit) {
        ok &= m_operator_payout_edit->text().isEmpty() || m_operator_payout_edit->isValid();
    }
    if (m_type == MnType::Evo) {
        ok &= IsHexString(m_platform_node_id_edit->text().trimmed(), 40);
        if (m_v24_active) {
            ok &= !SplitAddressList(m_platform_p2p_edit->text()).isEmpty();
            ok &= !SplitAddressList(m_platform_https_edit->text()).isEmpty();
        }
        // Pre-v24 the port spinboxes enforce a valid range on their own
    }
    setOkValid(ok);
}

void UpdateServiceDialog::submit()
{
    const QString fee_source{m_fee_source->selectedAddress()};
    if (m_is_shared && fee_source.isEmpty()) {
        showError(tr("Select a fee source address. A shared masternode requires one."));
        return;
    }

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("coreP2PAddrs", AddressArray(SplitAddressList(m_addrs_edit->text())));
    params.pushKV("operatorKey", m_operator_key_edit->text().trimmed().toStdString());
    if (m_type == MnType::Evo) {
        params.pushKV("platformNodeID", m_platform_node_id_edit->text().trimmed().toStdString());
        if (m_v24_active) {
            params.pushKV("platformP2PAddrs", AddressArray(SplitAddressList(m_platform_p2p_edit->text())));
            params.pushKV("platformHTTPSAddrs", AddressArray(SplitAddressList(m_platform_https_edit->text())));
        } else {
            // Pre-v24 ProTx versions only accept bare port numbers here
            params.pushKV("platformP2PAddrs", m_platform_p2p_port_edit->value());
            params.pushKV("platformHTTPSAddrs", m_platform_https_port_edit->value());
        }
    }
    if (m_operator_payout_edit && !m_operator_payout_edit->text().isEmpty()) {
        params.pushKV("operatorPayoutAddress", m_operator_payout_edit->text().trimmed().toStdString());
    }
    if (!fee_source.isEmpty()) {
        params.pushKV("feeSourceAddress", fee_source.toStdString());
    }

    runProTx(m_type == MnType::Evo ? "protx update_service_evo" : "protx update_service", params);
}

UpdateRegistrarDialog::UpdateRegistrarDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    MasternodeActionDialog(node, wallet_model, entry, parent)
{
    auto* form = new QFormLayout();

    m_operator_pubkey_edit = new QLineEdit(this);
    m_operator_pubkey_edit->setPlaceholderText(tr("Leave blank to keep the current operator key"));
    connect(m_operator_pubkey_edit, &QLineEdit::textChanged, this, &UpdateRegistrarDialog::validate);
    form->addRow(tr("Operator public key:"), m_operator_pubkey_edit);

    auto* warning_label = new QLabel(tr("Changing the operator key immediately PoSe-bans the masternode. It stays banned until the new operator sends a provider update service transaction."), this);
    warning_label->setWordWrap(true);
    warning_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING));
    form->addRow(warning_label);

    m_voting_edit = new QValidatedLineEdit(this);
    GUIUtil::setupAddressWidget(m_voting_edit, this);
    m_voting_edit->setPlaceholderText(tr("Leave blank to keep the current voting address"));
    connect(m_voting_edit, &QLineEdit::textChanged, this, &UpdateRegistrarDialog::validate);
    form->addRow(tr("Voting address:"), m_voting_edit);

    m_payout_edit = new QValidatedLineEdit(this);
    GUIUtil::setupAddressWidget(m_payout_edit, this);
    m_payout_edit->setPlaceholderText(tr("Leave blank to keep the current payout address"));
    connect(m_payout_edit, &QLineEdit::textChanged, this, &UpdateRegistrarDialog::validate);
    form->addRow(tr("Payout address:"), m_payout_edit);

    m_fee_source = addFeeSourceRow(form, tr("Optional: when no address is selected the fee is paid from the masternode payout address."));

    setupUi(tr("Update Masternode Registrar"),
            tr("Update the operator key, voting address or payout address of this masternode. The masternode owner key must be present in this wallet. Fields left blank keep their current value."),
            form, tr("Send Update"));

    // Shared masternodes manage keys and payouts per share; a ProUpRegTx does not
    // apply to them. Callers should not offer this dialog for shared rows, but
    // guard here as well.
    if (m_is_shared) {
        m_operator_pubkey_edit->setEnabled(false);
        m_voting_edit->setEnabled(false);
        m_payout_edit->setEnabled(false);
        m_fee_source->setEnabled(false);
        showError(tr("This masternode is shared. Shared masternodes are updated per share and cannot use a registrar update."));
    }
}

void UpdateRegistrarDialog::validate()
{
    if (m_is_shared) {
        setOkValid(false);
        return;
    }
    const QString operator_pubkey{m_operator_pubkey_edit->text().trimmed()};
    const bool any_change{!operator_pubkey.isEmpty() || !m_voting_edit->text().isEmpty() || !m_payout_edit->text().isEmpty()};
    bool ok{any_change};
    if (!operator_pubkey.isEmpty()) ok &= IsHexString(operator_pubkey, 96);
    ok &= m_voting_edit->text().isEmpty() || m_voting_edit->isValid();
    ok &= m_payout_edit->text().isEmpty() || m_payout_edit->isValid();
    setOkValid(ok);
}

void UpdateRegistrarDialog::submit()
{
    if (m_is_shared) return;

    // The three update fields are mandatory RPC arguments; an empty string keeps
    // the current value.
    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("operatorPubKey", m_operator_pubkey_edit->text().trimmed().toStdString());
    params.pushKV("votingAddress", m_voting_edit->text().trimmed().toStdString());
    params.pushKV("payoutAddress", m_payout_edit->text().trimmed().toStdString());
    const QString fee_source{m_fee_source->selectedAddress()};
    if (!fee_source.isEmpty()) {
        params.pushKV("feeSourceAddress", fee_source.toStdString());
    }

    runProTx("protx update_registrar", params);
}

RevokeDialog::RevokeDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent) :
    MasternodeActionDialog(node, wallet_model, entry, parent)
{
    auto* form = new QFormLayout();

    // Values follow CProUpRevTx::RevocationReason
    m_reason_combo = new QComboBox(this);
    m_reason_combo->addItem(tr("Not specified"), 0);
    m_reason_combo->addItem(tr("Termination of service"), 1);
    m_reason_combo->addItem(tr("Compromised keys"), 2);
    m_reason_combo->addItem(tr("Change of keys"), 3);
    form->addRow(tr("Reason:"), m_reason_combo);

    m_operator_key_edit = new QLineEdit(this);
    m_operator_key_edit->setEchoMode(QLineEdit::Password);
    m_operator_key_edit->setPlaceholderText(tr("Operator BLS secret key (64 hex characters)"));
    connect(m_operator_key_edit, &QLineEdit::textChanged, this, &RevokeDialog::validate);
    form->addRow(tr("Operator secret key:"), m_operator_key_edit);

    m_fee_source = addFeeSourceRow(form, m_is_shared ?
                                             tr("Required: a shared masternode has no payout address of its own to pay the transaction fee from.") :
                                             tr("Optional: when no address is selected the fee is paid from the operator payout or masternode payout address."));

    setupUi(tr("Revoke Masternode Service"),
            tr("Revoking clears the masternode's service address and PoSe-bans it until a new operator key is set with a registrar update. The collateral is not affected. This is mainly needed before re-registering with the same IP address or operator key."),
            form, tr("Revoke"));
}

void RevokeDialog::validate()
{
    setOkValid(IsHexString(m_operator_key_edit->text().trimmed(), 64));
}

void RevokeDialog::submit()
{
    const QString fee_source{m_fee_source->selectedAddress()};
    if (m_is_shared && fee_source.isEmpty()) {
        showError(tr("Select a fee source address. A shared masternode requires one."));
        return;
    }

    UniValue params(UniValue::VOBJ);
    params.pushKV("proTxHash", m_protx_hash.toStdString());
    params.pushKV("operatorKey", m_operator_key_edit->text().trimmed().toStdString());
    params.pushKV("reason", m_reason_combo->currentData().toInt());
    if (!fee_source.isEmpty()) {
        params.pushKV("feeSourceAddress", fee_source.toStdString());
    }

    runProTx("protx revoke", params);
}

ShowOperatorKeyDialog::ShowOperatorKeyDialog(WalletModel* wallet_model, const QString& protx_hash,
                                             const std::vector<unsigned char>& operator_pubkey, QWidget* parent) :
    QDialog(parent)
{
    using namespace MasternodeWidgetUtil;

    setWindowTitle(tr("Operator Key"));
    setMinimumWidth(650);

    // Read the key back out of the wallet first: without it this dialog has
    // nothing to show but the reason why
    std::vector<unsigned char> secret;
    std::string derivation_path;
    QString failure;
    if (wallet_model == nullptr) {
        failure = tr("No wallet is available.");
    } else {
        WalletModel::UnlockContext ctx(wallet_model->requestUnlock());
        std::string error;
        if (!ctx.isValid()) {
            failure = tr("The wallet stayed locked, so the operator secret key cannot be shown.");
        } else if (!wallet_model->wallet().getMasternodeOperatorKey(operator_pubkey, secret, derivation_path,
                                                                    error)) {
            failure = tr("This wallet could not produce the operator secret key: %1")
                          .arg(QString::fromStdString(error));
        }
    }

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(TITLE_SPACING);

    auto* protx_label = new QLabel(tr("Masternode: %1").arg(protx_hash), this);
    protx_label->setWordWrap(true);
    protx_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(protx_label);

    if (failure.isEmpty()) {
        const QString public_hex{QString::fromStdString(HexStr(operator_pubkey))};
        const QString secret_hex{QString::fromStdString(HexStr(secret))};
        layout->addWidget(makeHint(tr("Operator public key"), this));
        layout->addWidget(makeCopyableValue(chunked(public_hex), public_hex, this));
        layout->addSpacing(GROUP_SPACING);
        layout->addWidget(makeHint(tr("Operator secret key"), this));
        layout->addWidget(makeCopyableValue(chunked(secret_hex), secret_hex, this));
        layout->addSpacing(GROUP_SPACING);
        layout->addWidget(makeHint(tr("Add this line to dash.conf on your masternode server:"), this));
        const QString conf_line{QString("masternodeblsprivkey=%1").arg(secret_hex)};
        layout->addWidget(makeCopyableValue(QString("masternodeblsprivkey=%1").arg(chunked(secret_hex)), conf_line,
                                            this));
        layout->addSpacing(GROUP_SPACING);
        layout->addWidget(makeHint(derivation_path.empty() ?
                                       tr("This key is stored in this wallet — include it in your wallet backup.") :
                                       tr("This key is derived from this wallet's recovery phrase (path %1), so "
                                          "restoring that phrase brings it back.")
                                           .arg(QString::fromStdString(derivation_path)),
                                   this));
    } else {
        auto* failure_label = new QLabel(failure, this);
        failure_label->setWordWrap(true);
        failure_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
        layout->addWidget(failure_label);
    }

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(button_box);

    GUIUtil::updateFonts();
}
