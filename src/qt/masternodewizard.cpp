// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodewizard.h>

#include <chainparams.h>
#include <evo/dmn_types.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <util/strencodings.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/optionsmodel.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <univalue.h>

#include <variant>

namespace {
//! Headroom on top of the collateral the funding address must hold for fees
constexpr CAmount FEE_MARGIN{100000};
//! Combo item role holding the collateral output index
constexpr int VOUT_ROLE{Qt::UserRole + 1};

QLabel* MakeTitle(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    GUIUtil::setFont({label}, GUIUtil::FontWeight::Bold, 14);
    return label;
}

QLabel* MakeHint(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    return label;
}

QString FormatAmount(const WalletModel* wallet_model, CAmount amount)
{
    const auto unit{wallet_model && wallet_model->getOptionsModel() ?
                        wallet_model->getOptionsModel()->getDisplayUnit() :
                        BitcoinUnits::Unit::DASH};
    return BitcoinUnits::formatWithUnit(unit, amount, /*plussign=*/false, BitcoinUnits::SeparatorStyle::ALWAYS);
}
} // anonymous namespace

//! Keeps the wallet unlocked on the GUI thread while a ProTxSender call is in
//! flight. UnlockContext is neither copyable nor movable, so it is constructed
//! in place from requestUnlock()'s prvalue.
struct RegisterMasternodeWizard::UnlockHolder
{
    WalletModel::UnlockContext ctx;
    explicit UnlockHolder(WalletModel& wallet_model) :
        ctx(wallet_model.requestUnlock()) {}
};

RegisterMasternodeWizard::RegisterMasternodeWizard(interfaces::Node& node, WalletModel* walletModel, QWidget* parent) :
    QDialog(parent),
    m_node(node),
    m_walletModel(walletModel),
    m_sender(node, this)
{
    setWindowTitle(tr("Register Masternode"));

    m_pages = new QStackedWidget(this);
    // Insertion order must match the Page enum: the enum value doubles as the
    // stack index.
    m_pages->insertWidget(PageType, createTypePage());
    m_pages->insertWidget(PageCollateral, createCollateralPage());
    m_pages->insertWidget(PageService, createServicePage());
    m_pages->insertWidget(PageKeys, createKeysPage());
    m_pages->insertWidget(PagePayout, createPayoutPage());
    m_pages->insertWidget(PagePlatform, createPlatformPage());
    m_pages->insertWidget(PageFee, createFeePage());
    m_pages->insertWidget(PageReview, createReviewPage());
    m_pages->insertWidget(PageSign, createSignPage());
    m_pages->insertWidget(PageResult, createResultPage());

    m_error_label = new QLabel(this);
    m_error_label->setWordWrap(true);
    m_error_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
    m_error_label->setVisible(false);

    m_back_button = new QPushButton(tr("Back"), this);
    m_next_button = new QPushButton(tr("Next"), this);
    m_next_button->setDefault(true);
    m_cancel_button = new QPushButton(tr("Cancel"), this);

    auto* buttons{new QHBoxLayout()};
    buttons->addWidget(m_back_button);
    buttons->addStretch();
    buttons->addWidget(m_cancel_button);
    buttons->addWidget(m_next_button);

    auto* layout{new QVBoxLayout(this)};
    layout->addWidget(m_pages, /*stretch=*/1);
    layout->addWidget(m_error_label);
    layout->addLayout(buttons);

    connect(m_back_button, &QPushButton::clicked, this, &RegisterMasternodeWizard::onBack);
    connect(m_next_button, &QPushButton::clicked, this, &RegisterMasternodeWizard::onNext);
    connect(m_cancel_button, &QPushButton::clicked, this, &RegisterMasternodeWizard::reject);
    connect(&m_sender, &ProTxSender::finished, this, &RegisterMasternodeWizard::onRpcFinished);

    rebuildOrder();
    enterPage(PageType);

    GUIUtil::disableMacFocusRect(this);
    GUIUtil::updateFonts();
    setMinimumSize(700, 560);
}

RegisterMasternodeWizard::~RegisterMasternodeWizard() = default;

QWidget* RegisterMasternodeWizard::createTypePage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Masternode type"), page));

    m_type_regular = new QRadioButton(
        tr("Masternode — %1 collateral").arg(FormatAmount(m_walletModel, GetMnType(MnType::Regular).collat_amount)),
        page);
    m_type_regular->setChecked(true);
    layout->addWidget(m_type_regular);
    layout->addWidget(MakeHint(tr("Provides Core network services and earns regular masternode rewards."), page));

    m_type_evo = new QRadioButton(
        tr("Evonode — %1 collateral").arg(FormatAmount(m_walletModel, GetMnType(MnType::Evo).collat_amount)), page);
    layout->addWidget(m_type_evo);
    layout->addWidget(MakeHint(tr("Additionally hosts Dash Platform, has four times the voting weight and earns a "
                                  "larger share of rewards. Requires a Platform node ID and extra services."),
                               page));
    layout->addStretch();

    connect(m_type_regular, &QRadioButton::toggled, this, [this] {
        rebuildOrder();
        refreshCollateralCandidates();
    });
    return page;
}

