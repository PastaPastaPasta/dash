// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sharedmncreatedialog.h>

#include <bls/bls.h>
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
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
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

QSet<QString> WalletOutpoints(const WalletModel* wallet_model)
{
    QSet<QString> ret;
    if (wallet_model == nullptr) return ret;
    for (const auto& [dest, coins] : wallet_model->wallet().listCoins()) {
        for (const auto& [outpoint, txout] : coins) {
            ret.insert(OutpointKey(QString::fromStdString(outpoint.hash.ToString()), outpoint.n));
        }
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
    setObjectName(QStringLiteral("SharedMnCreateDialog"));
    setMinimumSize(900, 720);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(buildHeader());

    m_pages = new QStackedWidget(this);
    m_pages->insertWidget(PageLanding, buildLandingPage());
    m_pages->insertWidget(PageParticipants, buildParticipantsPage());
    m_pages->insertWidget(PageSettings, buildSettingsPage());
    m_pages->insertWidget(PageFunding, buildFundingPage());
    m_pages->insertWidget(PageReview, buildReviewPage());
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
    m_role_label = new QLabel(header);
    GUIUtil::setFont({m_role_label}, GUIUtil::FontWeight::Bold);
    top_row->addWidget(m_role_label);

    m_breadcrumb = new QLabel(header);
    m_breadcrumb->setTextFormat(Qt::RichText);
    top_row->addWidget(m_breadcrumb, /*stretch=*/1);

    m_import_button = new QPushButton(tr("Paste Participant Reply"), header);
    m_import_button->setToolTip(tr("Merge a participant's latest reply from the clipboard into this session."));
    connect(m_import_button, &QPushButton::clicked, this, &SharedMnCreateDialog::importFromClipboard);
    top_row->addWidget(m_import_button);

    m_export_button = new QPushButton(tr("Copy Update"), header);
    m_export_button->setToolTip(tr("Copy the latest session update for another participant."));
    connect(m_export_button, &QPushButton::clicked, this, &SharedMnCreateDialog::exportToClipboard);
    top_row->addWidget(m_export_button);

    m_more_button = new QPushButton(header);
    m_more_button->setText(tr("More"));
    auto* recovery_menu = new QMenu(m_more_button);
    m_load_action = recovery_menu->addAction(tr("Merge Participant Reply File…"), this,
                                             &SharedMnCreateDialog::loadFromFile);
    recovery_menu->addAction(tr("Save Session Backup…"), this, &SharedMnCreateDialog::saveToFile);
    m_more_button->setMenu(recovery_menu);
    top_row->addWidget(m_more_button);
    layout->addLayout(top_row);

    m_next_action_label = MakeHint(QString(), header);
    layout->addWidget(m_next_action_label);

    return header;
}

QWidget* SharedMnCreateDialog::buildLandingPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->addStretch();

    auto* title = new QLabel(tr("Set up a shared masternode"), page);
    title->setAlignment(Qt::AlignCenter);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 22);
    layout->addWidget(title);

    auto* subtitle = MakeHint(tr("Drafting is a sequential handoff: each participant adds only their own details "
                                 "and funding. Everyone then approves the locked terms; contribution signing is one "
                                 "parallel round."),
                              page);
    subtitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(subtitle);

    auto* choices = new QHBoxLayout();
    auto* create_box = new QGroupBox(tr("Create a shared masternode"), page);
    auto* create_layout = new QVBoxLayout(create_box);
    create_layout->addWidget(MakeHint(tr("I will define the participants and amounts, complete My share, and pass "
                                         "the draft from participant to participant. When it returns complete, I "
                                         "will lock it, collect approvals, then send one contribution-signing request "
                                         "to everyone and broadcast the combined result."),
                                      create_box));
    create_layout->addStretch();
    m_create_session_button = new QPushButton(tr("Create New Session"), create_box);
    connect(m_create_session_button, &QPushButton::clicked, this, &SharedMnCreateDialog::createSession);
    create_layout->addWidget(m_create_session_button);
    m_resume_coordinator_button = new QPushButton(tr("Resume My Session Backup…"), create_box);
    connect(m_resume_coordinator_button, &QPushButton::clicked, this, &SharedMnCreateDialog::resumeCoordinatorSession);
    create_layout->addWidget(m_resume_coordinator_button);
    m_returned_session_button = new QPushButton(tr("Resume Session From Clipboard"), create_box);
    connect(m_returned_session_button, &QPushButton::clicked, this, &SharedMnCreateDialog::resumeCoordinatorFromClipboard);
    create_layout->addWidget(m_returned_session_button);
    choices->addWidget(create_box);

    auto* join_box = new QGroupBox(tr("Join an existing session"), page);
    auto* join_layout = new QVBoxLayout(join_box);
    join_layout->addWidget(MakeHint(tr("I received the draft baton, an approval request, or a contribution-signing "
                                       "request. I will complete only My share or the one action requested, then "
                                       "return the update."),
                                    join_box));
    join_layout->addStretch();
    m_join_clipboard_button = new QPushButton(tr("Paste Coordinator Update"), join_box);
    connect(m_join_clipboard_button, &QPushButton::clicked, this, &SharedMnCreateDialog::joinFromClipboard);
    join_layout->addWidget(m_join_clipboard_button);
    m_join_file_button = new QPushButton(tr("Open Coordinator File…"), join_box);
    connect(m_join_file_button, &QPushButton::clicked, this, &SharedMnCreateDialog::joinFromFile);
    join_layout->addWidget(m_join_file_button);
    choices->addWidget(join_box);
    layout->addLayout(choices);
    layout->addStretch();
    return page;
}

QWidget* SharedMnCreateDialog::buildParticipantsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(tr("Participants and shares"), page);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 18);
    layout->addWidget(title);
    m_participants_hint = MakeHint(QString(), page);
    layout->addWidget(m_participants_hint);

    auto* my_share_box = new QGroupBox(tr("My share"), page);
    auto* my_share_layout = new QVBoxLayout(my_share_box);
    m_my_share_hint = MakeHint(tr("Choose who you are in this session. The fields below are the only participant "
                                  "details this wallet should add."),
                               my_share_box);
    my_share_layout->addWidget(m_my_share_hint);
    auto* my_share_form = new QFormLayout();
    my_share_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_my_share_combo = new QComboBox(my_share_box);
    connect(m_my_share_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &SharedMnCreateDialog::onMyShareChanged);
    my_share_form->addRow(tr("I am:"), m_my_share_combo);

    const auto add_address_row = [this, my_share_box, my_share_form](const QString& label, QLineEdit*& edit,
                                                                     QPushButton*& button, const QString& button_text,
                                                                     auto slot) {
        auto* row = new QWidget(my_share_box);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        edit = new QLineEdit(row);
        connect(edit, &QLineEdit::editingFinished, this, &SharedMnCreateDialog::onMyShareDetailsChanged);
        row_layout->addWidget(edit, /*stretch=*/1);
        button = new QPushButton(button_text, row);
        connect(button, &QPushButton::clicked, this, slot);
        row_layout->addWidget(button);
        my_share_form->addRow(label, row);
    };
    add_address_row(tr("My owner address:"), m_my_owner_edit, m_my_address_button, tr("Use This Wallet"),
                    &SharedMnCreateDialog::useMyOwnerAddress);
    add_address_row(tr("My refund address:"), m_my_refund_edit, m_my_refund_button, tr("Use This Wallet"),
                    &SharedMnCreateDialog::useMyRefundAddress);
    add_address_row(tr("My reward address (optional):"), m_my_reward_edit, m_my_reward_button, tr("Use This Wallet"),
                    &SharedMnCreateDialog::useMyRewardAddress);
    my_share_layout->addLayout(my_share_form);
    layout->addWidget(my_share_box);

    auto* shares_box = new QGroupBox(tr("All participants"), page);
    auto* shares_layout = new QVBoxLayout(shares_box);
    m_share_table = new QTableWidget(0, 5, shares_box);
    m_share_table->setHorizontalHeaderLabels(
        {tr("Label"), tr("Amount (DASH)"), tr("Owner address"), tr("Refund address"), tr("Reward address (optional)")});
    m_share_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_share_table->horizontalHeader()->setSectionResizeMode(COL_LABEL, QHeaderView::ResizeToContents);
    m_share_table->horizontalHeader()->setSectionResizeMode(COL_AMOUNT, QHeaderView::ResizeToContents);
    m_share_table->verticalHeader()->setVisible(false);
    connect(m_share_table, &QTableWidget::cellChanged, this, &SharedMnCreateDialog::onShareCellChanged);
    m_share_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    shares_layout->addWidget(m_share_table);

    auto* share_buttons = new QHBoxLayout();
    m_add_share_button = new QPushButton(tr("Add Share"), shares_box);
    connect(m_add_share_button, &QPushButton::clicked, this, &SharedMnCreateDialog::addShareRow);
    share_buttons->addWidget(m_add_share_button);
    m_remove_share_button = new QPushButton(tr("Remove Share"), shares_box);
    connect(m_remove_share_button, &QPushButton::clicked, this, &SharedMnCreateDialog::removeShareRow);
    share_buttons->addWidget(m_remove_share_button);
    share_buttons->addStretch();
    m_sum_label = new QLabel(shares_box);
    share_buttons->addWidget(m_sum_label);
    shares_layout->addLayout(share_buttons);

    layout->addWidget(shares_box, /*stretch=*/1);

    auto* nav = new QHBoxLayout();
    auto* cancel = new QPushButton(tr("Cancel"), page);
    connect(cancel, &QPushButton::clicked, this, &SharedMnCreateDialog::reject);
    nav->addWidget(cancel);
    nav->addStretch();
    auto* next = new QPushButton(tr("Continue to Masternode Settings"), page);
    connect(next, &QPushButton::clicked, this, &SharedMnCreateDialog::showSettingsPage);
    nav->addWidget(next);
    layout->addLayout(nav);

    if (m_wallet_model == nullptr) {
        for (QPushButton* button : {m_my_address_button, m_my_refund_button, m_my_reward_button}) {
            button->setEnabled(false);
            button->setToolTip(tr("No wallet is available."));
        }
    }
    return page;
}

