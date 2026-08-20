// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sharedmncreatedialog.h>

#include <coins.h>
#include <core_io.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <uint256.h>
#include <univalue.h>
#include <wallet/coincontrol.h>

#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/optionsmodel.h>
#include <qt/protxsender.h>
#include <qt/sendcoinsrecipient.h>
#include <qt/walletmodel.h>
#include <qt/walletmodeltransaction.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

namespace {
static QStringList parseServiceList(const QString& input, QString& err)
{
    err.clear();
    QStringList ret;
    QSet<QString> seen;
    const QStringList tokens{input.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)};
    for (const QString& token : tokens) {
        const std::optional<CService> service{
            Lookup(token.toStdString(), Params().GetDefaultPort(), /*fAllowLookup=*/false)};
        if (!service.has_value() || !service->IsValid()) {
            err = QObject::tr("Invalid address \"%1\". Expected format: IP:PORT (e.g. 1.2.3.4:%2).")
                      .arg(token)
                      .arg(Params().GetDefaultPort());
            return {};
        }
        const QString normalized{QString::fromStdString(service->ToStringAddrPort())};
        if (seen.contains(normalized)) {
            err = QObject::tr("Duplicate address \"%1\".").arg(normalized);
            return {};
        }
        seen.insert(normalized);
        ret << normalized;
    }
    return ret;
}


constexpr int COL_LABEL{0};
constexpr int COL_AMOUNT{1};
constexpr int COL_OWNER{2};
constexpr int COL_REFUND{3};
constexpr int COL_REWARD{4};

constexpr int FEE_UTXO_TXID_ROLE{Qt::UserRole};
constexpr int FEE_UTXO_VOUT_ROLE{Qt::UserRole + 1};
constexpr int FEE_UTXO_VALUE_ROLE{Qt::UserRole + 2};

//! Largest funding-input excess over collateral plus change accepted at
//! freeze time without a strong warning (the excess is paid to miners as fee)
constexpr CAmount MAX_EXPECTED_FUNDING_FEE{COIN / 100};

QLabel* MakeHint(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    return label;
}

//! Message box for text carrying untrusted content (participant labels from
//! an imported session file): rendered as plain text so a crafted label
//! cannot inject rich-text markup into the warning that talks about it
int ShowPlainMessage(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text,
                     QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                     QMessageBox::StandardButton default_button = QMessageBox::NoButton)
{
    QMessageBox box(parent);
    box.setIcon(icon);
    box.setWindowTitle(title);
    box.setTextFormat(Qt::PlainText);
    box.setText(text);
    box.setStandardButtons(buttons);
    if (default_button != QMessageBox::NoButton) box.setDefaultButton(default_button);
    return box.exec();
}

QString FormatAmount(const WalletModel* wallet_model, CAmount amount)
{
    const auto unit{wallet_model && wallet_model->getOptionsModel() ?
                        wallet_model->getOptionsModel()->getDisplayUnit() :
                        BitcoinUnits::Unit::DASH};
    return BitcoinUnits::formatWithUnit(unit, amount, /*plussign=*/false, BitcoinUnits::SeparatorStyle::ALWAYS);
}

//! Editable-cell representation of an amount: plain DASH decimal that
//! BitcoinUnits::parse round-trips
QString AmountCellText(CAmount amount)
{
    return BitcoinUnits::format(BitcoinUnits::Unit::DASH, amount, /*plussign=*/false,
                                BitcoinUnits::SeparatorStyle::NEVER);
}

QString OutpointKey(const QString& txid, uint32_t vout)
{
    return txid.toLower() + QLatin1Char(':') + QString::number(vout);
}

//! scriptSig presence per input of a serialized transaction, keyed by
//! lowercase "txid:vout"; empty map when the hex does not decode
std::map<QString, bool> InputSignatureMap(const QString& tx_hex)
{
    std::map<QString, bool> ret;
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, tx_hex.toStdString())) return ret;
    for (const auto& in : tx.vin) {
        ret[OutpointKey(QString::fromStdString(in.prevout.hash.ToString()), in.prevout.n)] = !in.scriptSig.empty();
    }
    return ret;
}

QString ShareDisplayLabel(const MnShareSession& session, int index)
{
    if (index >= 0 && static_cast<size_t>(index) < session.shares().size() &&
        !session.shares()[index].label.isEmpty()) {
        return session.shares()[index].label;
    }
    return QObject::tr("share %1").arg(index + 1);
}

} // anonymous namespace

SharedMnCreateDialog::SharedMnCreateDialog(interfaces::Node& node, WalletModel* wallet_model, QWidget* parent) :
    QDialog(parent),
    m_node{node},
    m_wallet_model{wallet_model},
    m_v24_active{node.isV24Active()},
    m_sender{new ProTxSender(node, this)}
{
    setMinimumSize(980, 720);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(buildHeader());

    m_pages = new QStackedWidget(this);
    m_pages->insertWidget(PageDraft, buildDraftPage());
    m_pages->insertWidget(PageSigning, buildSigningPage());
    m_pages->insertWidget(PageCombined, buildCombinedPage());
    m_pages->insertWidget(PageBroadcast, buildBroadcastPage());
    m_pages->insertWidget(PageDead, buildDeadPage());
    layout->addWidget(m_pages, /*stretch=*/1);

    GUIUtil::updateFonts();
    refreshFundingCandidates();
    refreshAll();
}

SharedMnCreateDialog::~SharedMnCreateDialog() = default;

QWidget* SharedMnCreateDialog::buildHeader()
{
    auto* header = new QWidget(this);
    auto* layout = new QVBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* top_row = new QHBoxLayout();
    m_breadcrumb = new QLabel(header);
    m_breadcrumb->setTextFormat(Qt::RichText);
    top_row->addWidget(m_breadcrumb, /*stretch=*/1);

    m_import_button = new QPushButton(tr("Import"), header);
    m_import_button->setToolTip(tr("Merge a session copy from the clipboard into this session."));
    connect(m_import_button, &QPushButton::clicked, this, &SharedMnCreateDialog::importFromClipboard);
    top_row->addWidget(m_import_button);

    m_load_button = new QPushButton(tr("Load…"), header);
    m_load_button->setToolTip(tr("Merge a session file into this session."));
    connect(m_load_button, &QPushButton::clicked, this, &SharedMnCreateDialog::loadFromFile);
    top_row->addWidget(m_load_button);

    m_export_button = new QPushButton(tr("Export"), header);
    m_export_button->setToolTip(tr("Copy this session to the clipboard to pass to another participant."));
    connect(m_export_button, &QPushButton::clicked, this, &SharedMnCreateDialog::exportToClipboard);
    top_row->addWidget(m_export_button);

    m_save_button = new QPushButton(tr("Save…"), header);
    m_save_button->setToolTip(tr("Save this session to a file to pass to another participant."));
    connect(m_save_button, &QPushButton::clicked, this, &SharedMnCreateDialog::saveToFile);
    top_row->addWidget(m_save_button);
    layout->addLayout(top_row);

    layout->addWidget(MakeHint(tr("This file is the session — any participant with the latest copy can continue it."),
                               header));

    m_phrase_label = new QLabel(header);
    m_phrase_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_phrase_label->setAlignment(Qt::AlignCenter);
    GUIUtil::setFont({m_phrase_label}, GUIUtil::FontWeight::Bold, 22);
    layout->addWidget(m_phrase_label);

    m_phrase_hint = MakeHint(tr("Check phrase — read this aloud to the other participants before signing. Everyone "
                                "must see the same phrase; a different phrase means different terms."),
                             header);
    m_phrase_hint->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_phrase_hint);

    return header;
}