QWidget* RegisterMasternodeWizard::createCollateralPage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Collateral"), page));

    m_col_fund = new QRadioButton(tr("Send collateral from this wallet to a new address"), page);
    m_col_fund->setChecked(true);
    layout->addWidget(m_col_fund);
    m_col_fund_box = new QWidget(page);
    {
        auto* box{new QVBoxLayout(m_col_fund_box)};
        box->setContentsMargins(24, 0, 0, 0);
        box->addWidget(MakeHint(tr("A single transaction funds the collateral and registers the masternode."),
                                m_col_fund_box));
        auto* row{new QHBoxLayout()};
        m_col_address = new QValidatedLineEdit(m_col_fund_box);
        GUIUtil::setupAddressWidget(m_col_address, this);
        row->addWidget(m_col_address, /*stretch=*/1);
        auto* fresh{new QPushButton(tr("Use new address"), m_col_fund_box)};
        connect(fresh, &QPushButton::clicked, this, [this] {
            QString err;
            const QString addr{freshAddress(err)};
            if (addr.isEmpty()) {
                showError(err);
            } else {
                m_col_address->setText(addr);
            }
        });
        row->addWidget(fresh);
        box->addLayout(row);
    }
    layout->addWidget(m_col_fund_box);

    m_col_wallet = new QRadioButton(tr("Use an existing collateral output of this wallet"), page);
    layout->addWidget(m_col_wallet);
    m_col_wallet_box = new QWidget(page);
    {
        auto* box{new QVBoxLayout(m_col_wallet_box)};
        box->setContentsMargins(24, 0, 0, 0);
        box->addWidget(MakeHint(tr("Unspent P2PKH outputs of exactly the collateral amount, with at least one "
                                   "confirmation and not used by another masternode."),
                                m_col_wallet_box));
        m_col_utxo_combo = new QComboBox(m_col_wallet_box);
        m_col_utxo_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_col_utxo_combo->setMinimumContentsLength(40);
        box->addWidget(m_col_utxo_combo);
        m_col_utxo_none = MakeHint(tr("No suitable output found in this wallet."), m_col_wallet_box);
        m_col_utxo_none->setVisible(false);
        box->addWidget(m_col_utxo_none);
    }
    layout->addWidget(m_col_wallet_box);

    m_col_external = new QRadioButton(tr("Reference an external collateral (e.g. hardware wallet)"), page);
    layout->addWidget(m_col_external);
    m_col_external_box = new QWidget(page);
    {
        auto* box{new QVBoxLayout(m_col_external_box)};
        box->setContentsMargins(24, 0, 0, 0);
        box->addWidget(MakeHint(tr("The collateral must be a confirmed P2PKH output of exactly the collateral "
                                   "amount. After review you will be asked to sign a message with the collateral "
                                   "key outside this wallet."),
                                m_col_external_box));
        auto* row{new QHBoxLayout()};
        m_col_txid = new QLineEdit(m_col_external_box);
        m_col_txid->setPlaceholderText(tr("Collateral transaction id (64 hexadecimal characters)"));
        m_col_txid->setMaxLength(64);
        row->addWidget(m_col_txid, /*stretch=*/1);
        m_col_vout = new QSpinBox(m_col_external_box);
        m_col_vout->setRange(0, 99999);
        m_col_vout->setToolTip(tr("Output index"));
        row->addWidget(m_col_vout);
        box->addLayout(row);
    }
    layout->addWidget(m_col_external_box);
    layout->addStretch();

    const auto update_boxes = [this] {
        m_col_fund_box->setVisible(m_col_fund->isChecked());
        m_col_wallet_box->setVisible(m_col_wallet->isChecked());
        m_col_external_box->setVisible(m_col_external->isChecked());
        if (m_col_wallet->isChecked()) refreshCollateralCandidates();
        rebuildOrder();
    };
    connect(m_col_fund, &QRadioButton::toggled, this, update_boxes);
    connect(m_col_wallet, &QRadioButton::toggled, this, update_boxes);
    connect(m_col_external, &QRadioButton::toggled, this, update_boxes);
    update_boxes();
    return page;
}

QWidget* RegisterMasternodeWizard::createServicePage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Service addresses"), page));
    layout->addWidget(MakeHint(tr("Public addresses your masternode will serve the Core P2P network on, separated "
                                  "by commas or spaces. Each entry must be unique on the network."),
                               page));
    m_service_edit = new QLineEdit(page);
    m_service_edit->setPlaceholderText(QString("1.2.3.4:%1").arg(Params().GetDefaultPort()));
    layout->addWidget(m_service_edit);
    layout->addWidget(MakeHint(tr("May be left empty; the masternode then stays inactive until you send a service "
                                  "update with an address."),
                               page));
    layout->addStretch();
    return page;
}

QWidget* RegisterMasternodeWizard::createKeysPage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Keys"), page));

    layout->addWidget(MakeHint(tr("Owner address (P2PKH). Controls this masternode; its key signs payout and "
                                  "voting updates. Kept in this wallet by default."),
                               page));
    auto* owner_row{new QHBoxLayout()};
    m_owner_edit = new QValidatedLineEdit(page);
    GUIUtil::setupAddressWidget(m_owner_edit, this);
    owner_row->addWidget(m_owner_edit, /*stretch=*/1);
    auto* owner_fresh{new QPushButton(tr("Use new address"), page)};
    connect(owner_fresh, &QPushButton::clicked, this, [this] {
        QString err;
        const QString addr{freshAddress(err)};
        if (addr.isEmpty()) {
            showError(err);
        } else {
            m_owner_edit->setText(addr);
        }
    });
    owner_row->addWidget(owner_fresh);
    layout->addLayout(owner_row);

    layout->addWidget(MakeHint(tr("Voting address (P2PKH). May be delegated; leave empty to vote with the owner "
                                  "key."),
                               page));
    m_voting_edit = new QValidatedLineEdit(page);
    GUIUtil::setupAddressWidget(m_voting_edit, this);
    m_voting_edit->setPlaceholderText(tr("Leave empty to use the owner address"));
    layout->addWidget(m_voting_edit);

    layout->addWidget(MakeHint(tr("Operator key (BLS). The operator runs the masternode server; only the public "
                                  "key is registered on-chain."),
                               page));
    m_operator_widget = new OperatorKeyWidget(page);
    layout->addWidget(m_operator_widget);
    layout->addStretch();
    return page;
}

