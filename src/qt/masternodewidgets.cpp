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
#include <qt/optionsmodel.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

namespace {
constexpr int BALANCE_ROLE{Qt::UserRole + 1};
} // anonymous namespace

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

    std::vector<std::pair<QString, CAmount>> entries;
    for (const auto& [dest, coins] : m_wallet_model->wallet().listCoins()) {
        CAmount total{0};
        for (const auto& [outpoint, txout] : coins) {
            if (txout.is_spent || txout.depth_in_main_chain < 0) continue;
            if (m_wallet_model->wallet().isLockedCoin(outpoint)) continue;
            total += txout.txout.nValue;
        }
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
        setItemData(count() - 1, static_cast<qlonglong>(balance), BALANCE_ROLE);
    }
    if (count() > 0) setCurrentIndex(0);
}

QString FeeSourcePicker::selectedAddress() const
{
    return currentData().toString();
}

CAmount FeeSourcePicker::selectedBalance() const
{
    if (currentIndex() < 0) return 0;
    return static_cast<CAmount>(itemData(currentIndex(), BALANCE_ROLE).toLongLong());
}

OperatorKeyWidget::OperatorKeyWidget(QWidget* parent) :
    QWidget(parent)
{
    // Generate the key pair up front so toggling the radios never discards a
    // key the user may already have seen.
    CBLSSecretKey secret_key;
    secret_key.MakeNewKey();
    m_generated_secret = QString::fromStdString(secret_key.ToString(/*specificLegacyScheme=*/false));
    m_generated_public = QString::fromStdString(secret_key.GetPublicKey().ToString(/*specificLegacyScheme=*/false));

    m_generate_radio = new QRadioButton(tr("Generate a new operator key (recommended)"), this);
    m_generate_radio->setChecked(true);
    m_generated_label = new QLabel(m_generated_public, this);
    m_generated_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_generated_label->setWordWrap(true);
    m_existing_radio = new QRadioButton(tr("Use an existing operator public key"), this);
    m_existing_edit = new QValidatedLineEdit(this);
    m_existing_edit->setPlaceholderText(tr("Operator BLS public key (96 hexadecimal characters, basic scheme)"));
    m_existing_edit->setEnabled(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_generate_radio);
    auto* generated_row = new QHBoxLayout();
    generated_row->addSpacing(24);
    generated_row->addWidget(m_generated_label, /*stretch=*/1);
    layout->addLayout(generated_row);
    layout->addWidget(m_existing_radio);
    auto* existing_row = new QHBoxLayout();
    existing_row->addSpacing(24);
    existing_row->addWidget(m_existing_edit, /*stretch=*/1);
    layout->addLayout(existing_row);

    connect(m_generate_radio, &QRadioButton::toggled, this, &OperatorKeyWidget::updateState);
    connect(m_existing_edit, &QLineEdit::textChanged, this, &OperatorKeyWidget::updateState);
}

void OperatorKeyWidget::updateState()
{
    const bool use_existing{m_existing_radio->isChecked()};
    m_existing_edit->setEnabled(use_existing);
    if (use_existing) {
        const QString text{m_existing_edit->text().trimmed()};
        // Leave the neutral style while empty; only flag actual invalid input
        m_existing_edit->setValid(text.isEmpty() || isValid());
    }
    Q_EMIT changed();
}

QString OperatorKeyWidget::publicKeyHex() const
{
    if (m_generate_radio->isChecked()) return m_generated_public;
    return isValid() ? m_existing_edit->text().trimmed() : QString();
}

QString OperatorKeyWidget::secretHex() const
{
    return hasGeneratedSecret() ? m_generated_secret : QString();
}

bool OperatorKeyWidget::hasGeneratedSecret() const
{
    return m_generate_radio->isChecked();
}

bool OperatorKeyWidget::isValid() const
{
    if (m_generate_radio->isChecked()) return true;
    CBLSPublicKey pubkey;
    return pubkey.SetHexStr(m_existing_edit->text().trimmed().toStdString(), /*specificLegacyScheme=*/false);
}
