// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODEWIDGETS_H
#define BITCOIN_QT_MASTERNODEWIDGETS_H

#include <consensus/amount.h>

#include <QComboBox>
#include <QString>
#include <QStringList>
#include <QWidget>

class QValidatedLineEdit;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QRadioButton;
QT_END_NAMESPACE

//! Free validation helpers shared by the masternode management dialogs
namespace MasternodeWidgetUtil {
//! Parse a comma/space separated list of "ADDR:PORT" entries. Entries without
//! a port get the network's default P2P port. Returns the normalized entries,
//! or an empty list with a non-empty `err` on the first invalid entry. An
//! empty/whitespace-only input yields an empty list and no error.
QStringList parseServiceList(const QString& input, QString& err);
//! True when `address` decodes to a P2PKH destination on the current network
bool isP2PKHAddress(const QString& address);
//! True when `address` decodes to a P2PKH or P2SH destination on the current network
bool isP2PKHorP2SHAddress(const QString& address);
} // namespace MasternodeWidgetUtil

//! Combo box listing the wallet's addresses with their spendable balance,
//! largest first. Used to pick the fee source (or funding source) of a protx
//! operation, defaulting to the address most likely able to pay.
class FeeSourcePicker : public QComboBox
{
    Q_OBJECT

public:
    explicit FeeSourcePicker(QWidget* parent = nullptr);

    void setWalletModel(WalletModel* wallet_model);
    //! Hide addresses whose spendable balance is below `minimum` (default 0 = show all)
    void setMinimumBalance(CAmount minimum);
    //! Re-scan the wallet's coins and rebuild the list
    void refresh();

    QString selectedAddress() const;

private:
    WalletModel* m_wallet_model{nullptr};
    CAmount m_minimum_balance{0};
};

//! Operator key selection: generate a fresh BLS key pair (basic scheme) or
//! paste an existing operator public key. The generated secret is kept only in
//! memory; the owning dialog is responsible for showing it to the user once.
class OperatorKeyWidget : public QWidget
{
    Q_OBJECT

public:
    explicit OperatorKeyWidget(QWidget* parent = nullptr);

    //! Selected operator public key in basic-scheme hex (empty when invalid)
    QString publicKeyHex() const;
    //! Generated secret key hex; empty when an existing public key is used
    QString secretHex() const;
    //! True when the user chose to generate a key inside this widget
    bool hasGeneratedSecret() const;
    //! True when publicKeyHex() would return a usable key
    bool isValid() const;

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void updateState();

private:
    QRadioButton* m_generate_radio;
    QRadioButton* m_existing_radio;
    QLabel* m_generated_label;
    QValidatedLineEdit* m_existing_edit;
    QString m_generated_secret;
    QString m_generated_public;
};

#endif // BITCOIN_QT_MASTERNODEWIDGETS_H