QWidget* RegisterMasternodeWizard::createPayoutPage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Payout"), page));

    layout->addWidget(MakeHint(tr("Address receiving this masternode's block rewards (P2PKH or P2SH)."), page));
    auto* payout_row{new QHBoxLayout()};
    m_payout_edit = new QValidatedLineEdit(page);
    GUIUtil::setupAddressWidget(m_payout_edit, this);
    payout_row->addWidget(m_payout_edit, /*stretch=*/1);
    auto* payout_fresh{new QPushButton(tr("Use new address"), page)};
    connect(payout_fresh, &QPushButton::clicked, this, [this] {
        QString err;
        const QString addr{freshAddress(err)};
        if (addr.isEmpty()) {
            showError(err);
        } else {
            m_payout_edit->setText(addr);
        }
    });
    payout_row->addWidget(payout_fresh);
    layout->addLayout(payout_row);

    layout->addWidget(MakeHint(tr("Share of the reward promised to the operator:"), page));
    m_operator_reward = new QDoubleSpinBox(page);
    m_operator_reward->setRange(0.0, 100.0);
    m_operator_reward->setDecimals(2);
    m_operator_reward->setSuffix(QString::fromUtf8(" %"));
    layout->addWidget(m_operator_reward);
    m_reward_warning = MakeHint(tr("The operator will permanently receive this share of all rewards of this "
                                   "masternode. Leave it at 0 unless you have an agreement with your operator."),
                                page);
    m_reward_warning->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
    m_reward_warning->setVisible(false);
    layout->addWidget(m_reward_warning);
    connect(m_operator_reward, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_reward_warning->setVisible(value > 0.0); });
    layout->addStretch();
    return page;
}

QWidget* RegisterMasternodeWizard::createPlatformPage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Platform services"), page));

    layout->addWidget(MakeHint(tr("Platform node ID, derived from the Platform P2P public key (40 hexadecimal "
                                  "characters)."),
                               page));
    m_platform_nodeid = new QLineEdit(page);
    m_platform_nodeid->setMaxLength(40);
    m_platform_nodeid->setPlaceholderText(QString("f2dbd9b0a1f541a7c44d34a58674d0262f5feca5"));
    layout->addWidget(m_platform_nodeid);

    m_platform_addr_box = new QWidget(page);
    {
        auto* box{new QVBoxLayout(m_platform_addr_box)};
        box->setContentsMargins(0, 0, 0, 0);
        box->addWidget(MakeHint(tr("Platform P2P addresses (ADDR:PORT, separated by commas or spaces):"),
                                m_platform_addr_box));
        m_platform_p2p = new QLineEdit(m_platform_addr_box);
        m_platform_p2p->setPlaceholderText(QString("1.2.3.4:26656"));
        box->addWidget(m_platform_p2p);
        box->addWidget(MakeHint(tr("Platform HTTPS API addresses (ADDR:PORT, separated by commas or spaces):"),
                                m_platform_addr_box));
        m_platform_https = new QLineEdit(m_platform_addr_box);
        m_platform_https->setPlaceholderText(QString("1.2.3.4:443"));
        box->addWidget(m_platform_https);
    }
    layout->addWidget(m_platform_addr_box);

    m_platform_port_box = new QWidget(page);
    {
        auto* box{new QVBoxLayout(m_platform_port_box)};
        box->setContentsMargins(0, 0, 0, 0);
        box->addWidget(MakeHint(tr("Before v24 activation only the Platform ports can be registered; they apply to "
                                   "the first service address."),
                                m_platform_port_box));
        auto* row{new QHBoxLayout()};
        row->addWidget(new QLabel(tr("Platform P2P port:"), m_platform_port_box));
        m_platform_p2p_port = new QSpinBox(m_platform_port_box);
        m_platform_p2p_port->setRange(1, 65535);
        m_platform_p2p_port->setValue(26656);
        row->addWidget(m_platform_p2p_port);
        row->addWidget(new QLabel(tr("Platform HTTPS port:"), m_platform_port_box));
        m_platform_https_port = new QSpinBox(m_platform_port_box);
        m_platform_https_port->setRange(1, 65535);
        m_platform_https_port->setValue(443);
        row->addWidget(m_platform_https_port);
        row->addStretch();
        box->addLayout(row);
    }
    layout->addWidget(m_platform_port_box);
    layout->addStretch();
    return page;
}

QWidget* RegisterMasternodeWizard::createFeePage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Fee source"), page));
    m_fee_explain = MakeHint(QString(), page);
    layout->addWidget(m_fee_explain);
    m_fee_picker = new FeeSourcePicker(page);
    m_fee_picker->setWalletModel(m_walletModel);
    layout->addWidget(m_fee_picker);
    layout->addStretch();
    return page;
}

