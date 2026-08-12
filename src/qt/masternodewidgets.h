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
class QFrame;
class QLabel;
class QRadioButton;
class QVBoxLayout;
QT_END_NAMESPACE

//! Free validation and layout helpers shared by the masternode management dialogs
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

//! Spacing tokens (px) keeping one vertical rhythm across the dialogs' pages
constexpr int GROUP_SPACING{16}; //!< between top-level groups of a page
constexpr int TITLE_SPACING{8};  //!< between a group's title and its body
constexpr int ROW_SPACING{10};   //!< between rows inside a group
constexpr int BODY_INDENT{26};   //!< indent of a body under its radio header
constexpr int CARD_PADDING{12};  //!< a card's internal padding

//! Bold label. A negative `point_size` keeps the theme's own size.
QLabel* makeTitle(const QString& text, QWidget* parent, double point_size = -1);
//! Dim, word-wrapped explanation
QLabel* makeHint(const QString& text, QWidget* parent);
//! Word-wrapped, selectable plain-text value; `monospace` for addresses, hashes and keys
QLabel* makeValue(const QString& text, QWidget* parent, bool monospace = false);
//! Frame grouping one choice or one section, named "mnCard" for theming
QFrame* makeCard(QWidget* parent);

//! A card presenting one choice: `header` (the radio button) sits on top, the
//! optional `hint` below it, and `body` holds the controls that only matter
//! while the choice is selected, so it can be hidden to collapse the card.
struct OptionCard {
    QFrame* card;
    QWidget* body;
    QVBoxLayout* body_layout;
};
//! Build an option card. `header` is re-parented into the card, so radio
//! buttons of one page need a QButtonGroup to stay mutually exclusive.
OptionCard makeOptionCard(QWidget* parent, QWidget* header, const QString& hint = QString());
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
//! memory; the owning dialog is responsible for storing it in the wallet or
//! showing it to the user once.
class OperatorKeyWidget : public QWidget
{
    Q_OBJECT

public:
    //! Where the operator key comes from and what happens to its secret
    enum class Mode {
        GenerateAndStore, //!< generated here; the caller stores the secret in the wallet
        GenerateOnly,     //!< generated here; the secret exists only in this dialog
        Existing,         //!< an operator public key supplied by the user
    };

    explicit OperatorKeyWidget(QWidget* parent = nullptr);

    //! Offer the "generate and save in this wallet" choice and select it. Only
    //! callers that actually store secretHex() after a successful registration
    //! may enable it, so it stays hidden until this is called. `enabled` false
    //! shows the choice but leaves it unselectable, explaining why through
    //! `disabled_reason`. Meant to be called once while setting the widget up.
    void offerStoreInWallet(bool enabled, const QString& disabled_reason = QString());

    //! Currently selected mode
    Mode mode() const;
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
    QFrame* m_store_card;
    QRadioButton* m_store_radio;
    QRadioButton* m_generate_radio;
    QRadioButton* m_existing_radio;
    QWidget* m_store_body;
    QWidget* m_generate_body;
    QWidget* m_existing_body;
    QValidatedLineEdit* m_existing_edit;
    QString m_generated_secret;
    QString m_generated_public;
};

#endif // BITCOIN_QT_MASTERNODEWIDGETS_H
