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

#include <vector>

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
//! Group `text` into blocks of `chunk_size` characters separated by spaces. A BLS
//! key is one 96-character word that a wrapping label refuses to break; grouped,
//! it wraps between blocks and stays readable.
QString chunked(const QString& text, int chunk_size = 12);
//! Monospace value with a Copy button beside it. `display` is shown as given,
//! `copy_text` is what the button puts on the clipboard, so a key can be shown
//! in chunks and still be copied unbroken.
QWidget* makeCopyableValue(const QString& display, const QString& copy_text, QWidget* parent);
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

//! Operator key selection: derive a BLS key pair from the wallet's HD seed,
//! generate a fresh one (basic scheme), or paste an existing operator public
//! key. A derived or generated secret is kept only in memory here; the owning
//! dialog is responsible for deriving it, storing it in the wallet, or showing
//! it to the user once.
class OperatorKeyWidget : public QWidget
{
    Q_OBJECT

public:
    //! Where the operator key comes from and what happens to its secret
    enum class Mode {
        Derive,           //!< derived by the wallet from its HD seed; see setDerivedKey()
        GenerateAndStore, //!< generated here; the caller stores the secret in the wallet
        GenerateOnly,     //!< generated here; the secret exists only in this dialog
        Existing,         //!< an operator public key supplied by the user
    };

    explicit OperatorKeyWidget(QWidget* parent = nullptr);

    //! Offer the "derive from this wallet's recovery phrase" choice and select
    //! it. Deriving needs an unlocked wallet, so the key itself is not produced
    //! here: the owning dialog derives it when the user is about to register and
    //! hands it back through setDerivedKey(). Only callers doing that may call
    //! this. Meant to be called once while setting the widget up.
    void offerDerived();

    //! Offer the "generate and save in this wallet" choice and select it. Only
    //! callers that actually store secretHex() after a successful registration
    //! may enable it, so it stays hidden until this is called. `enabled` false
    //! shows the choice but leaves it unselectable, explaining why through
    //! `disabled_reason`. Meant to be called once while setting the widget up.
    void offerStoreInWallet(bool enabled, const QString& disabled_reason = QString());

    //! Adopt the key the wallet derived: `secret` is its raw 32-byte BLS secret
    //! and `path` the derivation path to show beside it. False when `secret` is
    //! not a usable key, in which case the widget keeps waiting for one.
    bool setDerivedKey(const std::vector<unsigned char>& secret, const QString& path);
    //! True while Derive is selected and setDerivedKey() has not succeeded yet
    bool needsDerivation() const;
    //! Derivation path passed to setDerivedKey(); empty until then
    QString derivationPath() const { return m_derived_path; }

    //! Currently selected mode
    Mode mode() const;
    //! Selected operator public key in basic-scheme hex (empty when invalid, and
    //! while a derived key has not been handed back yet)
    QString publicKeyHex() const;
    //! Derived or generated secret key hex; empty when an existing public key is
    //! used, and while a derived key has not been handed back yet
    QString secretHex() const;
    //! True when this widget holds the operator secret (derived or generated),
    //! rather than only the public key of a secret somebody else keeps
    bool hasGeneratedSecret() const;
    //! True when publicKeyHex() would return a usable key, or will once the
    //! caller has derived it
    bool isValid() const;

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void updateState();

private:
    QFrame* m_derive_card;
    QFrame* m_store_card;
    QRadioButton* m_derive_radio;
    QRadioButton* m_store_radio;
    QRadioButton* m_generate_radio;
    QRadioButton* m_existing_radio;
    QWidget* m_derive_body;
    QWidget* m_store_body;
    QWidget* m_generate_body;
    QWidget* m_existing_body;
    QLabel* m_derive_pending;
    QWidget* m_derive_key_box;
    QLabel* m_derive_public_label;
    QLabel* m_derive_path_label;
    QValidatedLineEdit* m_existing_edit;
    QString m_derived_secret;
    QString m_derived_public;
    QString m_derived_path;
    QString m_generated_secret;
    QString m_generated_public;
};

#endif // BITCOIN_QT_MASTERNODEWIDGETS_H