QWidget* RegisterMasternodeWizard::createReviewPage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Review"), page));
    m_review_label = new QLabel(page);
    m_review_label->setWordWrap(true);
    m_review_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_review_label);
    layout->addStretch();
    return page;
}

QWidget* RegisterMasternodeWizard::createSignPage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Prove collateral ownership"), page));
    m_sign_address_label = MakeHint(QString(), page);
    layout->addWidget(m_sign_address_label);
    layout->addWidget(MakeHint(tr("Sign the following message with the collateral key (for example with your "
                                  "hardware wallet's sign-message feature), then paste the base64 signature "
                                  "below."),
                               page));
    m_sign_message = new QPlainTextEdit(page);
    m_sign_message->setReadOnly(true);
    m_sign_message->setMaximumHeight(90);
    layout->addWidget(m_sign_message);
    auto* copy_row{new QHBoxLayout()};
    auto* copy_button{new QPushButton(tr("Copy message"), page)};
    connect(copy_button, &QPushButton::clicked, this,
            [this] { GUIUtil::setClipboard(m_sign_message->toPlainText()); });
    copy_row->addWidget(copy_button);
    copy_row->addStretch();
    layout->addLayout(copy_row);
    m_sig_edit = new QPlainTextEdit(page);
    m_sig_edit->setPlaceholderText(tr("Paste the base64 signature here"));
    m_sig_edit->setMaximumHeight(90);
    layout->addWidget(m_sig_edit);
    layout->addStretch();
    return page;
}

QWidget* RegisterMasternodeWizard::createResultPage()
{
    auto* page{new QWidget(this)};
    auto* layout{new QVBoxLayout(page)};
    layout->addWidget(MakeTitle(tr("Masternode registered"), page));
    m_result_label = new QLabel(page);
    m_result_label->setWordWrap(true);
    m_result_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_result_label);

    m_secret_box = new QWidget(page);
    {
        auto* box{new QVBoxLayout(m_secret_box)};
        box->setContentsMargins(0, 12, 0, 0);
        auto* warn{MakeHint(tr("Operator secret key — save it now. It is not stored anywhere and will never be "
                               "shown again."),
                            m_secret_box)};
        warn->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
        box->addWidget(warn);
        m_secret_edit = new QLineEdit(m_secret_box);
        m_secret_edit->setReadOnly(true);
        box->addWidget(m_secret_edit);
        box->addWidget(MakeHint(tr("Add this line to dash.conf on your masternode server:"), m_secret_box));
        auto* conf_row{new QHBoxLayout()};
        m_conf_line_edit = new QLineEdit(m_secret_box);
        m_conf_line_edit->setReadOnly(true);
        conf_row->addWidget(m_conf_line_edit, /*stretch=*/1);
        auto* copy_conf{new QPushButton(tr("Copy"), m_secret_box)};
        connect(copy_conf, &QPushButton::clicked, this,
                [this] { GUIUtil::setClipboard(m_conf_line_edit->text()); });
        conf_row->addWidget(copy_conf);
        box->addLayout(conf_row);
        box->addWidget(MakeHint(tr("Type the last 4 characters of the secret key to confirm you saved it:"),
                                m_secret_box));
        m_confirm_edit = new QLineEdit(m_secret_box);
        m_confirm_edit->setMaxLength(4);
        m_confirm_edit->setMaximumWidth(120);
        connect(m_confirm_edit, &QLineEdit::textChanged, this, &RegisterMasternodeWizard::updateButtons);
        box->addWidget(m_confirm_edit);
    }
    layout->addWidget(m_secret_box);

    m_next_steps = new QLabel(page);
    m_next_steps->setWordWrap(true);
    layout->addWidget(m_next_steps);
    layout->addStretch();
    return page;
}

bool RegisterMasternodeWizard::isEvo() const
{
    return m_type_evo->isChecked();
}

bool RegisterMasternodeWizard::isExternalCollateral() const
{
    return m_col_external->isChecked();
}

bool RegisterMasternodeWizard::isFundCollateral() const
{
    return m_col_fund->isChecked();
}

QString RegisterMasternodeWizard::collateralAddress() const
{
    return isFundCollateral() ? m_col_address->text().trimmed() : QString();
}

QString RegisterMasternodeWizard::ownerAddress() const
{
    return m_owner_edit->text().trimmed();
}

QString RegisterMasternodeWizard::votingAddress() const
{
    return m_voting_edit->text().trimmed();
}

QString RegisterMasternodeWizard::freshAddress(QString& err) const
{
    err.clear();
    if (m_walletModel == nullptr) {
        err = tr("No wallet is available.");
        return {};
    }
    auto dest{m_walletModel->wallet().getNewDestination(/*label=*/"")};
    if (!dest) {
        err = tr("Could not generate a new address: %1")
                  .arg(QString::fromStdString(util::ErrorString(dest).translated));
        return {};
    }
    return QString::fromStdString(EncodeDestination(*dest));
}

CAmount RegisterMasternodeWizard::collateralAmount() const
{
    return GetMnType(isEvo() ? MnType::Evo : MnType::Regular).collat_amount;
}

bool RegisterMasternodeWizard::secretConfirmed() const
{
    return m_confirm_edit->text().trimmed().compare(m_operator_widget->secretHex().right(4),
                                                    Qt::CaseInsensitive) == 0;
}

void RegisterMasternodeWizard::rebuildOrder()
{
    m_order = {PageType, PageCollateral, PageService, PageKeys, PagePayout};
    if (isEvo()) m_order << PagePlatform;
    m_order << PageFee << PageReview;
    if (isExternalCollateral()) m_order << PageSign;
    m_order << PageResult;
    if (m_pos >= m_order.size()) m_pos = m_order.size() - 1;
}

