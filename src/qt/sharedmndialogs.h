// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SHAREDMNDIALOGS_H
#define BITCOIN_QT_SHAREDMNDIALOGS_H

#include <qt/masternodedialogs.h>

#include <consensus/amount.h>
#include <interfaces/node.h>

#include <QDialog>
#include <QString>
#include <QWidget>

#include <map>
#include <vector>

struct ProTxResult;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
QT_END_NAMESPACE

//! Send a ProUpShareTx ("protx update_share"): change the reward address of one
//! collateral share of a shared masternode. Only shares whose owner key this
//! wallet holds are offered; every other share field is immutable.
class UpdateShareDialog : public MasternodeActionDialog
{
    Q_OBJECT

public:
    UpdateShareDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent = nullptr);

protected:
    void validate() override;
    void submit() override;

private:
    const bool m_v24_active;

    QComboBox* m_share_combo{nullptr};
    QValidatedLineEdit* m_reward_edit{nullptr};
    FeeSourcePicker* m_fee_source{nullptr};
};

//! Collects share owner signatures for a multi-party shared masternode
//! transaction (unanimous ProDisTx or ProUpSharedRegTx): shows the prepared
//! transaction and its check phrase, exports/imports a small session envelope
//! via clipboard and file, and verifies every imported signature natively
//! against the share owner keys (an unverifiable signature is never counted).
class SharedSigCollector : public QWidget
{
    Q_OBJECT

public:
    enum class Kind {
        Dissolve,  //!< unanimous ProDisTx from "protx dissolve_prepare"
        Registrar, //!< ProUpSharedRegTx from "protx update_shared_registrar_prepare"
    };

    SharedSigCollector(Kind kind, const QString& pro_tx_hash, const std::vector<interfaces::MnShare>& shares,
                       QWidget* parent = nullptr);

    bool hasTransaction() const { return !m_tx_hex.isEmpty(); }
    const QString& txHex() const { return m_tx_hex; }
    //! Lowercase hex of the digest every share owner signs, recomputed natively
    //! from the adopted transaction (empty when no transaction is adopted)
    QString signHashHex() const;
    //! Adopt the prepared transaction (rejects a different transaction than
    //! the one already adopted; signatures collected so far are kept)
    bool setTransaction(const QString& tx_hex, QString& error);

    //! Verify and absorb an array of {shareIndex, signature} entries (the
    //! "protx shared_sign" result shape). Returns the number of newly absorbed
    //! signatures; `error` collects per-entry rejections.
    int addSignatures(const UniValue& entries, QString& error);

    int signedCount() const { return static_cast<int>(m_sigs.size()); }
    bool complete() const { return m_sigs.size() == m_shares.size(); }
    //! Signature entries in the shape "protx shared_combine" expects
    UniValue signaturesArray() const;

Q_SIGNALS:
    //! The transaction or the signature set changed
    void changed();

private Q_SLOTS:
    void copyEnvelope();
    void saveEnvelope();
    void pasteEnvelope();
    void loadEnvelope();

private:
    UniValue envelopeJson() const;
    bool importEnvelope(const QString& text, QString& error);
    bool importEnvelopeChecked(const UniValue& json, QString& error);
    void refreshUi();
    void showStatus(const QString& message, bool error);

    const Kind m_kind;
    const QString m_protx_hash;
    const std::vector<interfaces::MnShare> m_shares;
    QString m_tx_hex;
    std::map<int, QString> m_sigs; //!< shareIndex -> base64 signature

    QPlainTextEdit* m_tx_view{nullptr};
    QLabel* m_phrase_label{nullptr};
    QLabel* m_checklist_label{nullptr};
    QLabel* m_status_label{nullptr};
    QPushButton* m_copy_button{nullptr};
    QPushButton* m_save_button{nullptr};
};

//! Scaffolding for the larger shared masternode dialogs that run several RPC
//! commands over their lifetime: wallet/watch-only gating and the unlock +
//! busy-state + ProTxSender round trip with the result returned to the caller.
class SharedMnDialog : public QDialog
{
    Q_OBJECT

protected:
    SharedMnDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, QWidget* parent);

    //! True when a wallet able to sign transactions is available
    bool canSign() const;
    //! Disable `button` with an explanatory tooltip when no signing wallet is
    //! available or the v24 hard fork is not active yet; returns true when the
    //! button stays usable
    bool gateButton(QPushButton* button) const;
    //! Unlock the wallet, execute `method` with named `params` on the RPC
    //! bridge and wait in a local event loop (the dialog is disabled
    //! meanwhile). Returns false with a user-displayable `error` when the
    //! command could not run or failed.
    bool runCommand(const QString& method, const UniValue& params, ProTxResult& result, QString& error);

    //! True when every input of `tx_hex` is still unspent. A spent input makes
    //! the transaction permanently unbroadcastable: the session is dead and a
    //! new one must be started.
    bool txInputsUnspent(const QString& tx_hex, QString& error);

    //! Indexes into m_shares whose owner key this wallet can sign with
    std::vector<int> myShareIndexes() const;
    //! Combo box over myShareIndexes(): "Share #i — amount — owner address"
    QComboBox* makeMyShareCombo(QWidget* parent);

    interfaces::Node& m_node;
    WalletModel* const m_wallet_model;
    const QString m_protx_hash;
    const std::vector<interfaces::MnShare> m_shares;
    const CAmount m_early_penalty;
    const uint32_t m_early_period_blocks;
    const int m_registered_height;
    const bool m_v24_active;

