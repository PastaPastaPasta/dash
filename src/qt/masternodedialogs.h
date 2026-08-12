// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODEDIALOGS_H
#define BITCOIN_QT_MASTERNODEDIALOGS_H

#include <evo/dmn_types.h>

#include <QDialog>
#include <QString>

#include <vector>

class FeeSourcePicker;
class MasternodeEntry;
class ProTxSender;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLayout;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QValidatedLineEdit;
class UniValue;
class WalletModel;

namespace interfaces {
class Node;
} // namespace interfaces

//! Shared scaffolding for the small masternode maintenance dialogs: intro text,
//! a caller-built form, an error label and OK/Cancel buttons; wallet and
//! watch-only gating; and the unlock + busy-state + ProTxSender round trip.
class MasternodeActionDialog : public QDialog
{
    Q_OBJECT

public Q_SLOTS:
    //! Ignore Esc, Cancel and window-close while a protx command is in flight,
    //! so the dialog cannot disappear mid-transaction
    void reject() override;

protected:
    MasternodeActionDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent);

    //! Complete construction: place `form` between the intro text and the error
    //! label + button box, then run the first validate() pass
    void setupUi(const QString& title, const QString& intro, QLayout* form, const QString& ok_text);

    //! Append a "Fee source:" row (picker plus explanatory `note`) to `form` and
    //! return the picker, already wired to the dialog's wallet model
    FeeSourcePicker* addFeeSourceRow(QFormLayout* form, const QString& note);

    //! True when a wallet able to sign transactions is available
    bool canSign() const;
    //! Record the outcome of the derived dialog's validate() pass; the OK button
    //! additionally requires canSign() and no command in flight
    void setOkValid(bool valid);
    void showError(const QString& message);
    void clearError();
    //! Unlock the wallet, execute `method` with named `params` on the RPC bridge
    //! and report the outcome (error label on failure, txid message box + accept()
    //! on success). Waits in a local event loop so the unlock context stays alive
    //! on the GUI thread across the call; the dialog is disabled meanwhile.
    void runProTx(const QString& method, const UniValue& params);

    interfaces::Node& m_node;
    WalletModel* const m_wallet_model;
    const QString m_protx_hash;
    const bool m_is_shared;

protected Q_SLOTS:
    //! Recompute field validity and call setOkValid()
    virtual void validate() = 0;
    //! Perform final checks, assemble named params and call runProTx()
    virtual void submit() = 0;

private:
    void setBusy(bool busy);

    ProTxSender* m_sender;
    QDialogButtonBox* m_button_box{nullptr};
    QPushButton* m_ok_button{nullptr};
    QLabel* m_status_label{nullptr};
    bool m_busy{false};
    bool m_valid{false};
};

//! Send a ProUpServTx ("protx update_service" / "protx update_service_evo"):
//! update the network addresses (and for evonodes the Platform fields) of a
//! masternode, signed with the operator secret key. Also revives a PoSe-banned
//! masternode. A fee source is mandatory for shared masternodes, which have no
//! payout address of their own to draw the fee from.
class UpdateServiceDialog : public MasternodeActionDialog
{
    Q_OBJECT

public:
    UpdateServiceDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent = nullptr);

protected:
    void validate() override;
    void submit() override;

private:
    const MnType m_type;
    const uint16_t m_operator_reward_pct;
    const bool m_v24_active;

    QLineEdit* m_addrs_edit{nullptr};
    QLineEdit* m_operator_key_edit{nullptr};
    QLineEdit* m_platform_node_id_edit{nullptr};
    // v24 active: ADDR:PORT lists. Pre-v24 a ProUpServTx only carries bare
    // Platform port numbers, so port spinboxes are shown instead.
    QLineEdit* m_platform_p2p_edit{nullptr};
    QLineEdit* m_platform_https_edit{nullptr};
    QSpinBox* m_platform_p2p_port_edit{nullptr};
    QSpinBox* m_platform_https_port_edit{nullptr};
    QValidatedLineEdit* m_operator_payout_edit{nullptr};
    FeeSourcePicker* m_fee_source{nullptr};
};

//! Send a ProUpRegTx ("protx update_registrar"): update the operator key,
//! voting address and/or payout address of a masternode. Requires the owner key
//! in the wallet. Never applicable to shared masternodes (guarded here as well
//! as at the call site).
class UpdateRegistrarDialog : public MasternodeActionDialog
{
    Q_OBJECT

public:
    UpdateRegistrarDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent = nullptr);

protected:
    void validate() override;
    void submit() override;

private:
    QLineEdit* m_operator_pubkey_edit{nullptr};
    QValidatedLineEdit* m_voting_edit{nullptr};
    QValidatedLineEdit* m_payout_edit{nullptr};
    FeeSourcePicker* m_fee_source{nullptr};
};

//! Send a ProUpRevTx ("protx revoke"): clear the masternode's service address
//! and PoSe-ban it until a new operator key is set, signed with the operator
//! secret key. Mainly needed before re-registering with the same IP address or
//! operator key.
class RevokeDialog : public MasternodeActionDialog
{
    Q_OBJECT

public:
    RevokeDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent = nullptr);

protected:
    void validate() override;
    void submit() override;

private:
    QComboBox* m_reason_combo{nullptr};
    QLineEdit* m_operator_key_edit{nullptr};
    FeeSourcePicker* m_fee_source{nullptr};
};

//! Show the operator secret key this wallet holds for a masternode, together
//! with the dash.conf line the masternode server needs. Sends nothing: it only
//! reads the key back out of the wallet, which is why it is a plain dialog
//! rather than a MasternodeActionDialog.
class ShowOperatorKeyDialog : public QDialog
{
    Q_OBJECT

public:
    //! `operator_pubkey` is the masternode's registered operator public key, 48
    //! bytes in basic-scheme serialization. The wallet is unlocked while
    //! constructing; when that is refused, or the key turns out not to be this
    //! wallet's after all, the dialog says so instead of showing a key.
    ShowOperatorKeyDialog(WalletModel* wallet_model, const QString& protx_hash,
                          const std::vector<unsigned char>& operator_pubkey, QWidget* parent = nullptr);
};

#endif // BITCOIN_QT_MASTERNODEDIALOGS_H