void RegisterMasternodeWizard::goToPage(Page page)
{
    const int pos{m_order.indexOf(page)};
    if (pos < 0) return;
    m_pos = pos;
    enterPage(page);
}

void RegisterMasternodeWizard::enterPage(Page page)
{
    m_pages->setCurrentIndex(page);
    showError(QString());
    switch (page) {
    case PageCollateral:
        if (m_col_address->text().isEmpty() && m_walletModel != nullptr) {
            QString err;
            const QString addr{freshAddress(err)};
            if (!addr.isEmpty()) m_col_address->setText(addr);
        }
        refreshCollateralCandidates();
        break;
    case PageKeys:
        if (m_owner_edit->text().isEmpty() && m_walletModel != nullptr) {
            QString err;
            const QString addr{freshAddress(err)};
            if (!addr.isEmpty()) m_owner_edit->setText(addr);
        }
        break;
    case PagePayout:
        if (m_payout_edit->text().isEmpty() && m_walletModel != nullptr) {
            QString err;
            const QString addr{freshAddress(err)};
            if (!addr.isEmpty()) m_payout_edit->setText(addr);
        }
        break;
    case PagePlatform: {
        const bool v3{m_node.isV24Active()};
        m_platform_addr_box->setVisible(v3);
        m_platform_port_box->setVisible(!v3);
        break;
    }
    case PageFee: {
        const CAmount required{isFundCollateral() ? collateralAmount() + FEE_MARGIN : FEE_MARGIN};
        m_fee_picker->setMinimumBalance(required);
        m_fee_picker->refresh();
        if (isFundCollateral()) {
            m_fee_explain->setText(tr("The selected address funds the %1 collateral plus the transaction fee, so "
                                      "it must hold at least %2. Change returns to the same address.")
                                       .arg(FormatAmount(m_walletModel, collateralAmount()),
                                            FormatAmount(m_walletModel, required)));
        } else {
            m_fee_explain->setText(tr("The selected address pays the transaction fee."));
        }
        break;
    }
    case PageReview:
        populateReview();
        break;
    default:
        break;
    }
    updateButtons();
}

bool RegisterMasternodeWizard::validatePage(Page page, QString& err)
{
    err.clear();
    switch (page) {
    case PageCollateral:
        if (isFundCollateral()) {
            if (!MasternodeWidgetUtil::isP2PKHorP2SHAddress(collateralAddress())) {
                err = tr("Enter a valid collateral address (P2PKH or P2SH).");
            }
        } else if (m_col_wallet->isChecked()) {
            if (m_col_utxo_combo->currentIndex() < 0) {
                err = tr("This wallet has no unspent output of exactly %1. Fund the collateral from the wallet "
                         "instead.")
                          .arg(FormatAmount(m_walletModel, collateralAmount()));
            }
        } else {
            const QString txid{m_col_txid->text().trimmed()};
            if (txid.length() != 64 || !IsHex(txid.toStdString())) {
                err = tr("Enter the collateral transaction id as 64 hexadecimal characters.");
            }
        }
        break;
    case PageService: {
        QString parse_err;
        const QStringList list{MasternodeWidgetUtil::parseServiceList(m_service_edit->text(), parse_err)};
        if (!parse_err.isEmpty()) {
            err = parse_err;
        } else if (list.isEmpty() && isEvo() && !m_node.isV24Active()) {
            err = tr("Evonodes need at least one service address to carry the Platform ports before v24 "
                     "activation.");
        }
        break;
    }
    case PageKeys:
        if (!MasternodeWidgetUtil::isP2PKHAddress(ownerAddress())) {
            err = tr("Enter a valid owner address (P2PKH).");
        } else if (isFundCollateral() && ownerAddress() == collateralAddress()) {
            err = tr("The owner address must differ from the collateral address.");
        } else if (!votingAddress().isEmpty() && !MasternodeWidgetUtil::isP2PKHAddress(votingAddress())) {
            err = tr("Enter a valid voting address (P2PKH), or leave it empty to use the owner address.");
        } else if (!m_operator_widget->isValid()) {
            err = tr("Enter a valid operator BLS public key (96 hexadecimal characters, basic scheme).");
        }
        break;
    case PagePayout: {
        const QString payout{m_payout_edit->text().trimmed()};
        if (!MasternodeWidgetUtil::isP2PKHorP2SHAddress(payout)) {
            err = tr("Enter a valid payout address (P2PKH or P2SH).");
        } else if (isFundCollateral() && payout == collateralAddress()) {
            err = tr("The payout address must differ from the collateral address.");
        }
        break;
    }
    case PagePlatform: {
        const QString nodeid{m_platform_nodeid->text().trimmed()};
        if (nodeid.length() != 40 || !IsHex(nodeid.toStdString())) {
            err = tr("Enter the Platform node ID as 40 hexadecimal characters.");
            break;
        }
        if (m_node.isV24Active()) {
            QString parse_err;
            const QStringList p2p{MasternodeWidgetUtil::parseServiceList(m_platform_p2p->text(), parse_err)};
            if (!parse_err.isEmpty()) {
                err = tr("Platform P2P: %1").arg(parse_err);
                break;
            }
            const QStringList https{MasternodeWidgetUtil::parseServiceList(m_platform_https->text(), parse_err)};
            if (!parse_err.isEmpty()) {
                err = tr("Platform HTTPS: %1").arg(parse_err);
                break;
            }
            QString service_err;
            const bool has_service{!MasternodeWidgetUtil::parseServiceList(m_service_edit->text(), service_err).isEmpty()};
            if (p2p.isEmpty() != https.isEmpty()) {
                err = tr("Enter both a Platform P2P and a Platform HTTPS address; one cannot be registered "
                         "without the other.");
            } else if (has_service && p2p.isEmpty()) {
                err = tr("Enter at least one Platform P2P and one Platform HTTPS address, or clear the service "
                         "addresses to set everything later with a service update.");
            }
        }
        break;
    }
    case PageFee:
        if (m_fee_picker->currentIndex() < 0) {
            const CAmount required{isFundCollateral() ? collateralAmount() + FEE_MARGIN : FEE_MARGIN};
            err = tr("No wallet address holds at least %1.").arg(FormatAmount(m_walletModel, required));
        }
        break;
    case PageSign: {
        const QString sig{m_sig_edit->toPlainText().simplified().remove(' ')};
        if (sig.isEmpty() || !DecodeBase64(sig.toStdString()).has_value()) {
            err = tr("Paste the base64-encoded signature of the message above, made with the collateral key.");
        }
        break;
    }
    default:
        break;
    }
    return err.isEmpty();
}

