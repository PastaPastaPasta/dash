// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodewidgets.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <netbase.h>
#include <script/standard.h>
#include <util/strencodings.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <map>
#include <utility>
#include <variant>
#include <vector>

namespace MasternodeWidgetUtil {

QStringList parseServiceList(const QString& input, QString& err)
{
    err.clear();
    QStringList ret;
    QSet<QString> seen;
    const QStringList tokens{input.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts)};
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

bool isP2PKHAddress(const QString& address)
{
    const CTxDestination dest{DecodeDestination(address.trimmed().toStdString())};
    return std::holds_alternative<PKHash>(dest);
}

bool isP2PKHorP2SHAddress(const QString& address)
{
    const CTxDestination dest{DecodeDestination(address.trimmed().toStdString())};
    return std::holds_alternative<PKHash>(dest) || std::holds_alternative<ScriptHash>(dest);
}

QLabel* makeTitle(const QString& text, QWidget* parent, double point_size)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    GUIUtil::setFont({label}, GUIUtil::FontWeight::Bold, point_size);
    return label;
}

QLabel* makeHint(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_SECONDARY));
    return label;
}

QLabel* makeValue(const QString& text, QWidget* parent, bool monospace)
{
    auto* label{new QLabel(text, parent)};
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (monospace) label->setFont(GUIUtil::fixedPitchFont());
    return label;
}

QString chunked(const QString& text, int chunk_size)
{
    if (chunk_size <= 0) return text;
    QString ret;
    ret.reserve(text.size() + text.size() / chunk_size);
    for (int pos = 0; pos < text.size(); pos += chunk_size) {
        if (pos > 0) ret += QLatin1Char(' ');
        ret += text.mid(pos, chunk_size);
    }
    return ret;
}

QWidget* makeCopyableValue(const QString& display, const QString& copy_text, QWidget* parent)
{
    auto* row_widget{new QWidget(parent)};
    auto* row{new QHBoxLayout(row_widget)};
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(TITLE_SPACING);
    auto* value{makeValue(display, row_widget, /*monospace=*/true)};
    value->setToolTip(copy_text);
    row->addWidget(value, /*stretch=*/1);
    auto* copy{new QPushButton(QObject::tr("Copy"), row_widget)};
    copy->setToolTip(QObject::tr("Copy to clipboard"));
    QObject::connect(copy, &QPushButton::clicked, copy, [copy_text] { GUIUtil::setClipboard(copy_text); });
    row->addWidget(copy, /*stretch=*/0, Qt::AlignTop);
    return row_widget;
}

QFrame* makeCard(QWidget* parent)
{
    auto* card{new QFrame(parent)};
    card->setObjectName("mnCard");
    // The id selector keeps the border off the card's children. general.css
    // clears borders for plain frames, so the color comes from the theme's own
    // widget border color instead of a new one.
    card->setStyleSheet(QString("QFrame#mnCard { border: 1px solid %1; border-radius: 3px; }")
                            .arg(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BORDER_WIDGET).name()));
    return card;
}

OptionCard makeOptionCard(QWidget* parent, QWidget* header, const QString& hint)
{
    OptionCard ret;
    ret.card = makeCard(parent);
    auto* card_layout{new QVBoxLayout(ret.card)};
    card_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    card_layout->setSpacing(TITLE_SPACING);
    card_layout->addWidget(header);
    if (!hint.isEmpty()) {
        auto* hint_row{new QHBoxLayout()};
        hint_row->setContentsMargins(BODY_INDENT, 0, 0, 0);
        hint_row->addWidget(makeHint(hint, ret.card), /*stretch=*/1);
        card_layout->addLayout(hint_row);
    }
    ret.body = new QWidget(ret.card);
    ret.body_layout = new QVBoxLayout(ret.body);
    ret.body_layout->setContentsMargins(BODY_INDENT, 0, 0, 0);
    ret.body_layout->setSpacing(ROW_SPACING);
    card_layout->addWidget(ret.body);
    return ret;
}

} // namespace MasternodeWidgetUtil

FeeSourcePicker::FeeSourcePicker(QWidget* parent) :
    QComboBox(parent)
{
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(40);
}

void FeeSourcePicker::setWalletModel(WalletModel* wallet_model)
{
    m_wallet_model = wallet_model;
    refresh();
}

void FeeSourcePicker::setMinimumBalance(CAmount minimum)
{
    if (m_minimum_balance == minimum) return;
    m_minimum_balance = minimum;
    refresh();
}