QWidget* SharedMnCreateDialog::buildSettingsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    m_settings_title = new QLabel(page);
    GUIUtil::setFont({m_settings_title}, GUIUtil::FontWeight::Bold, 18);
    layout->addWidget(m_settings_title);
    m_settings_hint = MakeHint(QString(), page);
    layout->addWidget(m_settings_hint);

    auto* scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("sharedMnSettingsScroll"));
    scroll->viewport()->setObjectName(QStringLiteral("sharedMnSettingsViewport"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("sharedMnSettingsContent"));
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSizeConstraint(QLayout::SetMinAndMaxSize);

    auto* terms_box = new QGroupBox(tr("Registration settings"), content);
    auto* terms_form = new QFormLayout(terms_box);
    terms_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_service_edit = new QLineEdit(terms_box);
    m_service_edit->setPlaceholderText(tr("IP:PORT — leave empty to set later with a service update"));
    connect(m_service_edit, &QLineEdit::textChanged, this, &SharedMnCreateDialog::onTermsChanged);
    terms_form->addRow(tr("Service address:"), m_service_edit);

    m_operator_widget = new OperatorKeyWidget(terms_box);
    connect(m_operator_widget, &OperatorKeyWidget::changed, this, &SharedMnCreateDialog::onTermsChanged);
    terms_form->addRow(tr("Operator key:"), m_operator_widget);
    m_operator_field_label = qobject_cast<QLabel*>(terms_form->labelForField(m_operator_widget));

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
    auto* voting_row = new QWidget(terms_box);
    auto* voting_layout = new QHBoxLayout(voting_row);
    voting_layout->setContentsMargins(0, 0, 0, 0);
    voting_layout->addWidget(m_voting_edit, /*stretch=*/1);
    m_my_voting_button = new QPushButton(tr("Use Address From This Wallet"), voting_row);
    m_my_voting_button->setToolTip(tr("Generate a voting address in this wallet."));
    connect(m_my_voting_button, &QPushButton::clicked, this, &SharedMnCreateDialog::useMyVotingAddress);
    voting_layout->addWidget(m_my_voting_button);
    terms_form->addRow(tr("Voting address:"), voting_row);

    content_layout->addWidget(terms_box);

    auto* advanced_box = new QGroupBox(tr("Advanced reward and dissolution terms"), content);
    auto* advanced_layout = new QVBoxLayout(advanced_box);
    auto* advanced_toggle = new QToolButton(advanced_box);
    advanced_toggle->setText(tr("Show advanced terms"));
    advanced_toggle->setArrowType(Qt::RightArrow);
    advanced_toggle->setCheckable(true);
    advanced_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced_layout->addWidget(advanced_toggle, /*stretch=*/0, Qt::AlignLeft);
    auto* advanced_body = new QWidget(advanced_box);
    auto* advanced_form = new QFormLayout(advanced_body);
    advanced_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    advanced_layout->addWidget(advanced_body);
    advanced_body->setVisible(false);
    connect(advanced_toggle, &QToolButton::toggled, this, [advanced_toggle, advanced_body](bool visible) {
        advanced_toggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
        advanced_body->setVisible(visible);
    });

    m_operator_reward_spin = new QDoubleSpinBox(advanced_box);
    m_operator_reward_spin->setRange(0.0, 100.0);
    m_operator_reward_spin->setDecimals(2);
    m_operator_reward_spin->setSuffix(QStringLiteral(" %"));
    connect(m_operator_reward_spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &SharedMnCreateDialog::onTermsChanged);
    advanced_form->addRow(tr("Operator reward:"), m_operator_reward_spin);

    m_early_period_spin = new QSpinBox(advanced_box);
    m_early_period_spin->setRange(0, static_cast<int>(CProRegTx::MAX_EARLY_PERIOD_BLOCKS));
    connect(m_early_period_spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &SharedMnCreateDialog::onTermsChanged);
    m_early_period_hint = new QLabel(advanced_box);
    auto* early_period_box = new QVBoxLayout();
    early_period_box->addWidget(m_early_period_spin);
    early_period_box->addWidget(m_early_period_hint);
    advanced_form->addRow(tr("Early period (blocks):"), early_period_box);

    m_early_penalty_field = new BitcoinAmountField(advanced_box);
    connect(m_early_penalty_field, &BitcoinAmountField::valueChanged, this, &SharedMnCreateDialog::onTermsChanged);
    m_early_penalty_hint = new QLabel(advanced_box);
    m_early_penalty_hint->setWordWrap(true);
    auto* early_penalty_box = new QVBoxLayout();
    early_penalty_box->addWidget(m_early_penalty_field);
    early_penalty_box->addWidget(m_early_penalty_hint);
    advanced_form->addRow(tr("Early penalty:"), early_penalty_box);
    content_layout->addWidget(advanced_box);
    content_layout->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, /*stretch=*/1);

    auto* nav = new QHBoxLayout();
    auto* back = new QPushButton(tr("Back"), page);
    connect(back, &QPushButton::clicked, this, &SharedMnCreateDialog::showParticipantsPage);
    nav->addWidget(back);
    nav->addStretch();
    auto* next = new QPushButton(tr("Continue to Funding"), page);
    connect(next, &QPushButton::clicked, this, &SharedMnCreateDialog::showFundingPage);
    nav->addWidget(next);
    layout->addLayout(nav);
    return page;
}

QWidget* SharedMnCreateDialog::buildFundingPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(tr("Collect funding"), page);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 18);
    layout->addWidget(title);
    layout->addWidget(MakeHint(tr("Prepare your contribution, then share the updated session. Each participant "
                                  "contributes exactly their total share amount."),
                               page));

    auto* funding_box = new QGroupBox(tr("My contribution"), page);
    auto* funding_layout = new QVBoxLayout(funding_box);
    funding_layout->addWidget(
        MakeHint(tr("Each participant contributes a single output of exactly their total share amount. The "
                    "coordinator additionally contributes one small input to pay the transaction fee and takes "
                    "the only change output."),
                 funding_box));

    m_fee_box = new QGroupBox(tr("Coordinator transaction fee"), funding_box);
    auto* fee_layout = new QVBoxLayout(m_fee_box);
    auto* fee_toggle = new QToolButton(m_fee_box);
    fee_toggle->setText(tr("Show fee settings"));
    fee_toggle->setArrowType(Qt::RightArrow);
    fee_toggle->setCheckable(true);
    fee_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    fee_layout->addWidget(fee_toggle, /*stretch=*/0, Qt::AlignLeft);
    auto* fee_body = new QWidget(m_fee_box);
    auto* fee_form = new QFormLayout(fee_body);
    fee_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    fee_layout->addWidget(fee_body);
    fee_body->setVisible(false);
    connect(fee_toggle, &QToolButton::toggled, this, [fee_toggle, fee_body](bool visible) {
        fee_toggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
        fee_body->setVisible(visible);
    });
    m_fee_amount_field = new BitcoinAmountField(fee_body);
    m_fee_amount_field->setValue(100000); // 0.001 DASH covers even a maximal 8-share registration
    fee_form->addRow(tr("Registration fee:"), m_fee_amount_field);

    m_fee_utxo_combo = new QComboBox(fee_body);
    m_fee_utxo_combo->setObjectName(QStringLiteral("sharedMnFeeUtxoCombo"));
    m_fee_utxo_combo->setToolTip(tr("A small wallet output the coordinator adds to pay the fee from."));
    fee_form->addRow(tr("Wallet output used for fee:"), m_fee_utxo_combo);
    connect(m_fee_amount_field, &BitcoinAmountField::valueChanged, this, &SharedMnCreateDialog::refreshFundingCandidates);
    funding_layout->addWidget(m_fee_box);

    auto* funding_buttons = new QHBoxLayout();
    m_add_funding_button = new QPushButton(tr("Prepare My Contribution"), funding_box);
    connect(m_add_funding_button, &QPushButton::clicked, this, &SharedMnCreateDialog::addMyFunding);
    funding_buttons->addWidget(m_add_funding_button);
    m_remove_funding_button = new QPushButton(tr("Remove My Contribution"), funding_box);
    connect(m_remove_funding_button, &QPushButton::clicked, this, &SharedMnCreateDialog::removeMyContribution);
    funding_buttons->addWidget(m_remove_funding_button);
    m_refresh_funding_button = new QPushButton(tr("Refresh"), funding_box);
    connect(m_refresh_funding_button, &QPushButton::clicked, this, &SharedMnCreateDialog::refreshFundingCandidates);
    funding_buttons->addWidget(m_refresh_funding_button);
    funding_buttons->addStretch();
    funding_layout->addLayout(funding_buttons);

    m_contrib_list = new QListWidget(funding_box);
    m_contrib_list->setObjectName(QStringLiteral("sharedMnSessionList"));
    m_contrib_list->setToolTip(tr("Funding contributions recorded in the session."));
    m_contrib_list->setSelectionMode(QAbstractItemView::NoSelection);
    funding_layout->addWidget(m_contrib_list);
    m_funding_status_label = new QLabel(funding_box);
    m_funding_status_label->setObjectName(QStringLiteral("sharedMnFundingStatusLabel"));
    m_funding_status_label->setWordWrap(true);
    funding_layout->addWidget(m_funding_status_label);
    layout->addWidget(funding_box, /*stretch=*/1);

    auto* nav = new QHBoxLayout();
    auto* back = new QPushButton(tr("Back"), page);
    connect(back, &QPushButton::clicked, this, &SharedMnCreateDialog::showSettingsPage);
    nav->addWidget(back);
    nav->addStretch();
    auto* next = new QPushButton(tr("Continue to Review"), page);
    connect(next, &QPushButton::clicked, this, &SharedMnCreateDialog::showReviewPage);
    nav->addWidget(next);
    layout->addLayout(nav);

    if (m_wallet_model == nullptr) {
        for (QPushButton* button : {m_add_funding_button, m_remove_funding_button}) {
            button->setEnabled(false);
            button->setToolTip(tr("No wallet is available."));
        }
    }
    return page;
}