void RegisterMasternodeWizard::onNext()
{
    if (m_busy) return;
    const Page current{m_order[m_pos]};
    QString err;
    if (!validatePage(current, err)) {
        showError(err);
        return;
    }
    showError(QString());
    if (current == PageReview) {
        startRegistration();
    } else if (current == PageSign) {
        startSubmit();
    } else if (current == PageResult) {
        accept();
    } else {
        ++m_pos;
        enterPage(m_order[m_pos]);
    }
}

void RegisterMasternodeWizard::onBack()
{
    if (m_busy || m_pos == 0) return;
    --m_pos;
    enterPage(m_order[m_pos]);
}

void RegisterMasternodeWizard::updateButtons()
{
    const Page current{m_order[m_pos]};
    m_back_button->setVisible(current != PageType && current != PageResult);
    // After register_prepare the transaction is fixed; going back would
    // silently invalidate the message being signed.
    m_back_button->setEnabled(!m_busy && current != PageSign);
    m_cancel_button->setVisible(current != PageResult);
    m_cancel_button->setEnabled(!m_busy);

    if (!m_busy) {
        QString next_text{tr("Next")};
        if (current == PageReview) {
            next_text = isExternalCollateral() ? tr("Prepare") : tr("Register");
        } else if (current == PageSign) {
            next_text = tr("Submit");
        } else if (current == PageResult) {
            next_text = tr("Finish");
        }
        m_next_button->setText(next_text);
    }

    bool enabled{!m_busy};
    QString tooltip;
    if (m_walletModel == nullptr) {
        enabled = false;
        tooltip = tr("No wallet is available. Masternode registration requires a wallet.");
    } else if (m_walletModel->wallet().privateKeysDisabled()) {
        enabled = false;
        tooltip = tr("This wallet is watch-only. Masternode registration requires a wallet that can sign "
                     "transactions.");
    } else if (current == PageResult && m_operator_widget->hasGeneratedSecret() && !secretConfirmed()) {
        enabled = false;
        tooltip = tr("Confirm you saved the operator secret key by typing its last 4 characters.");
    }
    m_next_button->setEnabled(enabled);
    m_next_button->setToolTip(tooltip);
}

void RegisterMasternodeWizard::setBusy(bool busy, const QString& busy_text)
{
    m_busy = busy;
    if (busy) {
        m_next_button->setText(busy_text.isEmpty() ? tr("Working…") : busy_text);
    }
    updateButtons();
}

void RegisterMasternodeWizard::showError(const QString& message)
{
    m_error_label->setText(message);
    m_error_label->setVisible(!message.isEmpty());
}

void RegisterMasternodeWizard::refreshCollateralCandidates()
{
    m_col_utxo_combo->clear();
    if (m_walletModel == nullptr) {
        m_col_utxo_none->setVisible(true);
        return;
    }
    const CAmount collateral{collateralAmount()};
    for (const auto& [dest, coins] : m_walletModel->wallet().listCoins()) {
        for (const auto& [outpoint, txout] : coins) {
            if (txout.txout.nValue != collateral || txout.is_spent || txout.depth_in_main_chain < 1) continue;
            if (m_walletModel->wallet().isLockedCoin(outpoint)) continue;
            // protx register only accepts P2PKH collaterals; check the coin's own
            // script, as listCoins() groups change under the parent's address
            CTxDestination coin_dest;
            if (!ExtractDestination(txout.txout.scriptPubKey, coin_dest) ||
                !std::holds_alternative<PKHash>(coin_dest)) {
                continue;
            }
            const QString address{QString::fromStdString(EncodeDestination(coin_dest))};
            const QString txid{QString::fromStdString(outpoint.hash.ToString())};
            m_col_utxo_combo->addItem(QString("%1:%2 (%3)").arg(txid).arg(outpoint.n).arg(address), txid);
            m_col_utxo_combo->setItemData(m_col_utxo_combo->count() - 1, static_cast<int>(outpoint.n), VOUT_ROLE);
        }
    }
    m_col_utxo_none->setVisible(m_col_utxo_combo->count() == 0);
}