QWidget* SharedMnCreateDialog::buildDraftPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* shares_box = new QGroupBox(tr("Shares (2–8 participants, amounts must sum to the collateral)"), page);
    auto* shares_layout = new QVBoxLayout(shares_box);
    m_share_table = new QTableWidget(0, 5, shares_box);
    m_share_table->setHorizontalHeaderLabels(
        {tr("Label"), tr("Amount (DASH)"), tr("Owner address"), tr("Refund address"), tr("Reward address (optional)")});
    m_share_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_share_table->horizontalHeader()->setSectionResizeMode(COL_LABEL, QHeaderView::ResizeToContents);
    m_share_table->horizontalHeader()->setSectionResizeMode(COL_AMOUNT, QHeaderView::ResizeToContents);
    m_share_table->verticalHeader()->setVisible(false);
    connect(m_share_table, &QTableWidget::cellChanged, this, &SharedMnCreateDialog::onShareCellChanged);
    shares_layout->addWidget(m_share_table);

    auto* share_buttons = new QHBoxLayout();
    m_add_share_button = new QPushButton(tr("Add Share"), shares_box);
    connect(m_add_share_button, &QPushButton::clicked, this, &SharedMnCreateDialog::addShareRow);
    share_buttons->addWidget(m_add_share_button);
    m_remove_share_button = new QPushButton(tr("Remove Share"), shares_box);
    connect(m_remove_share_button, &QPushButton::clicked, this, &SharedMnCreateDialog::removeShareRow);
    share_buttons->addWidget(m_remove_share_button);
    m_my_address_button = new QPushButton(tr("Use My Wallet Address"), shares_box);
    m_my_address_button->setToolTip(tr("Fill the selected owner, refund or reward cell with a fresh address from "
                                       "this wallet."));
    connect(m_my_address_button, &QPushButton::clicked, this, &SharedMnCreateDialog::useMyWalletAddress);
    share_buttons->addWidget(m_my_address_button);
    share_buttons->addStretch();
    m_sum_label = new QLabel(shares_box);
    share_buttons->addWidget(m_sum_label);
    shares_layout->addLayout(share_buttons);
    layout->addWidget(shares_box);

    auto* middle = new QHBoxLayout();

    auto* terms_box = new QGroupBox(tr("Terms"), page);
    auto* terms_form = new QFormLayout(terms_box);

    m_service_edit = new QLineEdit(terms_box);
    m_service_edit->setPlaceholderText(tr("IP:PORT — leave empty to set later with a service update"));
    connect(m_service_edit, &QLineEdit::textChanged, this, &SharedMnCreateDialog::onTermsChanged);
    terms_form->addRow(tr("Service address:"), m_service_edit);

    m_operator_widget = new OperatorKeyWidget(terms_box);
    connect(m_operator_widget, &OperatorKeyWidget::changed, this, &SharedMnCreateDialog::onTermsChanged);
    terms_form->addRow(tr("Operator key:"), m_operator_widget);

    m_operator_session_key_label = new QLabel(terms_box);
    m_operator_session_key_label->setTextFormat(Qt::PlainText);
    m_operator_session_key_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_operator_session_key_label->setWordWrap(true);
    m_operator_session_key_label->setVisible(false);
    terms_form->addRow(QString(), m_operator_session_key_label);

    m_operator_secret_label = new QLabel(terms_box);
    m_operator_secret_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_operator_secret_label->setWordWrap(true);
    m_operator_secret_label->setVisible(false);
    terms_form->addRow(QString(), m_operator_secret_label);

    m_secret_holder_edit = new QLineEdit(terms_box);
    m_secret_holder_edit->setPlaceholderText(tr("Participant label, e.g. Alice"));
    m_secret_holder_edit->setToolTip(tr("Recorded in the session so everyone knows who can start and revive the "
                                        "masternode."));
    connect(m_secret_holder_edit, &QLineEdit::textChanged, this, &SharedMnCreateDialog::onTermsChanged);
    terms_form->addRow(tr("Operator secret held by:"), m_secret_holder_edit);

    m_voting_edit = new QLineEdit(terms_box);
    m_voting_edit->setPlaceholderText(tr("P2PKH address of the voting key"));
    connect(m_voting_edit, &QLineEdit::textChanged, this, &SharedMnCreateDialog::onTermsChanged);
    terms_form->addRow(tr("Voting address:"), m_voting_edit);

    m_operator_reward_spin = new QDoubleSpinBox(terms_box);
    m_operator_reward_spin->setRange(0.0, 100.0);
    m_operator_reward_spin->setDecimals(2);
    m_operator_reward_spin->setSuffix(QStringLiteral(" %"));
    connect(m_operator_reward_spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &SharedMnCreateDialog::onTermsChanged);
    terms_form->addRow(tr("Operator reward:"), m_operator_reward_spin);

    m_early_period_spin = new QSpinBox(terms_box);
    m_early_period_spin->setRange(0, static_cast<int>(CProRegTx::MAX_EARLY_PERIOD_BLOCKS));
    connect(m_early_period_spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &SharedMnCreateDialog::onTermsChanged);
    m_early_period_hint = new QLabel(terms_box);
    auto* early_period_box = new QVBoxLayout();
    early_period_box->addWidget(m_early_period_spin);
    early_period_box->addWidget(m_early_period_hint);
    terms_form->addRow(tr("Early period (blocks):"), early_period_box);

    m_early_penalty_field = new BitcoinAmountField(terms_box);
    connect(m_early_penalty_field, &BitcoinAmountField::valueChanged, this, &SharedMnCreateDialog::onTermsChanged);
    m_early_penalty_hint = new QLabel(terms_box);
    m_early_penalty_hint->setWordWrap(true);
    auto* early_penalty_box = new QVBoxLayout();
    early_penalty_box->addWidget(m_early_penalty_field);
    early_penalty_box->addWidget(m_early_penalty_hint);
    terms_form->addRow(tr("Early penalty:"), early_penalty_box);

    middle->addWidget(terms_box, /*stretch=*/1);

    auto* right_column = new QVBoxLayout();

    auto* validation_box = new QGroupBox(tr("Validation"), page);
    auto* validation_layout = new QVBoxLayout(validation_box);
    m_validation_list = new QListWidget(validation_box);
    m_validation_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_validation_list->setFocusPolicy(Qt::NoFocus);
    validation_layout->addWidget(m_validation_list);
    right_column->addWidget(validation_box, /*stretch=*/1);

    auto* funding_box = new QGroupBox(tr("Funding"), page);
    auto* funding_layout = new QVBoxLayout(funding_box);
    funding_layout->addWidget(
        MakeHint(tr("Each participant contributes a single output of exactly their total share amount. The "
                    "coordinator additionally contributes one small input to pay the transaction fee and takes "
                    "the only change output."),
                 funding_box));

    auto* funding_form = new QFormLayout();
    m_my_label_edit = new QLineEdit(funding_box);
    m_my_label_edit->setPlaceholderText(tr("How the other participants know you, e.g. Alice"));
    funding_form->addRow(tr("My participant label:"), m_my_label_edit);

    m_coordinator_check = new QCheckBox(tr("I am the coordinator (add the fee input and take the change)"), funding_box);
    funding_form->addRow(QString(), m_coordinator_check);

    m_fee_amount_field = new BitcoinAmountField(funding_box);
    m_fee_amount_field->setValue(100000); // 0.001 DASH covers even a maximal 8-share registration
    funding_form->addRow(tr("Transaction fee:"), m_fee_amount_field);

    m_fee_utxo_combo = new QComboBox(funding_box);
    m_fee_utxo_combo->setToolTip(tr("A small wallet output the coordinator adds to pay the fee from."));
    funding_form->addRow(tr("Fee input:"), m_fee_utxo_combo);
    funding_layout->addLayout(funding_form);

    auto* funding_buttons = new QHBoxLayout();
    m_add_funding_button = new QPushButton(tr("Add My Funding"), funding_box);
    connect(m_add_funding_button, &QPushButton::clicked, this, &SharedMnCreateDialog::addMyFunding);
    funding_buttons->addWidget(m_add_funding_button);
    m_remove_funding_button = new QPushButton(tr("Remove Selected"), funding_box);
    connect(m_remove_funding_button, &QPushButton::clicked, this, &SharedMnCreateDialog::removeSelectedContribution);
    funding_buttons->addWidget(m_remove_funding_button);
    m_refresh_funding_button = new QPushButton(tr("Refresh"), funding_box);
    connect(m_refresh_funding_button, &QPushButton::clicked, this, &SharedMnCreateDialog::refreshFundingCandidates);
    funding_buttons->addWidget(m_refresh_funding_button);
    funding_buttons->addStretch();
    funding_layout->addLayout(funding_buttons);

    m_contrib_list = new QListWidget(funding_box);
    m_contrib_list->setToolTip(tr("Funding contributions recorded in the session."));
    funding_layout->addWidget(m_contrib_list);
    m_funding_status_label = new QLabel(funding_box);
    m_funding_status_label->setWordWrap(true);
    funding_layout->addWidget(m_funding_status_label);
    right_column->addWidget(funding_box, /*stretch=*/2);

    middle->addLayout(right_column, /*stretch=*/1);
    layout->addLayout(middle, /*stretch=*/1);

    auto* freeze_row = new QHBoxLayout();
    freeze_row->addStretch();
    m_freeze_button = new QPushButton(tr("Freeze Terms…"), page);
    connect(m_freeze_button, &QPushButton::clicked, this, &SharedMnCreateDialog::freezeSession);
    freeze_row->addWidget(m_freeze_button);
    layout->addLayout(freeze_row);

    if (m_wallet_model == nullptr) {
        for (QPushButton* button : {m_my_address_button, m_add_funding_button, m_remove_funding_button}) {
            button->setEnabled(false);
            button->setToolTip(tr("No wallet is available."));
        }
    }
    return page;
}

QWidget* SharedMnCreateDialog::buildSigningPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* sheet_box = new QGroupBox(tr("Frozen terms"), page);
    auto* sheet_layout = new QVBoxLayout(sheet_box);
    m_term_sheet = new QLabel(sheet_box);
    m_term_sheet->setTextFormat(Qt::RichText);
    m_term_sheet->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_term_sheet->setWordWrap(true);
    sheet_layout->addWidget(m_term_sheet);
    layout->addWidget(sheet_box, /*stretch=*/1);

    auto* sigs_box = new QGroupBox(tr("Consent signatures"), page);
    auto* sigs_layout = new QVBoxLayout(sigs_box);
    m_sig_table = new QTableWidget(0, 3, sigs_box);
    m_sig_table->setHorizontalHeaderLabels({tr("Share"), tr("Participant"), tr("Status")});
    m_sig_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_sig_table->horizontalHeader()->setStretchLastSection(true);
    m_sig_table->verticalHeader()->setVisible(false);
    m_sig_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sig_table->setSelectionMode(QAbstractItemView::NoSelection);
    sigs_layout->addWidget(m_sig_table);

    auto* sign_row = new QHBoxLayout();
    m_sign_button = new QPushButton(tr("Sign With This Wallet"), sigs_box);
    connect(m_sign_button, &QPushButton::clicked, this, &SharedMnCreateDialog::signConsent);
    sign_row->addWidget(m_sign_button);
    m_unfreeze_button = new QPushButton(tr("Unfreeze…"), sigs_box);
    m_unfreeze_button->setToolTip(tr("Return the session to a draft. All collected signatures are discarded."));
    connect(m_unfreeze_button, &QPushButton::clicked, this, &SharedMnCreateDialog::unfreezeSession);
    sign_row->addWidget(m_unfreeze_button);
    sign_row->addStretch();
    sigs_layout->addLayout(sign_row);
    layout->addWidget(sigs_box, /*stretch=*/1);

    return page;
}