QWidget* SharedMnCreateDialog::buildReviewPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    m_review_title = new QLabel(page);
    GUIUtil::setFont({m_review_title}, GUIUtil::FontWeight::Bold, 18);
    layout->addWidget(m_review_title);
    m_review_hint = MakeHint(QString(), page);
    layout->addWidget(m_review_hint);

    m_review_summary = new QLabel(page);
    m_review_summary->setTextFormat(Qt::RichText);
    m_review_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_review_summary->setWordWrap(true);
    layout->addWidget(m_review_summary);

    auto* validation_box = new QGroupBox(tr("Readiness"), page);
    auto* validation_layout = new QVBoxLayout(validation_box);
    m_validation_list = new QListWidget(validation_box);
    m_validation_list->setObjectName(QStringLiteral("sharedMnSessionList"));
    m_validation_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_validation_list->setFocusPolicy(Qt::NoFocus);
    validation_layout->addWidget(m_validation_list);
    layout->addWidget(validation_box, /*stretch=*/1);

    auto* nav = new QHBoxLayout();
    auto* back = new QPushButton(tr("Back"), page);
    connect(back, &QPushButton::clicked, this, &SharedMnCreateDialog::showFundingPage);
    nav->addWidget(back);
    nav->addStretch();
    m_freeze_button = new QPushButton(tr("Lock Terms and Request Approval"), page);
    connect(m_freeze_button, &QPushButton::clicked, this, &SharedMnCreateDialog::copyOrFreezeDraft);
    nav->addWidget(m_freeze_button);
    layout->addLayout(nav);
    return page;
}

QWidget* SharedMnCreateDialog::buildSigningPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* sheet_box = new QGroupBox(tr("Locked terms"), page);
    auto* sheet_layout = new QVBoxLayout(sheet_box);
    m_term_sheet = new QLabel(sheet_box);
    m_term_sheet->setTextFormat(Qt::RichText);
    m_term_sheet->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_term_sheet->setWordWrap(true);
    sheet_layout->addWidget(m_term_sheet);
    layout->addWidget(sheet_box, /*stretch=*/1);

    auto* sigs_box = new QGroupBox(tr("Owner approvals"), page);
    auto* sigs_layout = new QVBoxLayout(sigs_box);
    m_sig_table = new QTableWidget(0, 3, sigs_box);
    m_sig_table->setHorizontalHeaderLabels({tr("Share"), tr("Participant"), tr("Approval")});
    m_sig_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_sig_table->horizontalHeader()->setStretchLastSection(true);
    m_sig_table->verticalHeader()->setVisible(false);
    m_sig_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sig_table->setSelectionMode(QAbstractItemView::NoSelection);
    sigs_layout->addWidget(m_sig_table);

    auto* sign_row = new QHBoxLayout();
    m_sign_button = new QPushButton(tr("Approve Locked Terms"), sigs_box);
    connect(m_sign_button, &QPushButton::clicked, this, &SharedMnCreateDialog::signConsent);
    sign_row->addWidget(m_sign_button);
    m_unfreeze_button = new QPushButton(tr("Unlock Terms…"), sigs_box);
    m_unfreeze_button->setToolTip(tr("Return to editing. All collected approvals are discarded."));
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
    m_combine_button = new QPushButton(tr("Combine Approvals"), page);
    m_combine_button->setToolTip(tr("Embed every share owner's approval into the transaction."));
    connect(m_combine_button, &QPushButton::clicked, this, &SharedMnCreateDialog::combineSignatures);
    combine_row->addWidget(m_combine_button);
    combine_row->addStretch();
    layout->addLayout(combine_row);

    auto* funding_box = new QGroupBox(tr("Contribution signatures"), page);
    auto* funding_layout = new QVBoxLayout(funding_box);
    funding_layout->addWidget(
        MakeHint(tr("This is one parallel signing round. Give every contributor the same combined update; each "
                    "person signs only their own contribution and returns that signed copy to the coordinator."),
                 funding_box));
    m_funding_sig_list = new QListWidget(funding_box);
    m_funding_sig_list->setObjectName(QStringLiteral("sharedMnSessionList"));
    m_funding_sig_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_funding_sig_list->setFocusPolicy(Qt::NoFocus);
    funding_layout->addWidget(m_funding_sig_list);

    auto* sign_row = new QHBoxLayout();
    m_sign_funding_button = new QPushButton(tr("Sign My Contribution"), funding_box);
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
    for (QPushButton* button : {m_import_button, m_export_button}) {
        button->setEnabled(!busy);
    }
    m_more_button->setEnabled(!busy);
}