private:
    ProTxSender* m_sender;
    bool m_busy{false};
};

//! Dissolution of a shared masternode ("protx dissolve" / "protx
//! dissolve_prepare" + "protx shared_sign" + "protx shared_combine"):
//! - Dissolve now: unilateral, with a live penalty preview mirroring the real
//!   transaction's outputs;
//! - Unanimous: penalty-free at any height, needs every share owner's
//!   signature, collected via a SharedSigCollector envelope;
//! - Standby: generate and store both offline dissolution variants
//!   (payPenalty true/false) next to the refund key backup.
class DissolveDialog : public SharedMnDialog
{
    Q_OBJECT

public:
    DissolveDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry, int current_height,
                   QWidget* parent = nullptr);

    //! Whether both standby variants for this masternode were ever saved to
    //! file from this GUI (per-proTx QSettings flag)
    static bool StandbyStored(const QString& pro_tx_hash);

private Q_SLOTS:
    void updateNowPreview();
    void submitNow();
    void startUnanimous();
    void signUnanimous();
    void combineUnanimous();

private:
    QWidget* buildNowTab();
    QWidget* buildUnanimousTab();
    QWidget* buildStandbyTab();
    void generateStandby(bool pay_penalty);
    void saveStandby(bool pay_penalty);
    void maybeMarkStandbyStored();
    void refreshUnanimousUi();

    const int m_current_height;

    // Dissolve now
    QComboBox* m_now_actor{nullptr};
    QSpinBox* m_now_fee{nullptr};
    QLabel* m_now_early_label{nullptr};
    QLabel* m_now_nudge_label{nullptr};
    QLabel* m_now_error_label{nullptr};
    QTableWidget* m_now_table{nullptr};
    QCheckBox* m_now_accept{nullptr};
    QPushButton* m_now_submit{nullptr};
    QLineEdit* m_now_result{nullptr};
    QLabel* m_now_status{nullptr};

    // Unanimous
    QComboBox* m_un_actor{nullptr};
    QSpinBox* m_un_fee{nullptr};
    QPushButton* m_un_start{nullptr};
    QPushButton* m_un_sign{nullptr};
    QPushButton* m_un_combine{nullptr};
    SharedSigCollector* m_un_collector{nullptr};
    QLineEdit* m_un_result{nullptr};
    QLabel* m_un_status{nullptr};

    // Standby
    QComboBox* m_sb_actor{nullptr};
    QSpinBox* m_sb_fee{nullptr};
    QPlainTextEdit* m_sb_view_a{nullptr};
    QPlainTextEdit* m_sb_view_b{nullptr};
    QLabel* m_sb_stored_label{nullptr};
    QLabel* m_sb_status{nullptr};
    bool m_sb_saved_a{false};
    bool m_sb_saved_b{false};
};

//! Rotate a shared masternode's operator and/or voting key ("protx
//! update_shared_registrar_prepare" + "protx shared_sign" + "protx
//! shared_combine"). The prepare and the final combine must run on the same
//! wallet, whose coins pay the fee; other participants open this dialog only
//! to import the envelope, sign and export.
class RotateSharedKeysDialog : public SharedMnDialog
{
    Q_OBJECT

public:
    RotateSharedKeysDialog(interfaces::Node& node, WalletModel* wallet_model, const MasternodeEntry& entry,
                           QWidget* parent = nullptr);

private Q_SLOTS:
    void validateForm();
    void prepare();
    void signMine();
    void combineAndSend();

private:
    void refreshButtons();

    QLineEdit* m_operator_edit{nullptr};
    QValidatedLineEdit* m_voting_edit{nullptr};
    FeeSourcePicker* m_fee_source{nullptr};
    QLabel* m_pose_warning{nullptr};
    QPushButton* m_prepare_button{nullptr};
    QPushButton* m_sign_button{nullptr};
    QPushButton* m_combine_button{nullptr};
    SharedSigCollector* m_collector{nullptr};
    QLineEdit* m_result_edit{nullptr};
    QLabel* m_status_label{nullptr};
};

#endif // BITCOIN_QT_SHAREDMNDIALOGS_H