QWidget* SharedMnCreateDialog::buildCombinedPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    m_combined_status = new QLabel(page);
    m_combined_status->setWordWrap(true);
    layout->addWidget(m_combined_status);

    auto* combine_row = new QHBoxLayout();
    m_combine_button = new QPushButton(tr("Combine Signatures"), page);
    m_combine_button->setToolTip(tr("Embed every share owner's consent signature into the transaction."));
    connect(m_combine_button, &QPushButton::clicked, this, &SharedMnCreateDialog::combineSignatures);
    combine_row->addWidget(m_combine_button);
    combine_row->addStretch();
    layout->addLayout(combine_row);

    auto* funding_box = new QGroupBox(tr("Funding signatures"), page);
    auto* funding_layout = new QVBoxLayout(funding_box);
    funding_layout->addWidget(
        MakeHint(tr("After combining, every contributor signs their own funding inputs in turn. Pass the session "
                    "to the next participant listed as pending."),
                 funding_box));
    m_funding_sig_list = new QListWidget(funding_box);
    m_funding_sig_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_funding_sig_list->setFocusPolicy(Qt::NoFocus);
    funding_layout->addWidget(m_funding_sig_list);

    auto* sign_row = new QHBoxLayout();
    m_sign_funding_button = new QPushButton(tr("Sign Funding Inputs With This Wallet"), funding_box);
    connect(m_sign_funding_button, &QPushButton::clicked, this, &SharedMnCreateDialog::signFundingInputs);
    sign_row->addWidget(m_sign_funding_button);
    sign_row->addStretch();
    funding_layout->addLayout(sign_row);
    layout->addWidget(funding_box, /*stretch=*/1);

    return page;
}

QWidget* SharedMnCreateDialog::buildBroadcastPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    m_broadcast_summary = new QLabel(page);
    m_broadcast_summary->setTextFormat(Qt::RichText);
    m_broadcast_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_broadcast_summary->setWordWrap(true);
    layout->addWidget(m_broadcast_summary);

    auto* broadcast_row = new QHBoxLayout();
    m_broadcast_button = new QPushButton(tr("Broadcast"), page);
    connect(m_broadcast_button, &QPushButton::clicked, this, &SharedMnCreateDialog::broadcastSession);
    broadcast_row->addWidget(m_broadcast_button);
    broadcast_row->addStretch();
    layout->addLayout(broadcast_row);

    m_success_label = new QLabel(page);
    m_success_label->setTextFormat(Qt::RichText);
    m_success_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_success_label->setWordWrap(true);
    m_success_label->setVisible(false);
    layout->addWidget(m_success_label);
    layout->addStretch();

    return page;
}

QWidget* SharedMnCreateDialog::buildDeadPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->addStretch();

    auto* title = new QLabel(tr("This session is dead"), page);
    title->setAlignment(Qt::AlignCenter);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 18);
    layout->addWidget(title);

    m_dead_label = new QLabel(page);
    m_dead_label->setTextFormat(Qt::PlainText); // shows imported participant labels
    m_dead_label->setWordWrap(true);
    m_dead_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_dead_label);

    auto* restart_row = new QHBoxLayout();
    restart_row->addStretch();
    auto* restart_button = new QPushButton(tr("Unlock My Coins and Restart From Draft"), page);
    connect(restart_button, &QPushButton::clicked, this, &SharedMnCreateDialog::restartFromDraft);
    restart_row->addWidget(restart_button);
    restart_row->addStretch();
    layout->addLayout(restart_row);
    layout->addStretch();

    return page;
}

bool SharedMnCreateDialog::canSign() const
{
    return m_wallet_model != nullptr && !m_wallet_model->wallet().privateKeysDisabled();
}

bool SharedMnCreateDialog::runRpc(const QString& method, const UniValue& params, bool needs_unlock, ProTxResult& result)
{
    if (m_busy) return false;

    // UnlockContext is neither copyable nor movable: keep it on this stack
    // frame and wait in a nested event loop until the worker thread reports back
    struct UnlockHolder {
        WalletModel::UnlockContext ctx;
        explicit UnlockHolder(WalletModel& wallet_model) : ctx(wallet_model.requestUnlock()) {}
    };
    std::unique_ptr<UnlockHolder> unlock;
    if (needs_unlock) {
        if (!canSign()) return false;
        unlock = std::make_unique<UnlockHolder>(*m_wallet_model);
        if (!unlock->ctx.isValid()) return false;
    }

    setBusy(true);
    QEventLoop loop;
    connect(m_sender, &ProTxSender::finished, &loop, [&](const ProTxResult& r) {
        result = r;
        loop.quit();
    });
    if (!m_sender->execute(method, params, m_wallet_model)) {
        setBusy(false);
        return false;
    }
    loop.exec();
    setBusy(false);
    return true;
}

void SharedMnCreateDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (busy) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
    } else {
        QApplication::restoreOverrideCursor();
    }
    m_pages->setEnabled(!busy);
    for (QPushButton* button : {m_import_button, m_load_button, m_export_button, m_save_button}) {
        button->setEnabled(!busy);
    }
}

void SharedMnCreateDialog::refreshAll()
{
    refreshHeader();
    if (m_dead) {
        m_dead_label->setText(m_dead_reason);
        m_pages->setCurrentIndex(PageDead);
        return;
    }
    const int total{static_cast<int>(m_session.shares().size())};
    const bool all_signed{total > 0 && m_session.signedCount() == total};
    switch (m_session.stage()) {
    case MnShareSession::Stage::Draft:
        refreshDraftPage();
        m_pages->setCurrentIndex(PageDraft);
        break;
    case MnShareSession::Stage::Frozen:
    case MnShareSession::Stage::Signing:
        if (all_signed) {
            refreshCombinedPage();
            m_pages->setCurrentIndex(PageCombined);
        } else {
            refreshSigningPage();
            m_pages->setCurrentIndex(PageSigning);
        }
        break;
    case MnShareSession::Stage::Combined:
        refreshCombinedPage();
        m_pages->setCurrentIndex(PageCombined);
        break;
    case MnShareSession::Stage::FundingSigned:
    case MnShareSession::Stage::Broadcast:
        refreshBroadcastPage();
        m_pages->setCurrentIndex(PageBroadcast);
        break;
    }
}

void SharedMnCreateDialog::refreshHeader()
{
    setWindowTitle(tr("Shared Masternode Session %1").arg(m_session.sessionId().left(8)));

    QStringList crumbs;
    for (const auto stage : {MnShareSession::Stage::Draft, MnShareSession::Stage::Frozen,
                             MnShareSession::Stage::Signing, MnShareSession::Stage::Combined,
                             MnShareSession::Stage::FundingSigned, MnShareSession::Stage::Broadcast}) {
        const QString name{MnShareSession::StageName(stage).toHtmlEscaped()};
        crumbs << (stage == m_session.stage() ? QStringLiteral("<b>%1</b>").arg(name) : name);
    }
    m_breadcrumb->setText(crumbs.join(QStringLiteral(" → ")) +
                          QStringLiteral(" &nbsp;·&nbsp; ") + tr("revision %1").arg(m_session.revision()));

    const bool show_phrase{!m_dead && m_session.stage() != MnShareSession::Stage::Draft &&
                           !m_session.consentHash().isEmpty()};
    m_phrase_label->setVisible(show_phrase);
    m_phrase_hint->setVisible(show_phrase);
    if (show_phrase) {
        m_phrase_label->setText(m_session.consentCheckPhrase());
    }
}

void SharedMnCreateDialog::refreshDraftPage()
{
    refreshSharesTable();
    pushTermsToWidgets();
    refreshContributions();
    refreshValidation();
}

void SharedMnCreateDialog::refreshSharesTable()
{
    m_updating = true;
    const auto& shares{m_session.shares()};
    m_share_table->setRowCount(static_cast<int>(shares.size()));
    for (int row = 0; row < static_cast<int>(shares.size()); ++row) {
        const auto& share{shares[row]};
        const auto set_cell = [this, row](int column, const QString& text) {
            auto* item = m_share_table->item(row, column);
            if (item == nullptr) {
                item = new QTableWidgetItem();
                m_share_table->setItem(row, column, item);
            }
            item->setText(text);
        };
        set_cell(COL_LABEL, share.label);
        set_cell(COL_AMOUNT, share.amount > 0 ? AmountCellText(share.amount) : QString());
        set_cell(COL_OWNER, share.ownerAddress);
        set_cell(COL_REFUND, share.refundAddress);
        set_cell(COL_REWARD, share.rewardAddress);
    }
    m_updating = false;
    m_add_share_button->setEnabled(shares.size() < CProRegTx::MAX_SHARES);
}