void SharedMnCreateDialog::refreshAll()
{
    if (m_dead) {
        m_dead_label->setText(m_dead_reason);
        m_pages->setCurrentIndex(PageDead);
        refreshHeader();
        return;
    }
    if (m_role == Role::Undecided) {
        m_pages->setCurrentIndex(PageLanding);
        refreshHeader();
        return;
    }
    const int total{static_cast<int>(m_session.shares().size())};
    const bool all_signed{total > 0 && m_session.signedCount() == total};
    switch (m_session.stage()) {
    case MnShareSession::Stage::Draft:
        refreshDraftPage();
        if (!isDraftPage(m_pages->currentIndex())) m_pages->setCurrentIndex(PageParticipants);
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
    refreshHeader();
}

void SharedMnCreateDialog::refreshHeader()
{
    const bool landing{m_pages->currentIndex() == PageLanding};
    for (QWidget* widget : {static_cast<QWidget*>(m_role_label), static_cast<QWidget*>(m_breadcrumb),
                            static_cast<QWidget*>(m_next_action_label), static_cast<QWidget*>(m_import_button),
                            static_cast<QWidget*>(m_export_button), static_cast<QWidget*>(m_more_button)}) {
        widget->setVisible(!landing);
    }
    if (landing) {
        setWindowTitle(tr("Shared Masternode"));
        const bool has_current_session{!m_session.shares().empty()};
        const bool coordinating{has_current_session && m_role == Role::Coordinator};
        const bool participating{has_current_session && m_role == Role::Participant};
        m_create_session_button->setText(coordinating ? tr("Return to Current Session") : tr("Create New Session"));
        m_create_session_button->setEnabled(!participating);
        m_resume_coordinator_button->setText(coordinating ? tr("Merge Returned Session File…")
                                                           : tr("Resume My Session Backup…"));
        m_resume_coordinator_button->setEnabled(!participating);
        m_returned_session_button->setText(coordinating ? tr("Paste Returned Draft")
                                                         : tr("Resume Session From Clipboard"));
        m_returned_session_button->setEnabled(!participating);
        m_returned_session_button->setToolTip(
            coordinating ? tr("Adopt the newer draft returned by the last participant.")
            : participating ? tr("Finish the current participant session before coordinating another session.")
                            : tr("Resume a coordinator session backup copied to the clipboard."));
        m_join_clipboard_button->setText(participating ? tr("Paste Next Coordinator Update")
                                                       : tr("Paste Coordinator Update"));
        m_join_file_button->setText(participating ? tr("Open Next Coordinator File…")
                                                  : tr("Open Coordinator File…"));
        m_join_clipboard_button->setEnabled(!coordinating);
        m_join_file_button->setEnabled(!coordinating);
        const QString join_tooltip{coordinating ? tr("Return or finish the current coordinator session first.")
                                                : participating
                ? tr("Import the next phase for this participant without releasing the reserved funding.")
                : QString()};
        m_join_clipboard_button->setToolTip(join_tooltip);
        m_join_file_button->setToolTip(join_tooltip);
        return;
    }

    setWindowTitle(tr("Shared Masternode — %1").arg(m_session.sessionId().left(8)));
    const bool coordinator{m_role == Role::Coordinator};
    m_role_label->setText(coordinator ? tr("Coordinator") : tr("Participant"));
    const bool collecting_approvals{m_session.stage() == MnShareSession::Stage::Frozen ||
                                    m_session.stage() == MnShareSession::Stage::Signing};
    const bool collecting_contributions{m_session.stage() == MnShareSession::Stage::Combined};
    m_import_button->setVisible(coordinator && (collecting_approvals || collecting_contributions));
    m_import_button->setText(collecting_contributions ? tr("Paste Signed Contribution") : tr("Paste Approval Reply"));
    m_import_button->setToolTip(
        collecting_contributions
            ? tr("Merge one participant's signed contribution from the clipboard.")
            : tr("Merge one participant's approval of the locked terms from the clipboard."));
    m_load_action->setVisible(coordinator && (collecting_approvals || collecting_contributions));
    m_load_action->setText(collecting_contributions ? tr("Merge Signed Contribution File…")
                                                    : tr("Merge Approval Reply File…"));
    m_export_button->setVisible(m_session.stage() != MnShareSession::Stage::Draft);

    QString task;
    QString next;
    switch (m_pages->currentIndex()) {
    case PageParticipants:
        task = tr("Step 1 of 4 · Participants");
        next = m_role == Role::Coordinator
                   ? tr("Next: define the participant roster, choose your own share and add your wallet addresses.")
                   : tr("Next: confirm your share and add only this wallet's addresses.");
        break;
    case PageSettings:
        task = tr("Step 2 of 4 · Settings");
        next = m_role == Role::Coordinator
                   ? tr("Next: choose the operator and voting keys, then continue to funding.")
                   : tr("Next: review the coordinator's settings, then continue to your contribution.");
        break;
    case PageFunding:
        task = tr("Step 3 of 4 · Funding");
        next = tr("Next: prepare the contribution for My share, then review the updated draft.");
        break;
    case PageReview:
        task = tr("Step 4 of 4 · Review");
        if (m_role == Role::Coordinator && m_freeze_button->property("canFreeze").toBool()) {
            next = tr("Next: lock the complete draft, then send one approval request to every participant.");
        } else if (m_freeze_button->isEnabled()) {
            next = tr("Next: %1.").arg(m_freeze_button->text());
        } else {
            next = tr("Next: complete My share details and funding before handing off the draft.");
        }
        break;
    case PageSigning:
        task = tr("Approve locked terms");
        {
            const auto my_indexes{myShareIndexes()};
            const bool mine_pending{std::any_of(my_indexes.begin(), my_indexes.end(),
                                                [this](int index) { return m_session.signatureFor(index).isEmpty(); })};
            if (mine_pending) {
                next = tr("Next: review every locked term and approve your share once.");
            } else if (m_role == Role::Coordinator) {
                next = tr("Next: paste participant approval replies (%1 of %2 shares approved).")
                           .arg(m_session.signedCount())
                           .arg(m_session.shares().size());
            } else {
                next = tr("Next: copy your approved update for the coordinator.");
            }
        }
        break;
    case PageCombined:
        if (m_session.stage() != MnShareSession::Stage::Combined) {
            task = tr("Combine approvals");
            next = m_role == Role::Coordinator
                       ? tr("Next: combine every approval, then send one contribution-signing request to everyone.")
                       : tr("Next: copy the complete approval update for the coordinator to combine.");
        } else {
            task = tr("Sign contributions · one parallel round");
            const auto signed_map{InputSignatureMap(m_session.protxHex())};
            int complete_contributions{0};
            bool mine_pending{false};
            const QSet<QString> wallet_outpoints{WalletOutpoints(m_wallet_model)};
            for (const auto& contribution : m_session.contributions()) {
                bool complete{true};
                for (const auto& input : contribution.inputs) {
                    const QString key{OutpointKey(input.txid, input.vout)};
                    const auto it{signed_map.find(key)};
                    const bool signed_input{it != signed_map.end() && it->second};
                    complete &= signed_input;
                    mine_pending |= !signed_input && wallet_outpoints.contains(key);
                }
                if (complete) ++complete_contributions;
            }
            if (mine_pending) {
                next = tr("Next: sign this wallet's contribution once, then return or collect the parallel replies.");
            } else if (m_role == Role::Coordinator) {
                next = tr("Next: paste signed contributions (%1 of %2 complete).")
                           .arg(complete_contributions)
                           .arg(m_session.contributions().size());
            } else {
                next = tr("Next: copy your signed contribution for the coordinator.");
            }
        }
        break;
    case PageBroadcast:
        task = m_session.stage() == MnShareSession::Stage::Broadcast ? tr("Registration complete")
                                                                     : tr("Ready to broadcast");
        next = m_session.stage() == MnShareSession::Stage::Broadcast
                   ? tr("Done: keep a backup of the final session and prepare standby dissolutions after confirmation.")
                   : (m_role == Role::Coordinator ? tr("Next: broadcast the fully signed registration.")
                                                  : tr("Next: copy the fully signed update for the coordinator."));
        break;
    case PageDead:
        task = tr("Session cannot continue");
        next = tr("Next: unlock your reserved coins and restart with a new session.");
        break;
    }
    m_breadcrumb->setText(task + QStringLiteral(" &nbsp;·&nbsp; ") + tr("revision %1").arg(m_session.revision()));
    m_next_action_label->setText(QStringLiteral("<b>%1</b>").arg(next.toHtmlEscaped()));
    if (collecting_approvals) {
        m_export_button->setText(coordinator ? tr("Copy Approval Request") : tr("Copy My Approval"));
    } else if (collecting_contributions) {
        m_export_button->setText(coordinator ? tr("Copy Contribution-Signing Request")
                                             : tr("Copy My Signed Contribution"));
    } else {
        m_export_button->setText(tr("Copy Final Session"));
    }
    bool export_ready{true};
    if (!coordinator && collecting_approvals) {
        const auto mine{myShareIndexes()};
        export_ready = !mine.empty() && std::all_of(mine.begin(), mine.end(),
                                                   [this](int index) { return !m_session.signatureFor(index).isEmpty(); });
        m_export_button->setToolTip(export_ready ? tr("Return your approval to the coordinator.")
                                                 : tr("Approve the locked terms before copying your reply."));
    } else if (!coordinator && collecting_contributions) {
        const auto signed_map{InputSignatureMap(m_session.protxHex())};
        const QSet<QString> wallet_outpoints{WalletOutpoints(m_wallet_model)};
        bool controls_input{false};
        export_ready = true;
        for (const QString& outpoint : wallet_outpoints) {
            const auto it{signed_map.find(outpoint)};
            if (it == signed_map.end()) continue;
            controls_input = true;
            export_ready &= it->second;
        }
        export_ready &= controls_input;
        m_export_button->setToolTip(export_ready ? tr("Return your signed contribution to the coordinator.")
                                                 : tr("Sign this wallet's contribution before copying your reply."));
    } else {
        m_export_button->setToolTip(QString());
    }
    m_export_button->setEnabled(!m_busy && export_ready);
    if (m_role == Role::Participant && m_pages->currentIndex() == PageReview) {
        m_export_button->setVisible(false);
    }

}

void SharedMnCreateDialog::refreshDraftPage()
{
    refreshSharesTable();
    refreshMySharePanel();
    pushTermsToWidgets();
    const bool coordinator{m_role == Role::Coordinator};
    m_participants_hint->setText(
        coordinator ? tr("Define every participant and amount, then choose your own row under My share. Pass the "
                         "draft to each participant in order so they can add only their details.")
                    : tr("Confirm who you are under My share, add this wallet's addresses and funding, then pass the "
                         "updated draft to the next participant (or back to the coordinator if you are last)."));
    m_settings_title->setText(coordinator ? tr("Masternode settings") : tr("Review masternode settings"));
    m_settings_hint->setText(
        coordinator ? tr("Choose the operator and voting keys. Service details may be added later with a service "
                         "update.")
                    : tr("These shared settings come from the coordinator and are read-only. Your personal addresses "
                         "are on the My share page."));
    m_review_title->setText(coordinator ? tr("Review and lock terms") : tr("Review your session update"));
    m_review_hint->setText(
        coordinator ? tr("Review the complete allocation, settings, and funding status. Locking creates the exact "
                         "terms every owner will approve.")
                    : tr("Check your share addresses and contribution, then return this update to the coordinator. "
                         "Only the coordinator can lock the final terms."));
    for (QLineEdit* edit : {m_service_edit, m_secret_holder_edit, m_voting_edit}) {
        edit->setEnabled(coordinator);
    }
    m_my_voting_button->setEnabled(coordinator && m_wallet_model != nullptr);
    if (coordinator && !m_operator_key_from_import) {
        m_operator_widget->setEnabled(true);
    } else {
        m_operator_widget->setEnabled(false);
    }
    for (QWidget* widget : {static_cast<QWidget*>(m_operator_reward_spin), static_cast<QWidget*>(m_early_period_spin),
                            static_cast<QWidget*>(m_early_penalty_field)}) {
        widget->setEnabled(coordinator);
    }
    refreshContributions();
    refreshValidation();
    refreshReviewPage();
}

bool SharedMnCreateDialog::isDraftPage(int page) const { return page >= PageParticipants && page <= PageReview; }

void SharedMnCreateDialog::refreshSharesTable()
{
    m_updating = true;
    const auto& shares{m_session.shares()};
    m_share_table->setRowCount(static_cast<int>(shares.size()));
    for (int row = 0; row < static_cast<int>(shares.size()); ++row) {
        const auto& share{shares[row]};
        const bool funded{std::any_of(m_session.contributions().begin(), m_session.contributions().end(),
                                      [&share](const auto& contribution) { return contribution.label == share.label; })};
        const auto set_cell = [this, row, funded](int column, const QString& text) {
            auto* item = m_share_table->item(row, column);
            if (item == nullptr) {
                item = new QTableWidgetItem();
                m_share_table->setItem(row, column, item);
            }
            item->setText(text);
            const bool coordinator_structure{m_role == Role::Coordinator && !funded &&
                                             (column == COL_LABEL || column == COL_AMOUNT)};
            item->setFlags(coordinator_structure ? item->flags() | Qt::ItemIsEditable
                                                 : item->flags() & ~Qt::ItemIsEditable);
            item->setToolTip(
                funded && (column == COL_LABEL || column == COL_AMOUNT)
                    ? tr("Remove this participant's funding contribution before changing the name or amount.")
                    : QString());
        };
        set_cell(COL_LABEL, share.label);
        set_cell(COL_AMOUNT, share.amount > 0 ? AmountCellText(share.amount) : QString());
        set_cell(COL_OWNER, share.ownerAddress);
        set_cell(COL_REFUND, share.refundAddress);
        set_cell(COL_REWARD, share.rewardAddress);
    }
    m_updating = false;
    m_add_share_button->setEnabled(m_role == Role::Coordinator && shares.size() < CProRegTx::MAX_SHARES);
    m_remove_share_button->setEnabled(m_role == Role::Coordinator);
}

void SharedMnCreateDialog::refreshMySharePanel()
{
    const int previous{myShareIndex()};
    const auto& shares{m_session.shares()};
    m_updating = true;
    m_my_share_combo->clear();
    m_my_share_combo->addItem(tr("Choose your participant…"), -1);
    const auto wallet_indexes{myShareIndexes()};
    for (int i = 0; i < static_cast<int>(shares.size()); ++i) {
        const bool mine{std::find(wallet_indexes.begin(), wallet_indexes.end(), i) != wallet_indexes.end()};
        const QString status{mine ? tr("mine")
                                  : (shares[i].ownerAddress.isEmpty() || shares[i].refundAddress.isEmpty()
                                         ? tr("needs details")
                                         : tr("details complete"))};
        m_my_share_combo->addItem(
            tr("%1 — %2 (%3)").arg(ShareDisplayLabel(m_session, i), FormatAmount(m_wallet_model, shares[i].amount), status),
            i);
    }

    int selected{previous};
    if (selected < 0 || selected >= static_cast<int>(shares.size())) {
        if (wallet_indexes.size() == 1) {
            selected = wallet_indexes.front();
        } else {
            // A sequential draft naturally identifies the next participant as
            // the first row whose personal details have not been supplied yet.
            const auto incomplete = std::find_if(shares.begin(), shares.end(), [](const auto& share) {
                return share.ownerAddress.isEmpty() || share.refundAddress.isEmpty();
            });
            if (incomplete != shares.end()) {
                selected = static_cast<int>(std::distance(shares.begin(), incomplete));
            } else if (m_role == Role::Coordinator && !shares.empty()) {
                selected = 0;
            }
        }
    }
    m_my_share_combo->setCurrentIndex(selected >= 0 ? selected + 1 : 0);
    const bool valid{selected >= 0 && selected < static_cast<int>(shares.size())};
    m_my_owner_edit->setText(valid ? shares[selected].ownerAddress : QString());
    m_my_refund_edit->setText(valid ? shares[selected].refundAddress : QString());
    m_my_reward_edit->setText(valid ? shares[selected].rewardAddress : QString());
    m_updating = false;

    const bool mine{valid && std::find(wallet_indexes.begin(), wallet_indexes.end(), selected) != wallet_indexes.end()};
    const bool owner_unclaimed{valid && shares[selected].ownerAddress.isEmpty()};
    const bool funded{
        valid && std::any_of(m_session.contributions().begin(), m_session.contributions().end(),
                             [&](const auto& contribution) { return contribution.label == shares[selected].label; })};
    const bool editable{valid && !funded && (mine || owner_unclaimed)};
    for (QLineEdit* edit : {m_my_owner_edit, m_my_refund_edit, m_my_reward_edit}) {
        edit->setEnabled(editable);
    }
    for (QPushButton* button : {m_my_address_button, m_my_refund_button, m_my_reward_button}) {
        button->setEnabled(editable && m_wallet_model != nullptr);
    }
    m_my_share_hint->setText(
        !valid   ? tr("Choose your participant before adding addresses or funding.")
        : funded ? tr("%1's details are locked to its funding contribution. Remove that contribution to edit them.")
                       .arg(ShareDisplayLabel(m_session, selected))
        : !editable
            ? tr("%1's details were supplied by another wallet. Choose the row assigned to you.")
                  .arg(ShareDisplayLabel(m_session, selected))
            : tr("You are completing %1's %2 share. Only these personal addresses and this wallet's funding "
                 "are yours to provide.")
                  .arg(ShareDisplayLabel(m_session, selected), FormatAmount(m_wallet_model, shares[selected].amount)));
}

void SharedMnCreateDialog::refreshValidation()
{
    QStringList errors{m_session.validateShares()};
    CBLSPublicKey operator_key;
    const bool operator_valid{
        operator_key.SetHexStr(m_session.terms().operatorPubKey.toStdString(), /*specificLegacyScheme=*/false)};
    if (!operator_valid) {
        errors << tr("The operator public key must be a valid basic-scheme BLS public key.");
    }
    QString service_error;
    parseServiceList(m_session.terms().coreP2PAddrs, service_error);
    if (!service_error.isEmpty()) errors << service_error;

    QSet<QString> contributed_labels;
    for (const auto& contribution : m_session.contributions()) {
        contributed_labels.insert(contribution.label);
    }
    QSet<QString> participant_labels;
    for (int i = 0; i < static_cast<int>(m_session.shares().size()); ++i) {
        const QString label{m_session.shares()[i].label.trimmed()};
        if (label.isEmpty()) {
            errors << tr("Participant %1 needs a name.").arg(i + 1);
            continue;
        }
        if (participant_labels.contains(label)) {
            errors << tr("Participant name \"%1\" is used more than once.").arg(label);
        }
        participant_labels.insert(label);
        if (!contributed_labels.contains(label)) {
            errors << tr("%1 has not added a funding contribution yet.").arg(label);
        }
    }
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

    const bool coordinator{m_role == Role::Coordinator};
    const int selected{myShareIndex()};
    bool my_details_complete{false};
    bool my_contribution_complete{false};
    if (selected >= 0 && selected < static_cast<int>(m_session.shares().size())) {
        const auto& share{m_session.shares()[selected]};
        const CTxDestination owner{DecodeDestination(share.ownerAddress.toStdString())};
        const CTxDestination refund{DecodeDestination(share.refundAddress.toStdString())};
        const CTxDestination reward{DecodeDestination(share.rewardAddress.toStdString())};
        const bool refund_valid{std::holds_alternative<PKHash>(refund) || std::holds_alternative<ScriptHash>(refund)};
        const bool reward_valid{share.rewardAddress.isEmpty() || std::holds_alternative<PKHash>(reward) ||
                                std::holds_alternative<ScriptHash>(reward)};
        my_details_complete = std::holds_alternative<PKHash>(owner) && myShareTotal() > 0 && refund_valid && reward_valid;
        my_contribution_complete = contributed_labels.contains(share.label);
    }
    QSet<QString> roster_labels;
    CAmount roster_total{0};
    bool roster_ready{m_session.shares().size() >= CProRegTx::MIN_SHARES &&
                      m_session.shares().size() <= CProRegTx::MAX_SHARES};
    for (const auto& share : m_session.shares()) {
        const QString label{share.label.trimmed()};
        roster_ready &= !label.isEmpty() && !roster_labels.contains(label) && share.amount >= CCollateralShare::MIN_AMOUNT;
        roster_labels.insert(label);
        roster_total += share.amount;
    }
    roster_ready &= roster_total == required;
    const bool voting_valid{
        std::holds_alternative<PKHash>(DecodeDestination(m_session.terms().votingAddress.toStdString()))};
    const bool can_pass{my_details_complete && my_contribution_complete && roster_ready && operator_valid &&
                        voting_valid && service_error.isEmpty()};
    const auto next_incomplete = std::find_if(m_session.shares().begin(), m_session.shares().end(),
                                              [&](const auto& share) {
                                                  return share.ownerAddress.isEmpty() || share.refundAddress.isEmpty() ||
                                                         !contributed_labels.contains(share.label);
                                              });
    QString next_participant;
    if (next_incomplete != m_session.shares().end()) {
        next_participant = ShareDisplayLabel(
            m_session, static_cast<int>(std::distance(m_session.shares().begin(), next_incomplete)));
        next_participant.replace(QLatin1Char('&'), QStringLiteral("&&"));
    }

    const bool can_freeze{coordinator && m_v24_active && errors.isEmpty() && !m_busy};
    m_freeze_button->setProperty("canFreeze", can_freeze);
    if (can_freeze) {
        m_freeze_button->setText(tr("Lock Terms and Start Signing"));
    } else if (!next_participant.isEmpty()) {
        m_freeze_button->setText(tr("Copy Draft for %1").arg(next_participant));
    } else {
        m_freeze_button->setText(tr("Return Completed Draft to Coordinator"));
    }
    m_freeze_button->setEnabled(!m_busy && (can_freeze || can_pass));
    if (!coordinator) {
        m_freeze_button->setToolTip(
            can_pass ? tr("Pass your addresses and funding to the next participant, or back to the coordinator if "
                          "you are last.")
                     : tr("Complete My share addresses and funding before passing the draft on."));
    } else if (!m_v24_active) {
        m_freeze_button->setToolTip(tr("Shared masternodes require the v24 hard fork to be active."));
    } else if (!can_freeze) {
        m_freeze_button->setToolTip(can_pass
                                        ? tr("The draft is not complete yet. Pass it to the next participant shown "
                                             "in Readiness.")
                                        : tr("Complete the participant roster, shared settings, My share addresses "
                                             "and My contribution before passing the draft on."));
    } else {
        m_freeze_button->setToolTip(tr("Lock the registration terms. Every share owner then reviews the complete "
                                       "locked term sheet and approves their share once."));
    }
}

void SharedMnCreateDialog::refreshContributions()
{
    m_fee_box->setVisible(m_role == Role::Coordinator);
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
    if (m_session.contributions().empty()) {
        new QListWidgetItem(m_role == Role::Coordinator
                                ? tr("No contributions yet. Prepare yours, then paste each participant's reply.")
                                : tr("No contribution from this wallet yet. Prepare it before returning the update."),
                            m_contrib_list);
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

    const CAmount mine{myShareTotal()};
    m_add_funding_button->setText(mine > 0 ? tr("Prepare My %1 Contribution").arg(FormatAmount(m_wallet_model, mine))
                                           : tr("Prepare My Contribution"));
}

void SharedMnCreateDialog::refreshReviewPage()
{
    CAmount share_total{0};
    for (const auto& share : m_session.shares())
        share_total += share.amount;

    const auto& terms{m_session.terms()};
    QStringList rows;
    rows << QStringLiteral("<b>%1</b> %2 · %3")
                .arg(tr("Participants:"), tr("%n share(s)", nullptr, static_cast<int>(m_session.shares().size())),
                     FormatAmount(m_wallet_model, share_total));
    rows << QStringLiteral("<b>%1</b> %2")
                .arg(tr("Operator:"),
                     terms.operatorPubKey.isEmpty() ? tr("not set") : terms.operatorPubKey.toHtmlEscaped());
    rows << QStringLiteral("<b>%1</b> %2")
                .arg(tr("Voting:"), terms.votingAddress.isEmpty() ? tr("not set") : terms.votingAddress.toHtmlEscaped());
    rows << QStringLiteral("<b>%1</b> %2")
                .arg(tr("Service:"),
                     terms.coreP2PAddrs.isEmpty() ? tr("will be set later") : terms.coreP2PAddrs.toHtmlEscaped());
    rows << QStringLiteral("<b>%1</b> %2")
                .arg(tr("Funding:"),
                     tr("%n contribution(s) recorded", nullptr, static_cast<int>(m_session.contributions().size())));
    m_review_summary->setText(rows.join(QStringLiteral("<br>")));
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
    const bool has_pending_approval{std::any_of(my_indexes.begin(), my_indexes.end(),
                                                [this](int index) { return m_session.signatureFor(index).isEmpty(); })};
    m_sig_table->setRowCount(static_cast<int>(shares.size()));
    for (int i = 0; i < static_cast<int>(shares.size()); ++i) {
        const QString label{ShareDisplayLabel(m_session, i)};
        QString status;
        const QString sig{m_session.signatureFor(i)};
        if (sig.isEmpty()) {
            status = tr("Pending — ask %1 to sign").arg(label);
        } else if (QString sig_error; m_session.verifySignature(i, sig, sig_error)) {
            status = tr("Approved");
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
    m_sign_button->setEnabled(signing_stage && canSign() && has_pending_approval && !m_busy);
    if (!canSign()) {
        m_sign_button->setToolTip(m_wallet_model == nullptr ? tr("No wallet is available.") :
                                                              tr("This wallet is watch-only and cannot sign."));
    } else if (my_indexes.empty()) {
        m_sign_button->setToolTip(tr("This wallet holds none of the share owner keys."));
    } else if (!has_pending_approval) {
        m_sign_button->setToolTip(tr("This wallet has already approved all of its shares."));
    } else {
        m_sign_button->setToolTip(tr("Approve the locked terms with every share owner key in this wallet."));
    }
    m_unfreeze_button->setEnabled(signing_stage && !m_busy);
    m_unfreeze_button->setVisible(m_role == Role::Coordinator);
}

void SharedMnCreateDialog::refreshCombinedPage()
{
    const int total{static_cast<int>(m_session.shares().size())};
    const bool combined{m_session.stage() == MnShareSession::Stage::Combined};
    const bool ready_to_combine{!combined && total > 0 && m_session.signedCount() == total};

    m_combine_button->setVisible(!combined && m_role == Role::Coordinator);
    m_combine_button->setText(tr("Combine Approvals"));
    m_combine_button->setEnabled(ready_to_combine && m_v24_active && !m_busy && m_role == Role::Coordinator);
    if (!m_v24_active) m_combine_button->setToolTip(tr("Shared masternodes require the v24 hard fork to be active."));
    m_combined_status->setText(combined
                                   ? (m_role == Role::Coordinator
                                          ? tr("All %n owner approval(s) are embedded. Send this same update to every "
                                               "contributor for one parallel contribution-signing round.",
                                               nullptr, total)
                                          : tr("All %n owner approval(s) are embedded. Sign your contribution once and "
                                               "return this copy to the coordinator.",
                                               nullptr, total))
                                   : (m_role == Role::Coordinator
                                          ? tr("Every share owner has approved the locked terms. Combine the approvals "
                                               "to continue.")
                                          : tr("Every share owner has approved the locked terms. Copy this update for "
                                               "the coordinator to combine.")));

    m_funding_sig_list->clear();
    const auto signed_map{InputSignatureMap(m_session.protxHex())};
    const QSet<QString> wallet_outpoints{WalletOutpoints(m_wallet_model)};
    int outstanding{0};
    bool mine_pending{false};
    for (const auto& contribution : m_session.contributions()) {
        int done{0};
        for (const auto& input : contribution.inputs) {
            const QString key{OutpointKey(input.txid, input.vout)};
            const auto it{signed_map.find(key)};
            if (it != signed_map.end() && it->second) ++done;
            if ((it == signed_map.end() || !it->second) && wallet_outpoints.contains(key)) mine_pending = true;
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

    m_sign_funding_button->setEnabled(combined && canSign() && mine_pending && !m_busy);
    m_sign_funding_button->setToolTip(!combined      ? tr("Combine the owner approvals first.")
                                      : !canSign()   ? tr("No wallet able to sign is available.")
                                      : mine_pending ? tr("Sign this wallet's contribution once.")
                                                     : tr("This wallet has no unsigned contribution in this update."));
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
    rows << tr("Transaction id: %1").arg(txid.toHtmlEscaped());
    rows << tr("Size: %n byte(s), collateral output index %1", nullptr,
               static_cast<int>(m_session.protxHex().size() / 2))
                .arg(m_session.collateralIndex());
    m_broadcast_summary->setText(rows.join(QStringLiteral("<br>")));

    m_broadcast_button->setVisible(!broadcast && m_role == Role::Coordinator);
    m_broadcast_button->setEnabled(!broadcast && m_v24_active && !m_busy && m_role == Role::Coordinator);
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
    m_operator_widget->setVisible(!imported_key);
    if (m_operator_field_label != nullptr) m_operator_field_label->setVisible(!imported_key);
    m_operator_widget->setEnabled(!imported_key);
    m_operator_session_key_label->setVisible(imported_key);
    if (imported_key) {
        m_operator_widget->setToolTip(tr("The imported session already carries the group's agreed operator key; "
                                         "it is kept as-is."));
        m_operator_session_key_label->setText(tr("Operator key from the imported session (used as-is): %1")
                                                  .arg(MasternodeWidgetUtil::chunked(terms.operatorPubKey)));
        if (m_operator_widget->publicKeyHex() != terms.operatorPubKey) {
            m_operator_secret_label->setVisible(false); // the widget's local secret is not the session key
        }
    } else {
        m_operator_widget->setToolTip(QString());
    }
    m_updating = false;
}

void SharedMnCreateDialog::createSession()
{
    m_role = Role::Coordinator;
    if (m_session.shares().empty()) {
        addShareRow();
        addShareRow();
    }
    showParticipantsPage();
}

void SharedMnCreateDialog::resumeCoordinatorSession()
{
    m_role = Role::Coordinator;
    loadFromFileImpl(/*accept_newer=*/true);
    if (m_session.shares().empty() && !m_dirty) m_role = Role::Undecided;
    refreshAll();
}

void SharedMnCreateDialog::resumeCoordinatorFromClipboard()
{
    m_role = Role::Coordinator;
    adoptImportedText(QApplication::clipboard()->text(), tr("clipboard"), /*accept_newer=*/true);
    if (m_session.shares().empty() && !m_dirty) m_role = Role::Undecided;
    refreshAll();
}

void SharedMnCreateDialog::joinFromClipboard()
{
    m_role = Role::Participant;
    adoptImportedText(QApplication::clipboard()->text(), tr("clipboard"),
                      /*accept_newer=*/!m_session.shares().empty());
    if (m_session.shares().empty() && !m_dirty) m_role = Role::Undecided;
    refreshAll();
}

void SharedMnCreateDialog::joinFromFile()
{
    m_role = Role::Participant;
    loadFromFileImpl(/*accept_newer=*/!m_session.shares().empty());
    if (m_session.shares().empty() && !m_dirty) m_role = Role::Undecided;
    refreshAll();
}

void SharedMnCreateDialog::showParticipantsPage()
{
    refreshDraftPage();
    m_pages->setCurrentIndex(PageParticipants);
    refreshHeader();
}

void SharedMnCreateDialog::showSettingsPage()
{
    syncTermsToSession();
    refreshDraftPage();
    m_pages->setCurrentIndex(PageSettings);
    refreshHeader();
}

void SharedMnCreateDialog::showFundingPage()
{
    syncTermsToSession();
    refreshDraftPage();
    m_pages->setCurrentIndex(PageFunding);
    refreshHeader();
}

void SharedMnCreateDialog::showReviewPage()
{
    syncTermsToSession();
    refreshDraftPage();
    m_pages->setCurrentIndex(PageReview);
    refreshHeader();
}

void SharedMnCreateDialog::importFromClipboard()
{
    adoptImportedText(QApplication::clipboard()->text(), tr("clipboard"));
}

void SharedMnCreateDialog::loadFromFile()
{
    loadFromFileImpl(/*accept_newer=*/false);
}

void SharedMnCreateDialog::loadFromFileImpl(bool accept_newer)
{
    const QString filename{GUIUtil::getOpenFileName(this, tr("Load Shared Masternode Session"), QString(),
                                                    tr("Session files (*.json)"), nullptr)};
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, windowTitle(), tr("Could not open %1 for reading.").arg(filename));
        return;
    }
    adoptImportedText(QString::fromUtf8(file.readAll()), filename, accept_newer);
}

void SharedMnCreateDialog::exportToClipboard()
{
    GUIUtil::setClipboard(m_session.toJsonString());
    QString message;
    if (m_session.stage() == MnShareSession::Stage::Draft) {
        message = m_role == Role::Coordinator
                      ? tr("The draft was copied for the next participant. This window will return to the session "
                           "start; use Paste Returned Draft when the completed draft comes back.")
                      : tr("Your updated draft was copied. Send it to the next participant, or back to the "
                           "coordinator if every participant has completed their share. This window will return "
                           "to the session start while you wait for the next coordinator update.");
    } else if (m_session.stage() == MnShareSession::Stage::Frozen ||
               m_session.stage() == MnShareSession::Stage::Signing) {
        message = m_role == Role::Coordinator
                      ? tr("The approval request was copied. Send the same request to every participant.")
                      : tr("Your approval reply was copied for the coordinator.");
    } else if (m_session.stage() == MnShareSession::Stage::Combined) {
        message = m_role == Role::Coordinator
                      ? tr("The contribution-signing request was copied. Every participant signs the same request in "
                           "parallel.")
                      : tr("Your signed contribution was copied for the coordinator.");
    } else {
        message = tr("The final session was copied.");
    }
    QMessageBox::information(this, windowTitle(), message);
    const bool wait_for_next_phase{m_role == Role::Participant &&
                                   m_session.stage() != MnShareSession::Stage::FundingSigned &&
                                   m_session.stage() != MnShareSession::Stage::Broadcast};
    if ((m_session.stage() == MnShareSession::Stage::Draft && m_role == Role::Coordinator) || wait_for_next_phase) {
        m_pages->setCurrentIndex(PageLanding);
        refreshHeader();
    }
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

void SharedMnCreateDialog::adoptImportedText(const QString& text, const QString& source, bool accept_newer)
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
        if (accept_newer) {
            unlockAllContributedCoins(); // the replaced session's coin locks would otherwise leak
            m_session = imported;
            markDirty();
            afterImport();
            break;
        }
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
    if (m_role == Role::Undecided) m_role = Role::Participant;
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
    const QString recipient{m_role == Role::Coordinator ? tr("participants") : tr("coordinator")};
    const auto choice{QMessageBox::question(this, windowTitle(),
                                            why + QStringLiteral("\n\n") +
                                                tr("Copy the updated session for the %1 now?").arg(recipient),
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)};
    if (choice != QMessageBox::Yes) return;
    GUIUtil::setClipboard(m_session.toJsonString());
    if (m_role == Role::Participant && m_session.stage() != MnShareSession::Stage::FundingSigned &&
        m_session.stage() != MnShareSession::Stage::Broadcast) {
        m_pages->setCurrentIndex(PageLanding);
        refreshHeader();
    }
}

void SharedMnCreateDialog::addShareRow()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    auto& shares{m_session.shares()};
    if (shares.size() >= CProRegTx::MAX_SHARES) return;
    MnShareSession::Share share;
    share.label = tr("Participant %1").arg(shares.size() + 1);
    shares.push_back(share);
    m_session.noteDraftChange();
    markDirty();
    refreshSharesTable();
    refreshMySharePanel();
    refreshValidation();
    refreshHeader();
}

void SharedMnCreateDialog::removeShareRow()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    const int row{m_share_table->currentRow()};
    auto& shares{m_session.shares()};
    if (row < 0 || row >= static_cast<int>(shares.size())) return;
    const QString label{shares[row].label};
    if (std::any_of(m_session.contributions().begin(), m_session.contributions().end(),
                    [&label](const auto& contribution) { return contribution.label == label; })) {
        QMessageBox::information(this, windowTitle(),
                                 tr("Remove %1's funding contribution before removing the participant.").arg(label));
        return;
    }
    shares.erase(shares.begin() + row);
    m_session.noteDraftChange();
    markDirty();
    refreshSharesTable();
    refreshMySharePanel();
    refreshValidation();
    refreshHeader();
}

void SharedMnCreateDialog::useMyOwnerAddress() { fillMyAddress(m_my_owner_edit); }

void SharedMnCreateDialog::useMyRefundAddress() { fillMyAddress(m_my_refund_edit); }

void SharedMnCreateDialog::useMyRewardAddress() { fillMyAddress(m_my_reward_edit); }

void SharedMnCreateDialog::useMyVotingAddress()
{
    QString error;
    const QString address{freshAddress(error)};
    if (address.isEmpty()) {
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    m_voting_edit->setText(address);
}

void SharedMnCreateDialog::fillMyAddress(QLineEdit* edit)
{
    if (edit == nullptr || myShareIndex() < 0) return;
    QString error;
    const QString address{freshAddress(error)};
    if (address.isEmpty()) {
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    edit->setText(address);
    onMyShareDetailsChanged();
}

int SharedMnCreateDialog::myShareIndex() const
{
    if (m_my_share_combo == nullptr) return -1;
    bool ok{false};
    const int index{m_my_share_combo->currentData().toInt(&ok)};
    return ok ? index : -1;
}

void SharedMnCreateDialog::onMyShareChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    const int share_index{myShareIndex()};
    if (share_index >= 0 && share_index < static_cast<int>(m_session.shares().size())) {
        m_share_table->selectRow(share_index);
    }
    refreshMySharePanel();
    refreshContributions();
    refreshHeader();
}

void SharedMnCreateDialog::onMyShareDetailsChanged()
{
    if (m_updating) return;
    const int index{myShareIndex()};
    auto& shares{m_session.shares()};
    if (index < 0 || index >= static_cast<int>(shares.size())) return;
    shares[index].ownerAddress = m_my_owner_edit->text().trimmed();
    shares[index].refundAddress = m_my_refund_edit->text().trimmed();
    shares[index].rewardAddress = m_my_reward_edit->text().trimmed();
    m_session.noteDraftChange();
    markDirty();
    refreshSharesTable();
    refreshValidation();
    refreshContributions();
    refreshHeader();
}

void SharedMnCreateDialog::onShareCellChanged(int row, int column)
{
    Q_UNUSED(column);
    if (m_updating) return;
    syncShareFromTable(row);
    m_session.noteDraftChange();
    markDirty();
    refreshMySharePanel();
    refreshValidation();
    refreshHeader();
}

void SharedMnCreateDialog::onTermsChanged()
{
    if (m_updating) return;
    syncTermsToSession();
    m_session.noteDraftChange();
    markDirty();
    refreshValidation();
    refreshHeader();
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
    const int index{myShareIndex()};
    if (index < 0 || index >= static_cast<int>(m_session.shares().size())) return 0;
    const auto owned{myShareIndexes()};
    return std::find(owned.begin(), owned.end(), index) != owned.end() ? m_session.shares()[index].amount : 0;
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
            if (txout.is_spent || txout.depth_in_main_chain < 0) continue;
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
    m_fee_utxo_combo->setCurrentIndex(-1);
    autoSelectFeeCandidate();
    refreshContributions();
}

void SharedMnCreateDialog::addMyFunding()
{
    if (m_wallet_model == nullptr || m_session.stage() != MnShareSession::Stage::Draft || m_busy) return;

    const int share_index{myShareIndex()};
    if (share_index < 0 || share_index >= static_cast<int>(m_session.shares().size())) {
        QMessageBox::information(this, windowTitle(), tr("Choose your participant under My share first."));
        return;
    }
    const auto owned{myShareIndexes()};
    if (std::find(owned.begin(), owned.end(), share_index) == owned.end()) {
        QMessageBox::information(this, windowTitle(),
                                 tr("The owner address for your selected share is not controlled by this wallet."));
        return;
    }
    const QString label{ShareDisplayLabel(m_session, share_index)};
    const CAmount amount{myShareTotal()};
    if (amount <= 0) {
        QMessageBox::information(this, windowTitle(),
                                 tr("The owner address for your selected share is not controlled by this wallet. Use "
                                    "a wallet address for My owner address before preparing the contribution."));
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
        // That send consumed wallet outputs, so any fee candidate chosen before it may
        // now be spent - including the very coin the send drew on.
        refreshFundingCandidates();
    }
    contribution.inputs.push_back(*chip);

    if (m_role == Role::Coordinator) {
        // The share output cannot also pay the fee, and after a just-prepared chip it is
        // the cheapest candidate, so the default selection would land on it.
        autoSelectFeeCandidate(/*excluded_txid=*/chip->txid, /*excluded_vout=*/chip->vout);
        const int fee_index{m_fee_utxo_combo->currentIndex()};
        const CAmount fee{m_fee_amount_field->value()};
        if (fee <= 0) {
            QMessageBox::information(this, windowTitle(), tr("Enter a positive registration fee."));
            return;
        }
        if (fee_index < 0) {
            QMessageBox::information(this, windowTitle(),
                                     tr("No unlocked wallet output is large enough to pay the %1 registration fee.")
                                         .arg(FormatAmount(m_wallet_model, fee)));
            return;
        }
        MnShareSession::Input fee_input;
        fee_input.txid = m_fee_utxo_combo->itemData(fee_index, FEE_UTXO_TXID_ROLE).toString();
        fee_input.vout = m_fee_utxo_combo->itemData(fee_index, FEE_UTXO_VOUT_ROLE).toUInt();
        const CAmount fee_value{m_fee_utxo_combo->itemData(fee_index, FEE_UTXO_VALUE_ROLE).toLongLong()};
        if (!isSpendableFundingInput(fee_input)) {
            QMessageBox::information(this, windowTitle(),
                                     tr("The selected fee input is no longer available to spend. Choose another "
                                        "output."));
            return;
        }
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
    refreshSharesTable();
    refreshMySharePanel();
    refreshContributions();
    refreshValidation();
    refreshHeader();
}

void SharedMnCreateDialog::removeMyContribution()
{
    if (m_session.stage() != MnShareSession::Stage::Draft) return;
    const int share_index{myShareIndex()};
    const auto& contributions{m_session.contributions()};
    if (share_index < 0 || share_index >= static_cast<int>(m_session.shares().size())) {
        QMessageBox::information(this, windowTitle(), tr("Choose your participant under My share first."));
        return;
    }
    const auto owned{myShareIndexes()};
    if (std::find(owned.begin(), owned.end(), share_index) == owned.end()) {
        QMessageBox::information(this, windowTitle(), tr("The selected contribution belongs to another wallet."));
        return;
    }
    const QString label{ShareDisplayLabel(m_session, share_index)};
    const auto contribution{std::find_if(contributions.begin(), contributions.end(),
                                         [&label](const auto& candidate) { return candidate.label == label; })};
    if (contribution == contributions.end()) {
        QMessageBox::information(this, windowTitle(), tr("%1 has no funding contribution to remove.").arg(label));
        return;
    }
    const MnShareSession::Contribution removed{*contribution};
    QString error;
    if (!m_session.removeContribution(removed.label, error)) {
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }
    setContributionLocked(removed, /*lock=*/false);
    markDirty();
    refreshSharesTable();
    refreshMySharePanel();
    refreshContributions();
    refreshValidation();
    refreshHeader();
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

void SharedMnCreateDialog::autoSelectFeeCandidate(const QString& excluded_txid, quint32 excluded_vout)
{
    const auto is_excluded = [this, &excluded_txid, excluded_vout](int index) {
        return m_fee_utxo_combo->itemData(index, FEE_UTXO_TXID_ROLE).toString() == excluded_txid &&
               m_fee_utxo_combo->itemData(index, FEE_UTXO_VOUT_ROLE).toUInt() == excluded_vout;
    };
    const int current{m_fee_utxo_combo->currentIndex()};
    if (current >= 0 && !is_excluded(current)) return;

    // Candidates are ordered by value, so this takes the smallest coin that still
    // covers the fee without spending the excluded share output itself.
    const CAmount fee{m_fee_amount_field->value()};
    for (int i = 0; i < m_fee_utxo_combo->count(); ++i) {
        if (is_excluded(i)) continue;
        if (m_fee_utxo_combo->itemData(i, FEE_UTXO_VALUE_ROLE).toLongLong() < fee) continue;
        m_fee_utxo_combo->setCurrentIndex(i);
        return;
    }
}

bool SharedMnCreateDialog::isSpendableFundingInput(const MnShareSession::Input& input) const
{
    if (m_wallet_model == nullptr) return false;
    const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
    const auto known{m_wallet_model->wallet().getCoins({outpoint})};
    if (known.empty() || known.front().is_spent || known.front().depth_in_main_chain < 0) return false;
    return !m_wallet_model->wallet().isLockedCoin(outpoint);
}

std::optional<CAmount> SharedMnCreateDialog::resolveInputValue(const MnShareSession::Input& input) const
{
    const COutPoint outpoint{uint256S(input.txid.toStdString()), input.vout};
    // The chainstate UTXO set still reports a coin as unspent while the transaction
    // spending it sits in the mempool, so this wallet's own view decides for the coins
    // it tracks. Other participants' coins are only visible in the UTXO set.
    if (m_wallet_model != nullptr) {
        const auto known{m_wallet_model->wallet().getCoins({outpoint})};
        if (!known.empty() && known.front().depth_in_main_chain >= 0) {
            if (known.front().is_spent) return std::nullopt;
            return known.front().txout.nValue;
        }
    }
    Coin coin;
    if (m_node.getUnspentOutput(outpoint, coin)) return coin.out.nValue;
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

void SharedMnCreateDialog::copyOrFreezeDraft()
{
    if (m_role == Role::Coordinator && m_freeze_button->property("canFreeze").toBool()) {
        freezeSession();
        return;
    }
    exportToClipboard();
}

void SharedMnCreateDialog::freezeSession()
{
    if (m_session.stage() != MnShareSession::Stage::Draft || m_busy) return;
    syncTermsToSession();
    QSet<QString> contributed_labels;
    for (const auto& contribution : m_session.contributions()) {
        contributed_labels.insert(contribution.label);
    }
    const bool every_share_contributed{
        std::all_of(m_session.shares().begin(), m_session.shares().end(),
                    [&](const auto& share) { return contributed_labels.contains(share.label); })};
    if (!m_session.validateShares().isEmpty() || m_session.fundingTxHex().isEmpty() ||
        m_session.contributions().size() != m_session.shares().size() || !every_share_contributed) {
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
        this, tr("Lock the session terms?"),
        tr("Locking prepares the registration transaction and fixes the share table, the terms and every funding "
           "input. Afterwards every share owner reviews and approves the complete locked terms; editing again "
           "discards all collected approvals.") +
            QStringLiteral("\n\n") + funding_note + QStringLiteral("\n\n") +
            tr("Make sure every participant has contributed funding — changing an input requires unlocking the "
               "terms and collecting approvals again."),
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
    offerExport(tr("The terms are locked. Send the same approval request to every participant. Each person reviews "
                   "the complete locked term sheet and approves their share once."));
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
        const QString message{tr("%n approval(s) were recorded (%1 of %2 shares approved).", nullptr, added)
                                  .arg(m_session.signedCount())
                                  .arg(m_session.shares().size())};
        if (m_role == Role::Coordinator) {
            QMessageBox::information(
                this, windowTitle(),
                message + QStringLiteral("\n\n") +
                    tr("Keep this session open and send the same approval request to every participant."));
        } else {
            offerExport(message);
        }
    }
}

void SharedMnCreateDialog::unfreezeSession()
{
    if (m_session.stage() != MnShareSession::Stage::Frozen && m_session.stage() != MnShareSession::Stage::Signing) {
        return;
    }
    const int sigs{m_session.signedCount()};
    const auto choice{QMessageBox::question(this, tr("Unlock the session terms?"),
                                            tr("Unlocking returns the session to editing and discards %n collected "
                                               "approval(s). Everyone will have to "
                                               "approve again after the terms are locked.",
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
    offerExport(tr("The approvals are combined. Send this same request to every contributor for the one parallel "
                   "contribution-signing round."));
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
        if (m_role == Role::Coordinator) {
            QMessageBox::information(this, windowTitle(),
                                     tr("All contributions are signed. The registration is ready to broadcast."));
        } else {
            offerExport(tr("All contributions are signed. Return the fully signed update to the coordinator."));
        }
        return;
    }

    if (signed_hex == before) {
        QMessageBox::information(this, windowTitle(),
                                 tr("This wallet does not control any unsigned contribution inputs in this update."));
        return;
    }
    if (!replaceSessionProTx(signed_hex, error)) {
        QMessageBox::critical(this, tr("Signing failed"), error);
        return;
    }
    markDirty();
    refreshAll();
    if (m_role == Role::Coordinator) {
        QMessageBox::information(this, windowTitle(),
                                 tr("Your contribution is signed. Paste each participant's signed contribution as it "
                                    "returns; all participants can sign the same request in parallel."));
    } else {
        offerExport(tr("This wallet's contribution is signed. Return this signed update to the coordinator; other "
                       "contributors sign their own copies in parallel."));
    }
}

bool SharedMnCreateDialog::replaceSessionProTx(const QString& tx_hex, QString& error)
{
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
    m_role = Role::Undecided;
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