void RegisterMasternodeWizard::populateReview()
{
    QStringList rows;
    const auto row = [&rows](const QString& key, const QString& value) {
        rows << QString("<b>%1</b> %2").arg(key, value.toHtmlEscaped());
    };

    row(tr("Type:"), isEvo() ? tr("Evonode") : tr("Masternode"));
    if (isFundCollateral()) {
        row(tr("Collateral:"), tr("send %1 to %2")
                                   .arg(FormatAmount(m_walletModel, collateralAmount()), collateralAddress()));
    } else if (m_col_wallet->isChecked()) {
        row(tr("Collateral:"), m_col_utxo_combo->currentText());
    } else {
        row(tr("Collateral:"),
            QString("%1:%2").arg(m_col_txid->text().trimmed()).arg(m_col_vout->value()));
    }
    QString parse_err;
    const QStringList services{MasternodeWidgetUtil::parseServiceList(m_service_edit->text(), parse_err)};
    row(tr("Service:"), services.isEmpty() ? tr("(none — set later with a service update)") : services.join(", "));
    row(tr("Owner address:"), ownerAddress());
    row(tr("Voting address:"),
        votingAddress().isEmpty() ? tr("%1 (owner)").arg(ownerAddress()) : votingAddress());
    row(tr("Operator key:"), m_operator_widget->hasGeneratedSecret() ?
                                 tr("%1 (newly generated)").arg(m_operator_widget->publicKeyHex()) :
                                 m_operator_widget->publicKeyHex());
    row(tr("Payout address:"), m_payout_edit->text().trimmed());
    row(tr("Operator reward:"), QString("%1%").arg(QString::number(m_operator_reward->value(), 'f', 2)));
    if (isEvo()) {
        row(tr("Platform node ID:"), m_platform_nodeid->text().trimmed());
        if (m_node.isV24Active()) {
            QString platform_err;
            row(tr("Platform P2P:"),
                MasternodeWidgetUtil::parseServiceList(m_platform_p2p->text(), platform_err).join(", "));
            row(tr("Platform HTTPS:"),
                MasternodeWidgetUtil::parseServiceList(m_platform_https->text(), platform_err).join(", "));
        } else {
            row(tr("Platform ports:"), QString("%1 (P2P), %2 (HTTPS)")
                                           .arg(m_platform_p2p_port->value())
                                           .arg(m_platform_https_port->value()));
        }
    }
    row(isFundCollateral() ? tr("Funding address:") : tr("Fee source:"), m_fee_picker->selectedAddress());

    QString footer;
    if (isExternalCollateral()) {
        footer = tr("Clicking Prepare creates the unsigned transaction and a message you must sign with the "
                    "collateral key. Nothing is broadcast yet.");
    } else {
        footer = tr("Clicking Register may ask to unlock the wallet, then broadcasts the transaction.");
    }
    m_review_label->setText(rows.join("<br>") + "<br><br>" + footer.toHtmlEscaped());
}

UniValue RegisterMasternodeWizard::buildRegisterParams() const
{
    UniValue params(UniValue::VOBJ);
    if (isFundCollateral()) {
        params.pushKV("collateralAddress", collateralAddress().toStdString());
    } else if (m_col_wallet->isChecked()) {
        params.pushKV("collateralHash", m_col_utxo_combo->currentData().toString().toStdString());
        params.pushKV("collateralIndex",
                      m_col_utxo_combo->itemData(m_col_utxo_combo->currentIndex(), VOUT_ROLE).toInt());
    } else {
        params.pushKV("collateralHash", m_col_txid->text().trimmed().toStdString());
        params.pushKV("collateralIndex", m_col_vout->value());
    }

    QString parse_err;
    UniValue core_addrs(UniValue::VARR);
    for (const QString& entry : MasternodeWidgetUtil::parseServiceList(m_service_edit->text(), parse_err)) {
        core_addrs.push_back(entry.toStdString());
    }
    params.pushKV("coreP2PAddrs", core_addrs);
    params.pushKV("ownerAddress", ownerAddress().toStdString());
    params.pushKV("operatorPubKey", m_operator_widget->publicKeyHex().toStdString());
    params.pushKV("votingAddress", votingAddress().toStdString());
    params.pushKV("operatorReward", QString::number(m_operator_reward->value(), 'f', 2).toStdString());
    params.pushKV("payoutAddress", m_payout_edit->text().trimmed().toStdString());
    if (isEvo()) {
        params.pushKV("platformNodeID", m_platform_nodeid->text().trimmed().toStdString());
        if (m_node.isV24Active()) {
            UniValue p2p_addrs(UniValue::VARR);
            for (const QString& entry : MasternodeWidgetUtil::parseServiceList(m_platform_p2p->text(), parse_err)) {
                p2p_addrs.push_back(entry.toStdString());
            }
            UniValue https_addrs(UniValue::VARR);
            for (const QString& entry :
                 MasternodeWidgetUtil::parseServiceList(m_platform_https->text(), parse_err)) {
                https_addrs.push_back(entry.toStdString());
            }
            params.pushKV("platformP2PAddrs", p2p_addrs);
            params.pushKV("platformHTTPSAddrs", https_addrs);
        } else {
            params.pushKV("platformP2PAddrs", m_platform_p2p_port->value());
            params.pushKV("platformHTTPSAddrs", m_platform_https_port->value());
        }
    }
    if (isFundCollateral()) {
        params.pushKV("fundAddress", m_fee_picker->selectedAddress().toStdString());
    } else {
        params.pushKV("feeSourceAddress", m_fee_picker->selectedAddress().toStdString());
    }
    if (!isExternalCollateral()) {
        params.pushKV("submit", true);
    }
    return params;
}