void SharedMnCreateDialog::refreshValidation()
{
    const QStringList errors{m_session.validateShares()};
    m_validation_list->clear();
    if (errors.isEmpty()) {
        auto* item = new QListWidgetItem(tr("The share table and terms are valid."), m_validation_list);
        item->setForeground(Qt::darkGreen);
    } else {
        for (const QString& error : errors) {
            new QListWidgetItem(error, m_validation_list);
        }
    }

    const CAmount required{GetMnType(MnType::Regular).collat_amount};
    CAmount total{0};
    CAmount min_share{0};
    for (const auto& share : m_session.shares()) {
        total += share.amount;
        if (share.amount > 0 && (min_share == 0 || share.amount < min_share)) min_share = share.amount;
    }
    m_sum_label->setText(tr("%1 of %2").arg(FormatAmount(m_wallet_model, total), FormatAmount(m_wallet_model, required)));

    m_early_period_hint->setText(MnShareSession::HumanEarlyPeriod(static_cast<uint32_t>(m_early_period_spin->value())));
    if (min_share > 0) {
        m_early_penalty_field->SetMaxValue(min_share - 1);
        m_early_penalty_hint->setText(tr("Paid by a participant who dissolves unilaterally during the early period; "
                                         "must stay below the smallest share (%1).")
                                          .arg(FormatAmount(m_wallet_model, min_share)));
    } else {
        m_early_penalty_hint->setText(tr("Paid by a participant who dissolves unilaterally during the early period; "
                                         "must stay below the smallest share."));
    }

    const bool can_freeze{m_v24_active && errors.isEmpty() && !m_session.contributions().empty() && !m_busy};
    m_freeze_button->setEnabled(can_freeze);
    if (!m_v24_active) {
        m_freeze_button->setToolTip(tr("Shared masternodes require the v24 hard fork to be active."));
    } else if (errors.isEmpty() && m_session.contributions().empty()) {
        m_freeze_button->setToolTip(tr("Add at least one funding contribution first."));
    } else {
        m_freeze_button->setToolTip(tr("Prepare the registration transaction and fix the terms. After freezing, "
                                       "every share owner signs the consent check phrase."));
    }
}

void SharedMnCreateDialog::refreshContributions()
{
    m_contrib_list->clear();
    CAmount resolved_total{0};
    int unknown_inputs{0};
    for (const auto& contribution : m_session.contributions()) {
        CAmount resolved{0};
        int unknown{0};
        for (const auto& input : contribution.inputs) {
            if (const auto value{resolveInputValue(input)}) {
                resolved += *value;
            } else {
                ++unknown;
            }
        }
        resolved_total += resolved;
        unknown_inputs += unknown;
        QString text{tr("%1 — %n input(s)", nullptr, static_cast<int>(contribution.inputs.size()))
                         .arg(contribution.label)};
        if (resolved > 0) text += tr(", %1 resolved").arg(FormatAmount(m_wallet_model, resolved));
        if (unknown > 0) text += tr(", %n unconfirmed or unknown input(s)", nullptr, unknown);
        if (contribution.hasChange) {
            text += tr(", change %1 to %2").arg(FormatAmount(m_wallet_model, contribution.changeAmount),
                                                contribution.changeAddress);
        }
        new QListWidgetItem(text, m_contrib_list);
    }

    const CAmount required{GetMnType(MnType::Regular).collat_amount};
    QString status{tr("Funding inputs resolve to %1 of the %2 collateral plus fee.")
                       .arg(FormatAmount(m_wallet_model, resolved_total), FormatAmount(m_wallet_model, required))};
    if (unknown_inputs > 0) {
        status += QLatin1Char(' ') +
                  tr("%n input(s) could not be resolved yet (unconfirmed outputs resolve after their transaction "
                     "confirms).",
                     nullptr, unknown_inputs);
    }
    m_funding_status_label->setText(status);
}

void SharedMnCreateDialog::refreshSigningPage()
{
    // Term sheet
    QStringList rows;
    rows << QStringLiteral("<b>%1</b>").arg(tr("Shares"));
    const auto& shares{m_session.shares()};
    for (size_t i = 0; i < shares.size(); ++i) {
        const auto& share{shares[i]};
        QString line{tr("%1. %2 — %3, owner %4, refund %5")
                         .arg(i + 1)
                         .arg(ShareDisplayLabel(m_session, static_cast<int>(i)).toHtmlEscaped(),
                              FormatAmount(m_wallet_model, share.amount), share.ownerAddress.toHtmlEscaped(),
                              share.refundAddress.toHtmlEscaped())};
        line += share.rewardAddress.isEmpty() ? QLatin1Char(' ') + tr("(rewards to the refund address)") :
                                                QLatin1Char(' ') + tr("(rewards to %1)").arg(share.rewardAddress.toHtmlEscaped());
        rows << line;
    }
    const auto& terms{m_session.terms()};
    rows << QStringLiteral("<br><b>%1</b>").arg(tr("Terms"));
    rows << tr("Service: %1").arg(terms.coreP2PAddrs.isEmpty() ?
                                      tr("(none — set later with a service update)") :
                                      terms.coreP2PAddrs.toHtmlEscaped());
    rows << tr("Operator key: %1").arg(terms.operatorPubKey.toHtmlEscaped());
    rows << tr("Operator secret held by: %1")
                .arg(m_session.operatorSecretHolder().isEmpty() ? tr("(not recorded)") :
                                                                  m_session.operatorSecretHolder().toHtmlEscaped());
    rows << tr("Voting: %1").arg(terms.votingAddress.toHtmlEscaped());
    rows << tr("Operator reward: %1%").arg(QString::number(terms.operatorReward / 100.0, 'f', 2));
    rows << tr("Early period: %1 blocks (%2)")
                .arg(terms.earlyPeriodBlocks)
                .arg(MnShareSession::HumanEarlyPeriod(terms.earlyPeriodBlocks));
    rows << tr("Early penalty: %1").arg(FormatAmount(m_wallet_model, terms.earlyPenalty));
    if (!m_session.prepareWallet().isEmpty()) {
        rows << tr("Prepared by wallet: %1").arg(m_session.prepareWallet().toHtmlEscaped());
    }
    m_term_sheet->setText(rows.join(QStringLiteral("<br>")));

    // Signature checklist. Stored signatures were verified when added, but an
    // envelope loaded from disk is re-verified here so a stale or tampered
    // signature is surfaced instead of counted (A7).
    const auto my_indexes{myShareIndexes()};
    m_sig_table->setRowCount(static_cast<int>(shares.size()));
    for (int i = 0; i < static_cast<int>(shares.size()); ++i) {
        const QString label{ShareDisplayLabel(m_session, i)};
        QString status;
        const QString sig{m_session.signatureFor(i)};
        if (sig.isEmpty()) {
            status = tr("Pending — ask %1 to sign").arg(label);
        } else if (QString sig_error; m_session.verifySignature(i, sig, sig_error)) {
            status = tr("Signed");
        } else {
            status = sig_error;
        }
        const auto set_cell = [this, i](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_sig_table->setItem(i, column, item);
        };
        set_cell(0, QString::number(i + 1));
        set_cell(1, label + (std::count(my_indexes.begin(), my_indexes.end(), i) > 0 ?
                                 QLatin1Char(' ') + tr("(you)") :
                                 QString()));
        set_cell(2, status);
    }

    const bool signing_stage{m_session.stage() == MnShareSession::Stage::Frozen ||
                             m_session.stage() == MnShareSession::Stage::Signing};
    m_sign_button->setEnabled(signing_stage && canSign() && !my_indexes.empty() && !m_busy);
    if (!canSign()) {
        m_sign_button->setToolTip(m_wallet_model == nullptr ? tr("No wallet is available.") :
                                                              tr("This wallet is watch-only and cannot sign."));
    } else if (my_indexes.empty()) {
        m_sign_button->setToolTip(tr("This wallet holds none of the share owner keys."));
    } else {
        m_sign_button->setToolTip(tr("Sign the consent check phrase with every share owner key in this wallet."));
    }
    m_unfreeze_button->setEnabled(signing_stage && !m_busy);
}

void SharedMnCreateDialog::refreshCombinedPage()
{
    const int total{static_cast<int>(m_session.shares().size())};
    const bool combined{m_session.stage() == MnShareSession::Stage::Combined};
    const bool ready_to_combine{!combined && total > 0 && m_session.signedCount() == total};

    m_combine_button->setVisible(!combined);
    m_combine_button->setEnabled(ready_to_combine && m_v24_active && !m_busy);
    if (!m_v24_active) m_combine_button->setToolTip(tr("Shared masternodes require the v24 hard fork to be active."));
    m_combined_status->setText(combined ?
                                   tr("All %n consent signature(s) are embedded in the transaction. The funding "
                                      "inputs now have to be signed by their contributors.",
                                      nullptr, total) :
                                   tr("Every share owner has signed. Combine the signatures into the transaction to "
                                      "continue."));

    m_funding_sig_list->clear();
    const auto signed_map{InputSignatureMap(m_session.protxHex())};
    int outstanding{0};
    for (const auto& contribution : m_session.contributions()) {
        int done{0};
        for (const auto& input : contribution.inputs) {
            const auto it{signed_map.find(OutpointKey(input.txid, input.vout))};
            if (it != signed_map.end() && it->second) ++done;
        }
        const int inputs{static_cast<int>(contribution.inputs.size())};
        if (done < inputs) ++outstanding;
        new QListWidgetItem(done == inputs ?
                                tr("%1 — signed").arg(contribution.label) :
                                tr("%1 — pending (%2 of %3 inputs signed)").arg(contribution.label).arg(done).arg(inputs),
                            m_funding_sig_list);
    }
    if (outstanding == 0 && combined && !m_session.contributions().empty()) {
        new QListWidgetItem(tr("All funding inputs are signed."), m_funding_sig_list);
    }

    m_sign_funding_button->setEnabled(combined && canSign() && !m_busy);
    m_sign_funding_button->setToolTip(!combined ? tr("Combine the consent signatures first.") :
                                      canSign()  ? tr("Sign every funding input this wallet holds the keys for.") :
                                                   tr("No wallet able to sign is available."));
}