void FeeSourcePicker::refresh()
{
    clear();
    if (m_wallet_model == nullptr) return;

    // Group each coin under its own scriptPubKey destination: the RPCs'
    // FundSpecialTx only selects coins paying exactly the chosen address, while
    // listCoins() groups change outputs under their parent non-change address,
    // which would overstate what a protx call can actually spend.
    std::map<CTxDestination, CAmount> balances;
    for (const auto& [dest, coins] : m_wallet_model->wallet().listCoins()) {
        for (const auto& [outpoint, txout] : coins) {
            if (txout.is_spent || txout.depth_in_main_chain < 0) continue;
            if (m_wallet_model->wallet().isLockedCoin(outpoint)) continue;
            CTxDestination coin_dest;
            if (!ExtractDestination(txout.txout.scriptPubKey, coin_dest)) continue;
            balances[coin_dest] += txout.txout.nValue;
        }
    }

    std::vector<std::pair<QString, CAmount>> entries;
    for (const auto& [dest, total] : balances) {
        if (total <= 0 || total < m_minimum_balance) continue;
        entries.emplace_back(QString::fromStdString(EncodeDestination(dest)), total);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

    const auto unit{m_wallet_model->getOptionsModel()->getDisplayUnit()};
    for (const auto& [address, balance] : entries) {
        addItem(QString("%1 (%2)").arg(address, BitcoinUnits::formatWithUnit(unit, balance, /*plussign=*/false,
                                                                             BitcoinUnits::SeparatorStyle::ALWAYS)),
                address);
    }
    if (count() > 0) setCurrentIndex(0);
}

QString FeeSourcePicker::selectedAddress() const
{
    return currentData().toString();
}

OperatorKeyWidget::OperatorKeyWidget(QWidget* parent) :
    QWidget(parent)
{
    using namespace MasternodeWidgetUtil;

    // Generate the key pair up front so toggling the radios never discards a
    // key the user may already have seen.
    CBLSSecretKey secret_key;
    secret_key.MakeNewKey();
    m_generated_secret = QString::fromStdString(secret_key.ToString(/*specificLegacyScheme=*/false));
    m_generated_public = QString::fromStdString(secret_key.GetPublicKey().ToString(/*specificLegacyScheme=*/false));

    // The key is 96 hex characters: it gets its own full-width line, otherwise it
    // wraps against the edge of whatever sits beside it
    const auto public_key_row = [this](QWidget* body) {
        auto* column{new QVBoxLayout()};
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(TITLE_SPACING);
        column->addWidget(makeHint(tr("Public key"), body));
        column->addWidget(makeValue(chunked(m_generated_public), body, /*monospace=*/true));
        return column;
    };

    m_derive_radio = new QRadioButton(tr("Derive from this wallet's recovery phrase"), this);
    auto derive_card{makeOptionCard(this, m_derive_radio,
                                    tr("The secret key comes from this wallet's recovery phrase, so restoring that "
                                       "phrase restores this key."))};
    m_derive_card = derive_card.card;
    m_derive_body = derive_card.body;
    // Deriving needs an unlocked wallet, so the key only appears once the owning
    // dialog has asked for it
    m_derive_pending = makeHint(tr("The key is derived when you reach the review; this wallet may ask you to "
                                   "unlock first."),
                                m_derive_body);
    derive_card.body_layout->addWidget(m_derive_pending);
    m_derive_key_box = new QWidget(m_derive_body);
    {
        auto* column{new QVBoxLayout(m_derive_key_box)};
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(TITLE_SPACING);
        column->addWidget(makeHint(tr("Public key"), m_derive_key_box));
        m_derive_public_label = makeValue(QString(), m_derive_key_box, /*monospace=*/true);
        column->addWidget(m_derive_public_label);
        m_derive_path_label = makeHint(QString(), m_derive_key_box);
        column->addWidget(m_derive_path_label);
    }
    m_derive_key_box->setVisible(false);
    derive_card.body_layout->addWidget(m_derive_key_box);

    m_store_radio = new QRadioButton(tr("Generate and save in this wallet"), this);
    auto store_card{makeOptionCard(this, m_store_radio,
                                   tr("The secret key is stored in this wallet, so the wallet backup covers it."))};
    m_store_card = store_card.card;
    m_store_body = store_card.body;
    store_card.body_layout->addLayout(public_key_row(m_store_body));

    m_generate_radio = new QRadioButton(tr("Generate without saving"), this);
    m_generate_radio->setChecked(true);
    auto generate_card{makeOptionCard(this, m_generate_radio,
                                      tr("The secret key is shown once after registering and kept nowhere else."))};
    m_generate_body = generate_card.body;
    generate_card.body_layout->addLayout(public_key_row(m_generate_body));

    m_existing_radio = new QRadioButton(tr("Use an existing operator public key"), this);
    auto existing_card{makeOptionCard(this, m_existing_radio,
                                      tr("For a hosted node: the operator keeps the secret key and gives you the "
                                         "public key."))};
    m_existing_body = existing_card.body;
    m_existing_edit = new QValidatedLineEdit(m_existing_body);
    m_existing_edit->setPlaceholderText(tr("Operator BLS public key (96 hexadecimal characters, basic scheme)"));
    m_existing_edit->setEnabled(false);
    existing_card.body_layout->addWidget(m_existing_edit);

    // The radios live in separate cards, so auto-exclusivity by parent no
    // longer applies and a button group has to keep them exclusive.
    auto* group{new QButtonGroup(this)};
    group->addButton(m_derive_radio);
    group->addButton(m_store_radio);
    group->addButton(m_generate_radio);
    group->addButton(m_existing_radio);

    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(TITLE_SPACING);
    layout->addWidget(m_derive_card);
    layout->addWidget(m_store_card);
    layout->addWidget(generate_card.card);
    layout->addWidget(existing_card.card);

    // Both stay hidden until a caller commits to deriving or storing the secret;
    // dialogs that can do neither keep the generate/paste pair they always had.
    m_derive_card->setVisible(false);
    m_store_card->setVisible(false);

    connect(m_derive_radio, &QRadioButton::toggled, this, &OperatorKeyWidget::updateState);
    connect(m_store_radio, &QRadioButton::toggled, this, &OperatorKeyWidget::updateState);
    connect(m_generate_radio, &QRadioButton::toggled, this, &OperatorKeyWidget::updateState);
    connect(m_existing_radio, &QRadioButton::toggled, this, &OperatorKeyWidget::updateState);
    connect(m_existing_edit, &QLineEdit::textChanged, this, &OperatorKeyWidget::updateState);
    updateState();
}

void OperatorKeyWidget::offerDerived()
{
    m_derive_card->setVisible(true);
    m_derive_radio->setChecked(true);
    updateState();
}

bool OperatorKeyWidget::setDerivedKey(const std::vector<unsigned char>& secret, const QString& path)
{
    const CBLSSecretKey key{secret};
    if (!key.IsValid()) return false;
    m_derived_secret = QString::fromStdString(key.ToString(/*specificLegacyScheme=*/false));
    m_derived_public = QString::fromStdString(key.GetPublicKey().ToString(/*specificLegacyScheme=*/false));
    m_derived_path = path;
    m_derive_public_label->setText(MasternodeWidgetUtil::chunked(m_derived_public));
    m_derive_path_label->setText(path.isEmpty() ? QString() : tr("Derivation path %1").arg(path));
    m_derive_path_label->setVisible(!path.isEmpty());
    updateState();
    return true;
}

bool OperatorKeyWidget::needsDerivation() const
{
    return mode() == Mode::Derive && m_derived_public.isEmpty();
}

void OperatorKeyWidget::offerStoreInWallet(bool enabled, const QString& disabled_reason)
{
    m_store_card->setVisible(true);
    m_store_radio->setEnabled(enabled);
    // A disabled radio button swallows tooltips, so the card carries it as well.
    m_store_card->setToolTip(enabled ? QString() : disabled_reason);
    m_store_radio->setToolTip(enabled ? QString() : disabled_reason);
    if (enabled) {
        m_store_radio->setChecked(true);
    } else if (m_store_radio->isChecked()) {
        m_generate_radio->setChecked(true);
    }
    updateState();
}

void OperatorKeyWidget::updateState()
{
    const Mode current{mode()};
    m_derive_body->setVisible(current == Mode::Derive);
    m_derive_pending->setVisible(m_derived_public.isEmpty());
    m_derive_key_box->setVisible(!m_derived_public.isEmpty());
    m_store_body->setVisible(current == Mode::GenerateAndStore);
    m_generate_body->setVisible(current == Mode::GenerateOnly);
    m_existing_body->setVisible(current == Mode::Existing);
    m_existing_edit->setEnabled(current == Mode::Existing);
    if (current == Mode::Existing) {
        const QString text{m_existing_edit->text().trimmed()};
        // Leave the neutral style while empty; only flag actual invalid input
        m_existing_edit->setValid(text.isEmpty() || isValid());
    }
    Q_EMIT changed();
}

OperatorKeyWidget::Mode OperatorKeyWidget::mode() const
{
    if (m_existing_radio->isChecked()) return Mode::Existing;
    if (m_derive_radio->isChecked()) return Mode::Derive;
    if (m_store_radio->isChecked()) return Mode::GenerateAndStore;
    return Mode::GenerateOnly;
}

QString OperatorKeyWidget::publicKeyHex() const
{
    switch (mode()) {
    case Mode::Derive:
        return m_derived_public;
    case Mode::GenerateAndStore:
    case Mode::GenerateOnly:
        return m_generated_public;
    case Mode::Existing:
        return isValid() ? m_existing_edit->text().trimmed() : QString();
    }
    return {};
}

QString OperatorKeyWidget::secretHex() const
{
    switch (mode()) {
    case Mode::Derive:
        return m_derived_secret;
    case Mode::GenerateAndStore:
    case Mode::GenerateOnly:
        return m_generated_secret;
    case Mode::Existing:
        return {};
    }
    return {};
}

bool OperatorKeyWidget::hasGeneratedSecret() const
{
    return mode() != Mode::Existing;
}

bool OperatorKeyWidget::isValid() const
{
    // A derived key is only produced once the caller has an unlocked wallet, so
    // the choice counts as valid before the key itself exists
    if (hasGeneratedSecret()) return true;
    CBLSPublicKey pubkey;
    return pubkey.SetHexStr(m_existing_edit->text().trimmed().toStdString(), /*specificLegacyScheme=*/false);
}