void RegisterMasternodeWizard::startRegistration()
{
    if (m_walletModel == nullptr) return;
    m_unlock = std::make_unique<UnlockHolder>(*m_walletModel);
    if (!m_unlock->ctx.isValid()) {
        m_unlock.reset();
        return;
    }

    QString method;
    if (isFundCollateral()) {
        method = isEvo() ? "protx register_fund_evo" : "protx register_fund";
    } else if (isExternalCollateral()) {
        method = isEvo() ? "protx register_prepare_evo" : "protx register_prepare";
    } else {
        method = isEvo() ? "protx register_evo" : "protx register";
    }
    m_stage = isExternalCollateral() ? Stage::Prepare : Stage::Register;
    setBusy(true, isExternalCollateral() ? tr("Preparing…") : tr("Registering…"));
    if (!m_sender.execute(method, buildRegisterParams(), m_walletModel)) {
        m_stage = Stage::None;
        m_unlock.reset();
        setBusy(false);
    }
}

void RegisterMasternodeWizard::startSubmit()
{
    if (m_walletModel == nullptr) return;
    m_unlock = std::make_unique<UnlockHolder>(*m_walletModel);
    if (!m_unlock->ctx.isValid()) {
        m_unlock.reset();
        return;
    }

    UniValue params(UniValue::VOBJ);
    params.pushKV("tx", m_prepared_tx.toStdString());
    params.pushKV("sig", m_sig_edit->toPlainText().simplified().remove(' ').toStdString());
    m_stage = Stage::Submit;
    setBusy(true, tr("Submitting…"));
    if (!m_sender.execute("protx register_submit", params, m_walletModel)) {
        m_stage = Stage::None;
        m_unlock.reset();
        setBusy(false);
    }
}

void RegisterMasternodeWizard::onRpcFinished(const ProTxResult& result)
{
    const Stage stage{m_stage};
    m_stage = Stage::None;
    m_unlock.reset();
    setBusy(false);

    if (!result.ok) {
        QMessageBox::critical(this, tr("Registration failed"), result.message);
        return;
    }

    switch (stage) {
    case Stage::Register:
    case Stage::Submit: {
        const QString txid{result.value.isStr() ? QString::fromStdString(result.value.get_str()) :
                                                  QString::fromStdString(result.value.write())};
        populateResult(txid);
        goToPage(PageResult);
        break;
    }
    case Stage::Prepare: {
        const UniValue& tx{result.value.find_value("tx")};
        const UniValue& address{result.value.find_value("collateralAddress")};
        const UniValue& message{result.value.find_value("signMessage")};
        if (!result.value.isObject() || !tx.isStr() || !address.isStr() || !message.isStr()) {
            showError(tr("Unexpected reply to the prepare request."));
            return;
        }
        m_prepared_tx = QString::fromStdString(tx.get_str());
        m_sign_address_label->setText(
            tr("Collateral address holding the key: %1").arg(QString::fromStdString(address.get_str())));
        m_sign_message->setPlainText(QString::fromStdString(message.get_str()));
        goToPage(PageSign);
        break;
    }
    case Stage::None:
        break;
    }
}

void RegisterMasternodeWizard::populateResult(const QString& pro_tx_hash)
{
    m_registered = true;
    m_result_label->setText(tr("The provider registration transaction was sent to the network.") + "<br><br>" +
                            tr("Provider transaction hash:") + "<br><b>" + pro_tx_hash.toHtmlEscaped() + "</b>");

    const bool generated{m_operator_widget->hasGeneratedSecret()};
    m_secret_box->setVisible(generated);
    if (generated) {
        m_secret_edit->setText(m_operator_widget->secretHex());
        m_conf_line_edit->setText(QString("masternodeblsprivkey=%1").arg(m_operator_widget->secretHex()));
    }

    QStringList steps;
    steps << tr("Next steps:");
    if (generated) {
        steps << tr("1. Add the line above to dash.conf on your masternode server and restart dashd there.");
    } else {
        steps << tr("1. Configure your masternode server with the BLS secret key matching the operator public key "
                    "you provided.");
    }
    steps << tr("2. Make sure the service address and port are reachable from the internet.");
    if (isEvo()) {
        steps << tr("3. Provision the Dash Platform services (Tenderdash and Drive) with the Platform node key "
                    "matching the node ID you registered.");
        steps << tr("4. The evonode becomes active once the registration is confirmed and the first update is "
                    "signed by the operator.");
    } else {
        steps << tr("3. The masternode becomes active once the registration is confirmed on the network.");
    }
    m_next_steps->setText(steps.join("<br>"));
}

void RegisterMasternodeWizard::reject()
{
    if (m_busy) return;
    if (m_registered) {
        if (m_operator_widget->hasGeneratedSecret() && !secretConfirmed() &&
            QMessageBox::warning(this, tr("Operator secret not confirmed"),
                                 tr("You have not confirmed saving the operator secret key. Without it your "
                                    "masternode cannot operate. Close anyway?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        QDialog::accept();
        return;
    }
    if (!m_prepared_tx.isEmpty()) {
        if (QMessageBox::warning(this, tr("Discard prepared registration?"),
                                 tr("The prepared registration will be discarded. If the collateral belongs to "
                                    "this wallet it stays locked; unlock it via coin control if you do not intend "
                                    "to retry."),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
    }
    QDialog::reject();
}