void SharedMnCreateDialog::refreshBroadcastPage()
{
    const bool broadcast{m_session.stage() == MnShareSession::Stage::Broadcast};

    CMutableTransaction tx;
    QString txid;
    if (DecodeHexTx(tx, m_session.protxHex().toStdString())) {
        txid = QString::fromStdString(tx.GetHash().ToString());
    }

    QStringList rows;
    rows << tr("The shared masternode registration is fully signed.");
    rows << tr("Check phrase: <b>%1</b>").arg(m_session.consentCheckPhrase());
    rows << tr("Transaction id: %1").arg(txid.toHtmlEscaped());
    rows << tr("Size: %n byte(s), collateral output index %1", nullptr,
               static_cast<int>(m_session.protxHex().size() / 2))
                .arg(m_session.collateralIndex());
    m_broadcast_summary->setText(rows.join(QStringLiteral("<br>")));

    m_broadcast_button->setVisible(!broadcast);
    m_broadcast_button->setEnabled(!broadcast && m_v24_active && !m_busy);
    if (!m_v24_active) m_broadcast_button->setToolTip(tr("Shared masternodes require the v24 hard fork to be active."));

    m_success_label->setVisible(broadcast);
    if (broadcast) {
        m_success_label->setText(
            QStringLiteral("<b>%1</b><br><br>%2<br><b>%3</b><br><br>%4")
                .arg(tr("The registration was sent to the network."), tr("Provider transaction hash (proTxHash):"),
                     txid.toHtmlEscaped(),
                     tr("IMPORTANT: once the registration has one confirmation, every participant should generate "
                        "standby dissolutions from the Masternodes list (select the new masternode, then Dissolve → "
                        "Standby) and store both variants together with their refund-key backup, separately from the "
                        "share owner key. A standby dissolution never expires and recovers your principal even if "
                        "your owner key is lost.")));
    }
}

void SharedMnCreateDialog::syncShareFromTable(int row)
{
    auto& shares{m_session.shares()};
    if (row < 0 || row >= static_cast<int>(shares.size())) return;
    const auto cell = [this, row](int column) {
        const auto* item = m_share_table->item(row, column);
        return item != nullptr ? item->text().trimmed() : QString();
    };
    shares[row].label = cell(COL_LABEL);
    CAmount amount{0};
    if (!BitcoinUnits::parse(BitcoinUnits::Unit::DASH, cell(COL_AMOUNT), &amount)) amount = 0;
    shares[row].amount = amount;
    shares[row].ownerAddress = cell(COL_OWNER);
    shares[row].refundAddress = cell(COL_REFUND);
    shares[row].rewardAddress = cell(COL_REWARD);
}

void SharedMnCreateDialog::syncTermsToSession()
{
    auto& terms{m_session.terms()};
    terms.coreP2PAddrs = m_service_edit->text().trimmed();
    // An imported session's operator key is the group's agreed key: never
    // replace it with this dialog's widget (the widget cannot be pre-filled
    // from a session file, so its "generated" key is always a local one)
    if (!m_operator_key_from_import && m_operator_widget->isValid()) {
        terms.operatorPubKey = m_operator_widget->publicKeyHex();
    }
    terms.votingAddress = m_voting_edit->text().trimmed();
    terms.operatorReward = qRound(m_operator_reward_spin->value() * 100.0);
    terms.earlyPeriodBlocks = static_cast<uint32_t>(m_early_period_spin->value());
    terms.earlyPenalty = m_early_penalty_field->value();
    m_session.setOperatorSecretHolder(m_secret_holder_edit->text().trimmed());

    const bool widget_key_in_use{!m_operator_key_from_import ||
                                 m_operator_widget->publicKeyHex() == terms.operatorPubKey};
    if (widget_key_in_use && m_operator_widget->hasGeneratedSecret()) {
        m_operator_secret_label->setVisible(true);
        m_operator_secret_label->setText(
            tr("Operator secret key (write it down now, it is not stored anywhere): %1")
                .arg(m_operator_widget->secretHex()));
    } else {
        m_operator_secret_label->setVisible(false);
    }
}

void SharedMnCreateDialog::pushTermsToWidgets()
{
    m_updating = true;
    const auto& terms{m_session.terms()};
    m_service_edit->setText(terms.coreP2PAddrs);
    m_voting_edit->setText(terms.votingAddress);
    m_operator_reward_spin->setValue(terms.operatorReward / 100.0);
    m_early_period_spin->setValue(static_cast<int>(terms.earlyPeriodBlocks));
    m_early_penalty_field->setValue(terms.earlyPenalty);
    m_secret_holder_edit->setText(m_session.operatorSecretHolder());

    const bool imported_key{m_operator_key_from_import && !terms.operatorPubKey.isEmpty()};
    m_operator_widget->setEnabled(!imported_key);
    m_operator_session_key_label->setVisible(imported_key);
    if (imported_key) {
        m_operator_widget->setToolTip(tr("The imported session already carries the group's agreed operator key; "
                                         "it is kept as-is."));
        m_operator_session_key_label->setText(tr("Operator key from the imported session (used as-is): %1")
                                                  .arg(terms.operatorPubKey));
        if (m_operator_widget->publicKeyHex() != terms.operatorPubKey) {
            m_operator_secret_label->setVisible(false); // the widget's local secret is not the session key
        }
    } else {
        m_operator_widget->setToolTip(QString());
    }
    m_updating = false;
}

void SharedMnCreateDialog::importFromClipboard()
{
    adoptImportedText(QApplication::clipboard()->text(), tr("clipboard"));
}

void SharedMnCreateDialog::loadFromFile()
{
    const QString filename{GUIUtil::getOpenFileName(this, tr("Load Shared Masternode Session"), QString(),
                                                    tr("Session files (*.json)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, windowTitle(), tr("Could not open %1 for reading.").arg(filename));
        return;
    }
    adoptImportedText(QString::fromUtf8(file.readAll()), filename);
}

void SharedMnCreateDialog::exportToClipboard()
{
    GUIUtil::setClipboard(m_session.toJsonString());
    QMessageBox::information(this, windowTitle(),
                             tr("The session was copied to the clipboard. Also save it to a file so it cannot be "
                                "lost to a clipboard overwrite."));
}

void SharedMnCreateDialog::saveToFile()
{
    writeSessionToFile();
}

bool SharedMnCreateDialog::writeSessionToFile()
{
    const QString suggested{QStringLiteral("shared-mn-session-%1-r%2.json")
                                .arg(m_session.sessionId().left(8))
                                .arg(m_session.revision())};
    const QString filename{GUIUtil::getSaveFileName(this, tr("Save Shared Masternode Session"), suggested,
                                                    tr("Session files (*.json)"), nullptr)};
    if (filename.isEmpty()) return false;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::critical(this, windowTitle(), tr("Could not open %1 for writing.").arg(filename));
        return false;
    }
    file.write(m_session.toJsonString().toUtf8());
    file.close();
    m_dirty = false;
    return true;
}

void SharedMnCreateDialog::adoptImportedText(const QString& text, const QString& source)
{
    if (text.trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("%1 does not contain a session file.").arg(source));
        return;
    }
    MnShareSession imported;
    QString error;
    if (!imported.fromJson(text.toStdString(), error)) {
        ShowPlainMessage(this, QMessageBox::Critical, windowTitle(), error);
        return;
    }

    // A pristine dialog adopts the imported session outright
    const bool pristine{m_session.stage() == MnShareSession::Stage::Draft && m_session.shares().empty() &&
                        m_session.contributions().empty() && !m_dirty};
    if (pristine) {
        m_session = imported;
        afterImport();
        return;
    }

    QString merge_error;
    switch (m_session.mergeEnvelope(imported, merge_error)) {
    case MnShareSession::MergeResult::Merged:
        if (!merge_error.isEmpty()) {
            ShowPlainMessage(this, QMessageBox::Warning, tr("Merged with warnings"), merge_error);
        }
        markDirty();
        afterImport();
        break;
    case MnShareSession::MergeResult::OtherOlder:
        ShowPlainMessage(this, QMessageBox::Information, tr("Imported copy is older"), merge_error);
        break;
    case MnShareSession::MergeResult::OtherNewer: {
        const int choice{ShowPlainMessage(
            this, QMessageBox::Question, tr("Imported copy is newer"),
            merge_error + QStringLiteral("\n\n") + tr("Adopt the newer copy? Your local copy is discarded."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)};
        if (choice == QMessageBox::Yes) {
            unlockAllContributedCoins(); // the replaced session's coin locks would otherwise leak
            m_session = imported;
            markDirty();
            afterImport();
        }
        break;
    }
    case MnShareSession::MergeResult::Conflict: {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Session conflict"));
        box.setTextFormat(Qt::PlainText);
        box.setText(merge_error);
        box.setInformativeText(tr("Replace this session with the imported copy, or keep working on this one?"));
        auto* replace{box.addButton(tr("Replace With Imported"), QMessageBox::DestructiveRole)};
        box.addButton(tr("Keep Mine"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == replace) {
            unlockAllContributedCoins(); // the replaced session's coin locks would otherwise leak
            m_session = imported;
            markDirty();
            afterImport();
        }
        break;
    }
    }
}

void SharedMnCreateDialog::afterImport()
{
    // A7: an adopted envelope's signatures were stored as-is by fromJson;
    // re-verify them all and drop any that do not verify against the frozen
    // terms so they are flagged instead of counted (mergeEnvelope verifies on
    // its own, so this is a no-op for merges)
    QStringList bad;
    for (const auto& check : m_session.verifyAllSignatures()) {
        if (!check.valid) bad << check.error;
    }
    if (!bad.isEmpty()) {
        UniValue json{m_session.toJson()};
        UniValue sigs(UniValue::VARR);
        for (const auto& sig : m_session.signatures()) {
            QString sig_error;
            if (!m_session.verifySignature(sig.shareIndex, sig.signatureB64, sig_error)) continue;
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("shareIndex", sig.shareIndex);
            entry.pushKV("signature", sig.signatureB64.toStdString());
            sigs.push_back(entry);
        }
        json.pushKV("sigs", sigs);
        QString error;
        m_session.fromJson(json, error); // leaves the session untouched on failure
        ShowPlainMessage(this, QMessageBox::Warning, tr("Signatures not counted"), bad.join(QLatin1Char('\n')));
    }

    // An imported envelope's operator key is the group's agreed key; from now
    // on this dialog displays it read-only instead of the generate widget
    m_operator_key_from_import = !m_session.terms().operatorPubKey.isEmpty();

    // A replacement session gets a fresh liveness verdict
    m_dead = false;
    m_dead_reason.clear();
    checkSessionLiveness();
    // Keep the A8 invariant on this machine too: any of this wallet's coins
    // the adopted session uses as funding inputs must be locked
    if (!m_dead) lockKnownContributedCoins();
    refreshAll();
}

void SharedMnCreateDialog::checkSessionLiveness()
{
    QStringList spent;
    for (const auto& contribution : m_session.contributions()) {
        for (const auto& input : contribution.inputs) {
            const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
            Coin coin;
            if (m_node.getUnspentOutput(outpoint, coin)) continue;
            // Not in the confirmed UTXO set — which alone proves nothing:
            // getUnspentOutput consults neither the mempool nor other wallets,
            // so another participant's unconfirmed chip looks exactly the
            // same. Only an input this wallet tracks and reports spent is
            // proof the funding transaction can never confirm.
            if (m_wallet_model == nullptr) continue;
            const auto known{m_wallet_model->wallet().getCoins({outpoint})};
            if (known.empty() || known.front().depth_in_main_chain < 0) continue; // unknown here: pending
            if (!known.front().is_spent) continue; // own unconfirmed output: pending
            spent << tr("%1:%2 (contributed by %3)").arg(input.txid).arg(input.vout).arg(contribution.label);
        }
    }
    if (!spent.isEmpty()) {
        markDead(tr("A funding input contributed from this wallet was spent, so the recorded funding "
                    "transaction can never confirm. The session cannot be continued — restart from a new draft.") +
                 QStringLiteral("\n\n") + spent.join(QLatin1Char('\n')));
    }
}

void SharedMnCreateDialog::markDead(const QString& reason)
{
    m_dead = true;
    m_dead_reason = reason;
    refreshAll();
}

void SharedMnCreateDialog::markDirty()
{
    m_dirty = true;
}

void SharedMnCreateDialog::offerExport(const QString& why)
{
    if (m_dead) return;
    const auto choice{QMessageBox::question(
        this, windowTitle(),
        why + QStringLiteral("\n\n") +
            tr("Copy the updated session to the clipboard and save it to a file now? The other participants need "
               "the latest copy to continue."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)};
    if (choice != QMessageBox::Yes) return;
    GUIUtil::setClipboard(m_session.toJsonString());
    writeSessionToFile();
}

void SharedMnCreateDialog::addShareRow()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    auto& shares{m_session.shares()};
    if (shares.size() >= CProRegTx::MAX_SHARES) return;
    MnShareSession::Share share;
    share.label = tr("Participant %1").arg(shares.size() + 1);
    shares.push_back(share);
    markDirty();
    refreshSharesTable();
    refreshValidation();
}

void SharedMnCreateDialog::removeShareRow()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    const int row{m_share_table->currentRow()};
    auto& shares{m_session.shares()};
    if (row < 0 || row >= static_cast<int>(shares.size())) return;
    shares.erase(shares.begin() + row);
    markDirty();
    refreshSharesTable();
    refreshValidation();
}

void SharedMnCreateDialog::useMyWalletAddress()
{
    if (m_wallet_model == nullptr) return;
    const int row{m_share_table->currentRow()};
    if (row < 0 || row >= m_share_table->rowCount()) {
        QMessageBox::information(this, windowTitle(), tr("Select a share row first."));
        return;
    }
    int column{m_share_table->currentColumn()};
    if (column != COL_OWNER && column != COL_REFUND && column != COL_REWARD) column = COL_OWNER;

    QString error;
    const QString address{freshAddress(error)};
    if (address.isEmpty()) {
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    auto* item = m_share_table->item(row, column);
    if (item == nullptr) {
        item = new QTableWidgetItem();
        m_share_table->setItem(row, column, item);
    }
    item->setText(address); // triggers onShareCellChanged -> session sync
}

void SharedMnCreateDialog::onShareCellChanged(int row, int column)
{
    Q_UNUSED(column);
    if (m_updating) return;
    syncShareFromTable(row);
    markDirty();
    refreshValidation();
}

void SharedMnCreateDialog::onTermsChanged()
{
    if (m_updating) return;
    syncTermsToSession();
    markDirty();
    refreshValidation();
}

std::vector<int> SharedMnCreateDialog::myShareIndexes() const
{
    std::vector<int> ret;
    if (m_wallet_model == nullptr) return ret;
    const auto& shares{m_session.shares()};
    for (size_t i = 0; i < shares.size(); ++i) {
        const CTxDestination dest{DecodeDestination(shares[i].ownerAddress.toStdString())};
        if (IsValidDestination(dest) && m_wallet_model->wallet().isSpendable(dest)) {
            ret.push_back(static_cast<int>(i));
        }
    }
    return ret;
}

CAmount SharedMnCreateDialog::myShareTotal() const
{
    CAmount total{0};
    for (const int index : myShareIndexes()) {
        total += m_session.shares()[index].amount;
    }
    return total;
}

QString SharedMnCreateDialog::freshAddress(QString& error) const
{
    error.clear();
    if (m_wallet_model == nullptr) {
        error = tr("No wallet is available.");
        return {};
    }
    auto dest{m_wallet_model->wallet().getNewDestination(/*label=*/"")};
    if (!dest) {
        error = tr("Could not generate a new address: %1")
                    .arg(QString::fromStdString(util::ErrorString(dest).translated));
        return {};
    }
    return QString::fromStdString(EncodeDestination(*dest));
}

std::optional<MnShareSession::Input> SharedMnCreateDialog::findFundingChip(CAmount amount) const
{
    if (m_wallet_model == nullptr || amount <= 0) return std::nullopt;
    for (const auto& [dest, coins] : m_wallet_model->wallet().listCoins()) {
        for (const auto& [outpoint, txout] : coins) {
            if (txout.txout.nValue != amount || txout.is_spent || txout.depth_in_main_chain < 0) continue;
            if (m_wallet_model->wallet().isLockedCoin(outpoint)) continue;
            MnShareSession::Input input;
            input.txid = QString::fromStdString(outpoint.hash.ToString());
            input.vout = outpoint.n;
            return input;
        }
    }
    return std::nullopt;
}

std::optional<MnShareSession::Input> SharedMnCreateDialog::prepareFundingChip(CAmount amount, QString& error)
{
    QString address{freshAddress(error)};
    if (address.isEmpty()) return std::nullopt;

    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());
    if (!ctx.isValid()) {
        error = tr("Wallet unlock was cancelled.");
        return std::nullopt;
    }

    SendCoinsRecipient recipient(address, tr("Shared masternode funding"), amount, /*_message=*/QString());
    WalletModelTransaction transaction({recipient});
    wallet::CCoinControl coin_control;
    const auto prepared{m_wallet_model->prepareTransaction(transaction, coin_control)};
    if (prepared.status != WalletModel::OK) {
        error = tr("Could not prepare the funding output (is the balance sufficient to cover %1 plus fee?)")
                    .arg(FormatAmount(m_wallet_model, amount));
        return std::nullopt;
    }
    m_wallet_model->sendCoins(transaction, /*fIsCoinJoin=*/false);

    const CTransactionRef& wtx{transaction.getWtx()};
    const CScript script{GetScriptForDestination(DecodeDestination(address.toStdString()))};
    for (size_t n = 0; n < wtx->vout.size(); ++n) {
        if (wtx->vout[n].scriptPubKey == script && wtx->vout[n].nValue == amount) {
            MnShareSession::Input input;
            input.txid = QString::fromStdString(wtx->GetHash().ToString());
            input.vout = static_cast<uint32_t>(n);
            return input;
        }
    }
    error = tr("The funding transaction was sent but its output could not be located.");
    return std::nullopt;
}

void SharedMnCreateDialog::refreshFundingCandidates()
{
    m_fee_utxo_combo->clear();
    if (m_wallet_model == nullptr) return;

    std::vector<std::tuple<CAmount, QString, uint32_t>> candidates;
    for (const auto& [dest, coins] : m_wallet_model->wallet().listCoins()) {
        for (const auto& [outpoint, txout] : coins) {
            if (txout.is_spent || txout.depth_in_main_chain < 1) continue;
            if (m_wallet_model->wallet().isLockedCoin(outpoint)) continue;
            candidates.emplace_back(txout.txout.nValue, QString::fromStdString(outpoint.hash.ToString()), outpoint.n);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& [value, txid, vout] : candidates) {
        m_fee_utxo_combo->addItem(QStringLiteral("%1 — %2:%3").arg(FormatAmount(m_wallet_model, value),
                                                                   txid.left(16) + QStringLiteral("…"),
                                                                   QString::number(vout)));
        const int index{m_fee_utxo_combo->count() - 1};
        m_fee_utxo_combo->setItemData(index, txid, FEE_UTXO_TXID_ROLE);
        m_fee_utxo_combo->setItemData(index, vout, FEE_UTXO_VOUT_ROLE);
        m_fee_utxo_combo->setItemData(index, static_cast<qlonglong>(value), FEE_UTXO_VALUE_ROLE);
    }
    refreshContributions();
}

void SharedMnCreateDialog::addMyFunding()
{
    if (m_wallet_model == nullptr || m_session.stage() != MnShareSession::Stage::Draft || m_busy) return;

    const QString label{m_my_label_edit->text().trimmed()};
    if (label.isEmpty()) {
        QMessageBox::information(this, windowTitle(), tr("Enter your participant label first."));
        return;
    }
    const CAmount amount{myShareTotal()};
    if (amount <= 0) {
        QMessageBox::information(this, windowTitle(),
                                 tr("This wallet owns none of the shares. Set the owner address of your share(s) "
                                    "to an address of this wallet first — your contribution must equal your total "
                                    "share amount."));
        return;
    }

    MnShareSession::Contribution contribution;
    contribution.label = label;

    auto chip{findFundingChip(amount)};
    if (!chip) {
        const auto choice{QMessageBox::question(
            this, windowTitle(),
            tr("No single unlocked output of exactly %1 exists in this wallet. Prepare one now with an ordinary "
               "send to a fresh address of your own? (This pays one extra transaction fee.)")
                .arg(FormatAmount(m_wallet_model, amount)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)};
        if (choice != QMessageBox::Yes) return;
        QString error;
        chip = prepareFundingChip(amount, error);
        if (!chip) {
            QMessageBox::critical(this, windowTitle(), error);
            return;
        }
    }
    contribution.inputs.push_back(*chip);

    if (m_coordinator_check->isChecked()) {
        const int fee_index{m_fee_utxo_combo->currentIndex()};
        const CAmount fee{m_fee_amount_field->value()};
        if (fee_index < 0 || fee <= 0) {
            QMessageBox::information(this, windowTitle(), tr("Select a fee input and a positive fee amount."));
            return;
        }
        MnShareSession::Input fee_input;
        fee_input.txid = m_fee_utxo_combo->itemData(fee_index, FEE_UTXO_TXID_ROLE).toString();
        fee_input.vout = m_fee_utxo_combo->itemData(fee_index, FEE_UTXO_VOUT_ROLE).toUInt();
        const CAmount fee_value{m_fee_utxo_combo->itemData(fee_index, FEE_UTXO_VALUE_ROLE).toLongLong()};
        if (fee_input.txid == chip->txid && fee_input.vout == chip->vout) {
            QMessageBox::information(this, windowTitle(),
                                     tr("The fee input is the funding output itself. Pick a different, smaller "
                                        "output."));
            return;
        }
        if (fee_value < fee) {
            QMessageBox::information(this, windowTitle(), tr("The selected fee input is smaller than the fee."));
            return;
        }
        contribution.inputs.push_back(fee_input);
        if (fee_value > fee) {
            QString error;
            const QString change_address{freshAddress(error)};
            if (change_address.isEmpty()) {
                QMessageBox::critical(this, windowTitle(), error);
                return;
            }
            contribution.hasChange = true;
            contribution.changeAddress = change_address;
            contribution.changeAmount = fee_value - fee;
        }
    }

    QString error;
    if (!m_session.addContribution(contribution, error)) {
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    setContributionLocked(contribution, /*lock=*/true); // A8: contributed coins stay locked
    markDirty();
    refreshContributions();
    refreshValidation();
}

void SharedMnCreateDialog::removeSelectedContribution()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    const int row{m_contrib_list->currentRow()};
    const auto& contributions{m_session.contributions()};
    if (row < 0 || row >= static_cast<int>(contributions.size())) {
        QMessageBox::information(this, windowTitle(), tr("Select a contribution first."));
        return;
    }
    const MnShareSession::Contribution removed{contributions[row]};
    QString error;
    if (!m_session.removeContribution(removed.label, error)) {
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    setContributionLocked(removed, /*lock=*/false);
    markDirty();
    refreshContributions();
    refreshValidation();
}

void SharedMnCreateDialog::setContributionLocked(const MnShareSession::Contribution& contribution, bool lock)
{
    if (m_wallet_model == nullptr) return;
    for (const auto& input : contribution.inputs) {
        const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
        if (lock) {
            m_wallet_model->wallet().lockCoin(outpoint, /*write_to_db=*/true);
        } else {
            m_wallet_model->wallet().unlockCoin(outpoint);
        }
    }
}

void SharedMnCreateDialog::unlockAllContributedCoins()
{
    for (const auto& contribution : m_session.contributions()) {
        setContributionLocked(contribution, /*lock=*/false);
    }
}

std::optional<CAmount> SharedMnCreateDialog::resolveInputValue(const MnShareSession::Input& input) const
{
    const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
    Coin coin;
    if (m_node.getUnspentOutput(outpoint, coin)) return coin.out.nValue;
    // Not confirmed: this wallet can still value its own pending outputs
    if (m_wallet_model != nullptr) {
        const auto known{m_wallet_model->wallet().getCoins({outpoint})};
        if (!known.empty() && known.front().depth_in_main_chain >= 0 && !known.front().is_spent) {
            return known.front().txout.nValue;
        }
    }
    return std::nullopt;
}

void SharedMnCreateDialog::lockKnownContributedCoins()
{
    if (m_wallet_model == nullptr) return;
    for (const auto& contribution : m_session.contributions()) {
        for (const auto& input : contribution.inputs) {
            const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
            const auto known{m_wallet_model->wallet().getCoins({outpoint})};
            if (known.empty() || known.front().depth_in_main_chain < 0 || known.front().is_spent) continue;
            m_wallet_model->wallet().lockCoin(outpoint, /*write_to_db=*/true);
        }
    }
}

void SharedMnCreateDialog::freezeSession()
{
    if (m_session.stage() != MnShareSession::Stage::Draft || m_busy) return;
    syncTermsToSession();
    if (!m_session.validateShares().isEmpty() || m_session.fundingTxHex().isEmpty()) {
        refreshValidation();
        return;
    }

    const auto& terms{m_session.terms()};
    QString parse_error;
    const QStringList services{parseServiceList(terms.coreP2PAddrs, parse_error)};
    if (!parse_error.isEmpty()) {
        QMessageBox::critical(this, windowTitle(), parse_error);
        return;
    }

    // Funding sufficiency gate. register_shared_prepare cannot value-check the
    // inputs (they may be unconfirmed), so a mismatch would otherwise surface
    // only at the final broadcast — or silently overpay the excess to miners.
    // Inputs this node cannot resolve (other participants' unconfirmed chips)
    // are surfaced honestly instead of blocking the flow.
    const CAmount collateral{GetMnType(MnType::Regular).collat_amount};
    CAmount change_total{0};
    CAmount resolved_total{0};
    int unresolved{0};
    for (const auto& contribution : m_session.contributions()) {
        if (contribution.hasChange) change_total += contribution.changeAmount;
        for (const auto& input : contribution.inputs) {
            if (const auto value{resolveInputValue(input)}) {
                resolved_total += *value;
            } else {
                ++unresolved;
            }
        }
    }
    QString funding_note;
    if (unresolved == 0) {
        const CAmount implied_fee{resolved_total - collateral - change_total};
        if (implied_fee <= 0) {
            QMessageBox::critical(
                this, tr("Funding is insufficient"),
                tr("The recorded funding inputs total %1, but the collateral (%2) plus change (%3) plus a positive "
                   "transaction fee requires more. The funding transaction could never be broadcast — adjust the "
                   "contributions before freezing.")
                    .arg(FormatAmount(m_wallet_model, resolved_total), FormatAmount(m_wallet_model, collateral),
                         FormatAmount(m_wallet_model, change_total)));
            return;
        }
        if (implied_fee > MAX_EXPECTED_FUNDING_FEE) {
            funding_note = tr("WARNING: the funding inputs exceed the collateral plus change by %1 — that entire "
                              "excess would be paid to miners as the transaction fee. Continue only if this is "
                              "intended.")
                               .arg(FormatAmount(m_wallet_model, implied_fee));
        } else {
            funding_note = tr("The funding inputs cover the collateral; %1 pays the transaction fee.")
                               .arg(FormatAmount(m_wallet_model, implied_fee));
        }
    } else {
        funding_note = tr("%n funding input(s) cannot be value-checked on this node yet (other participants' "
                          "unconfirmed contributions). Resolved inputs total %1 of the %2 collateral — make sure "
                          "every contribution is exactly its share total before freezing.",
                          nullptr, unresolved)
                           .arg(FormatAmount(m_wallet_model, resolved_total), FormatAmount(m_wallet_model, collateral));
    }

    const auto choice{QMessageBox::question(
        this, tr("Freeze the session terms?"),
        tr("Freezing prepares the registration transaction and fixes the share table, the terms and every funding "
           "input. Afterwards every share owner must sign the consent check phrase; any further change discards all "
           "collected signatures.") +
            QStringLiteral("\n\n") + funding_note + QStringLiteral("\n\n") +
            tr("Make sure every participant has contributed funding — the funding inputs cannot be changed without "
               "unfreezing."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes)};
    if (choice != QMessageBox::Yes) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("fundingTx", m_session.fundingTxHex().toStdString());
    UniValue shares(UniValue::VARR);
    for (const auto& share : m_session.shares()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("amount", share.amount);
        entry.pushKV("refundAddress", share.refundAddress.toStdString());
        if (!share.rewardAddress.isEmpty()) {
            entry.pushKV("rewardAddress", share.rewardAddress.toStdString());
        }
        entry.pushKV("ownerAddress", share.ownerAddress.toStdString());
        shares.push_back(entry);
    }
    params.pushKV("shares", shares);
    UniValue service_arr(UniValue::VARR);
    for (const QString& service : services) {
        service_arr.push_back(service.toStdString());
    }
    params.pushKV("coreP2PAddrs", service_arr);
    params.pushKV("operatorPubKey", terms.operatorPubKey.toStdString());
    params.pushKV("votingAddress", terms.votingAddress.toStdString());
    params.pushKV("operatorReward",
                  QStringLiteral("%1.%2").arg(terms.operatorReward / 100).arg(terms.operatorReward % 100, 2, 10, QLatin1Char('0')).toStdString());
    params.pushKV("earlyPeriodBlocks", static_cast<int64_t>(terms.earlyPeriodBlocks));
    params.pushKV("earlyPenalty", terms.earlyPenalty);

    // register_shared_prepare only assembles the transaction; no keys are involved
    ProTxResult result;
    if (!runRpc("protx register_shared_prepare", params, /*needs_unlock=*/false, result)) return;
    if (!result.ok) {
        QMessageBox::critical(this, tr("Prepare failed"), result.message);
        return;
    }
    const UniValue& tx{result.value.find_value("tx")};
    const UniValue& collateral_index{result.value.find_value("collateralIndex")};
    const UniValue& consent_hash{result.value.find_value("consentHash")};
    if (!result.value.isObject() || !tx.isStr() || !collateral_index.isNum() || !consent_hash.isStr()) {
        QMessageBox::critical(this, tr("Prepare failed"), tr("Unexpected reply to the prepare request."));
        return;
    }
    QString error;
    if (!m_session.freeze(QString::fromStdString(tx.get_str()), QString::fromStdString(consent_hash.get_str()),
                          collateral_index.getInt<int>(), error)) {
        QMessageBox::critical(this, tr("Prepare failed"), error);
        return;
    }
    if (m_wallet_model != nullptr) {
        m_session.setPrepareWallet(m_wallet_model->getWalletName());
    }
    markDirty();
    refreshAll();
    offerExport(tr("The terms are frozen. Every participant must now verify the check phrase and sign."));
}

void SharedMnCreateDialog::signConsent()
{
    if (m_busy || !canSign()) return;
    if (m_session.stage() != MnShareSession::Stage::Frozen && m_session.stage() != MnShareSession::Stage::Signing) {
        return;
    }

    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", m_session.protxHex().toStdString());
    ProTxResult result;
    if (!runRpc("protx shared_sign", params, /*needs_unlock=*/true, result)) return;
    if (!result.ok) {
        QMessageBox::critical(this, tr("Signing failed"), result.message);
        return;
    }
    if (!result.value.isArray()) {
        QMessageBox::critical(this, tr("Signing failed"), tr("Unexpected reply to the signing request."));
        return;
    }

    int added{0};
    QStringList errors;
    for (const auto& entry : result.value.getValues()) {
        try {
            const int share_index{entry.find_value("shareIndex").getInt<int>()};
            const QString signature{QString::fromStdString(entry.find_value("signature").get_str())};
            if (QString error; m_session.addSignature(share_index, signature, error)) {
                ++added;
            } else {
                errors << error;
            }
        } catch (const std::exception& e) {
            errors << QString::fromUtf8(e.what());
        }
    }
    if (!errors.isEmpty()) {
        ShowPlainMessage(this, QMessageBox::Warning, tr("Signing finished with warnings"), errors.join(QLatin1Char('\n')));
    }
    markDirty();
    refreshAll();
    if (added > 0) {
        offerExport(tr("%n signature(s) were recorded (%1 of %2 shares signed).", nullptr, added)
                        .arg(m_session.signedCount())
                        .arg(m_session.shares().size()));
    }
}

void SharedMnCreateDialog::unfreezeSession()
{
    if (m_session.stage() != MnShareSession::Stage::Frozen && m_session.stage() != MnShareSession::Stage::Signing) {
        return;
    }
    const int sigs{m_session.signedCount()};
    const auto choice{QMessageBox::question(
        this, tr("Unfreeze the session?"),
        tr("Unfreezing returns the session to a draft and discards %n collected signature(s). Everyone will have to "
           "sign again after the terms are re-frozen.",
           nullptr, sigs),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)};
    if (choice != QMessageBox::Yes) return;
    m_session.unfreeze();
    markDirty();
    refreshAll();
}

void SharedMnCreateDialog::combineSignatures()
{
    if (m_busy) return;
    const int total{static_cast<int>(m_session.shares().size())};
    if (m_session.signedCount() != total || total == 0) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", m_session.protxHex().toStdString());
    UniValue signatures(UniValue::VARR);
    for (const auto& sig : m_session.signatures()) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("shareIndex", sig.shareIndex);
        entry.pushKV("signature", sig.signatureB64.toStdString());
        signatures.push_back(entry);
    }
    params.pushKV("signatures", signatures);
    params.pushKV("submit", false);

    ProTxResult result;
    if (!runRpc("protx shared_combine", params, /*needs_unlock=*/false, result)) return;
    if (!result.ok) {
        QMessageBox::critical(this, tr("Combine failed"), result.message);
        return;
    }
    if (!result.value.isStr()) {
        QMessageBox::critical(this, tr("Combine failed"), tr("Unexpected reply to the combine request."));
        return;
    }
    QString error;
    if (!m_session.setCombinedTx(QString::fromStdString(result.value.get_str()), error)) {
        QMessageBox::critical(this, tr("Combine failed"), error);
        return;
    }
    markDirty();
    refreshAll();
    offerExport(tr("The consent signatures are combined. Every contributor must now sign their funding inputs."));
}

void SharedMnCreateDialog::signFundingInputs()
{
    if (m_busy || !canSign() || m_session.stage() != MnShareSession::Stage::Combined) return;

    const QString before{m_session.protxHex()};
    UniValue params(UniValue::VOBJ);
    params.pushKV("hexstring", before.toStdString());
    ProTxResult result;
    if (!runRpc("signrawtransactionwithwallet", params, /*needs_unlock=*/true, result)) return;
    if (!result.ok) {
        QMessageBox::critical(this, tr("Signing failed"), result.message);
        return;
    }
    const UniValue& hex{result.value.find_value("hex")};
    const UniValue& complete{result.value.find_value("complete")};
    if (!result.value.isObject() || !hex.isStr() || !complete.isBool()) {
        QMessageBox::critical(this, tr("Signing failed"), tr("Unexpected reply to the signing request."));
        return;
    }
    const QString signed_hex{QString::fromStdString(hex.get_str())};

    QString error;
    if (complete.get_bool()) {
        if (!m_session.setFundingSignedTx(signed_hex, error)) {
            QMessageBox::critical(this, tr("Signing failed"), error);
            return;
        }
        markDirty();
        refreshAll();
        offerExport(tr("All funding inputs are signed. The registration is ready to broadcast."));
        return;
    }

    if (signed_hex == before) {
        QMessageBox::information(this, windowTitle(),
                                 tr("This wallet could not add any funding signatures. Pass the session to a "
                                    "participant whose inputs are still pending."));
        return;
    }
    if (!replaceSessionProTx(signed_hex, error)) {
        QMessageBox::critical(this, tr("Signing failed"), error);
        return;
    }
    markDirty();
    refreshAll();
    offerExport(tr("This wallet's funding inputs are signed; other contributors still have to sign theirs. Pass "
                   "the session to the next participant."));
}

bool SharedMnCreateDialog::replaceSessionProTx(const QString& tx_hex, QString& error)
{
    // The session API only accepts a *complete* funding-signed transaction; a
    // partially signed one is stored by rewriting the envelope's protx field
    // and re-parsing (fromJson leaves the session untouched on failure)
    UniValue json{m_session.toJson()};
    json.pushKV("protx", tx_hex.toStdString());
    return m_session.fromJson(json, error);
}

void SharedMnCreateDialog::broadcastSession()
{
    if (m_busy || m_session.stage() != MnShareSession::Stage::FundingSigned) return;

    const auto choice{QMessageBox::question(
        this, tr("Broadcast the registration?"),
        tr("This sends the shared masternode registration to the network. It cannot be undone; the collateral can "
           "only be recovered through a dissolution afterwards."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes)};
    if (choice != QMessageBox::Yes) return;

    UniValue params(UniValue::VOBJ);
    params.pushKV("hexstring", m_session.protxHex().toStdString());
    ProTxResult result;
    if (!runRpc("sendrawtransaction", params, /*needs_unlock=*/false, result)) return;
    if (!result.ok) {
        QMessageBox::critical(this, tr("Broadcast failed"), result.message);
        return;
    }
    QString error;
    if (!m_session.setBroadcast(error)) {
        QMessageBox::critical(this, tr("Broadcast failed"), error);
        return;
    }
    markDirty();
    refreshAll();
    offerExport(tr("The registration was broadcast. Share the final session copy with every participant for their "
                   "records."));
}

void SharedMnCreateDialog::restartFromDraft()
{
    unlockAllContributedCoins();
    m_session = MnShareSession();
    m_dead = false;
    m_dead_reason.clear();
    m_dirty = false;
    m_operator_key_from_import = false;
    refreshFundingCandidates();
    refreshAll();
}

void SharedMnCreateDialog::reject()
{
    if (m_busy) return;
    if (m_dead) {
        // A dead session can never continue; closing must not strand the
        // funding coin locks (the restart button unlocks them the same way)
        unlockAllContributedCoins();
        QDialog::reject();
        return;
    }
    if (m_dirty) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(windowTitle());
        box.setText(tr("The session has unsaved changes. A participant without the latest copy cannot continue the "
                       "session."));
        if (!m_session.contributions().empty() && m_session.stage() != MnShareSession::Stage::Broadcast) {
            box.setInformativeText(tr("Discarding also unlocks the coins this session reserved for funding; save "
                                      "instead to keep them reserved and continue later."));
        }
        box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Save);
        switch (box.exec()) {
        case QMessageBox::Save:
            if (!writeSessionToFile()) return;
            break; // saved to continue later: the funding coins stay locked
        case QMessageBox::Discard:
            if (m_session.stage() != MnShareSession::Stage::Broadcast) unlockAllContributedCoins();
            break;
        default:
            return;
        }
    }
    QDialog::reject();
}
