// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SHAREDMNCREATEDIALOG_H
#define BITCOIN_QT_SHAREDMNCREATEDIALOG_H

#include <consensus/amount.h>

#include <qt/mnsharesession.h>

#include <QDialog>
#include <QString>

#include <optional>
#include <vector>

class BitcoinAmountField;
class OperatorKeyWidget;
class ProTxSender;
struct ProTxResult;
class UniValue;
class WalletModel;

namespace interfaces {
class Node;
} // namespace interfaces

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
QT_END_NAMESPACE

//! Resumable multi-party session dialog creating a shared masternode: a group
//! drafts a share table and terms, pools funding inputs, freezes the terms via
//! "protx register_shared_prepare", collects every share owner's consent
//! signature, combines them, has each contributor sign their funding inputs
//! and finally broadcasts the registration. The session state lives in one
//! MnShareSession envelope that participants pass around as JSON (clipboard or
//! file); any participant holding the latest copy can continue the session in
//! this dialog.
class SharedMnCreateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SharedMnCreateDialog(interfaces::Node& node, WalletModel* wallet_model, QWidget* parent = nullptr);
    ~SharedMnCreateDialog() override;

    //! Close protection: prompts to save an unsaved session first
    void reject() override;

private Q_SLOTS:
    // Entry experience
    void createSession();
    void resumeCoordinatorSession();
    void resumeCoordinatorFromClipboard();
    void joinFromClipboard();
    void joinFromFile();

    // Session envelope plumbing (always available)
    void importFromClipboard();
    void exportToClipboard();
    void loadFromFile();
    void saveToFile();

    // Draft page
    void addShareRow();
    void removeShareRow();
    void useMyOwnerAddress();
    void useMyRefundAddress();
    void useMyRewardAddress();
    void useMyVotingAddress();
    void onMyShareChanged(int index);
    void onMyShareDetailsChanged();
    void onShareCellChanged(int row, int column);
    void onTermsChanged();
    void addMyFunding();
    void removeMyContribution();
    void refreshFundingCandidates();
    void autoSelectFeeCandidate(const QString& excluded_txid = {}, quint32 excluded_vout = 0);
    void copyOrFreezeDraft();
    void freezeSession();

    // Frozen/Signing page
    void signConsent();
    void unfreezeSession();

    // Combined page
    void combineSignatures();
    void signFundingInputs();

    // Broadcast page
    void broadcastSession();

    // Dead-session page
    void restartFromDraft();

    // Draft navigation
    void showParticipantsPage();
    void showSettingsPage();
    void showFundingPage();
    void showReviewPage();

private:
    enum Page : int {
        PageLanding = 0,
        PageParticipants,
        PageSettings,
        PageFunding,
        PageReview,
        PageSigning,
        PageCombined,
        PageBroadcast,
        PageDead,
    };

    enum class Role {
        Undecided,
        Coordinator,
        Participant,
    };

    QWidget* buildHeader();
    QWidget* buildLandingPage();
    QWidget* buildParticipantsPage();
    QWidget* buildSettingsPage();
    QWidget* buildFundingPage();
    QWidget* buildReviewPage();
    QWidget* buildSigningPage();
    QWidget* buildCombinedPage();
    QWidget* buildBroadcastPage();
    QWidget* buildDeadPage();

    //! True when a wallet able to sign is selected
    bool canSign() const;
    //! Execute one RPC command on the bridge, waiting in a nested event loop on
    //! the GUI thread (keeps the non-movable unlock context alive when
    //! needs_unlock). Returns false when the call could not be started.
    bool runRpc(const QString& method, const UniValue& params, bool needs_unlock, ProTxResult& result);
    void setBusy(bool busy);

    // Session -> UI
    void refreshAll();
    void refreshHeader();
    void refreshDraftPage();
    void refreshMySharePanel();
    void refreshSharesTable();
    void refreshValidation();
    void refreshContributions();
    void refreshSigningPage();
    void refreshCombinedPage();
    void refreshBroadcastPage();
    void refreshReviewPage();
    void fillMyAddress(QLineEdit* edit);
    int myShareIndex() const;
    bool isDraftPage(int page) const;

    // UI -> session
    void syncShareFromTable(int row);
    void syncTermsToSession();
    void pushTermsToWidgets();

    //! Parse an imported envelope and adopt or merge it into the current session
    void adoptImportedText(const QString& text, const QString& source, bool accept_newer = false);
    void loadFromFileImpl(bool accept_newer);
    void afterImport();
    //! A8: re-check that every recorded funding input is still unspent
    void checkSessionLiveness();
    void markDead(const QString& reason);
    void markDirty();
    //! Prompt to circulate the updated envelope (clipboard + file, per A3)
    void offerExport(const QString& why);
    bool writeSessionToFile();

    //! Share indexes whose owner key this wallet can sign with
    std::vector<int> myShareIndexes() const;
    //! Sum of this wallet's share amounts (what "my funding" must contribute)
    CAmount myShareTotal() const;
    //! Fresh receive address from the wallet, or empty with an error set
    QString freshAddress(QString& error) const;
    //! Send exactly `amount` to a fresh own address and return the created
    //! outpoint (the "chip" that becomes this participant's funding input)
    std::optional<MnShareSession::Input> prepareFundingChip(CAmount amount, QString& error);
    //! Find a single unlocked wallet UTXO of exactly `amount`
    std::optional<MnShareSession::Input> findFundingChip(CAmount amount) const;
    //! Replace the session's transaction hex in place (partially signed funding
    //! inputs) without changing the stage, via an envelope round-trip
    bool replaceSessionProTx(const QString& tx_hex, QString& error);
    //! Lock or unlock a contribution's inputs in this wallet
    void setContributionLocked(const MnShareSession::Contribution& contribution, bool lock);
    void unlockAllContributedCoins();
    //! Lock every contribution input this wallet knows (after adopting an
    //! imported session, so its funding coins cannot be spent by accident)
    void lockKnownContributedCoins();
    //! Whether a recorded funding input can still be spent from this wallet:
    //! tracked here, unspent, and not locked
    bool isSpendableFundingInput(const MnShareSession::Input& input) const;
    //! Value of a recorded funding input: from this wallet for the outputs it
    //! tracks, else from the confirmed UTXO set; nullopt for an own output that
    //! is already spent and for other participants' unconfirmed inputs
    std::optional<CAmount> resolveInputValue(const MnShareSession::Input& input) const;

    interfaces::Node& m_node;
    WalletModel* const m_wallet_model;
    const bool m_v24_active;
    MnShareSession m_session;
    ProTxSender* const m_sender;

    bool m_busy{false};
    bool m_dirty{false};
    bool m_dead{false};
    QString m_dead_reason;
    bool m_updating{false}; //!< guards table/widget refresh against change signals
    Role m_role{Role::Undecided};
    //! The session was imported with an agreed operator key: display it
    //! read-only and never overwrite it from this dialog's key widget
    bool m_operator_key_from_import{false};

    // Header
    QLabel* m_breadcrumb{nullptr};
    QLabel* m_role_label{nullptr};
    QLabel* m_next_action_label{nullptr};
    QPushButton* m_import_button{nullptr};
    QPushButton* m_export_button{nullptr};
    QPushButton* m_more_button{nullptr};
    QAction* m_load_action{nullptr};

    QStackedWidget* m_pages{nullptr};

    // Landing page
    QPushButton* m_create_session_button{nullptr};
    QPushButton* m_resume_coordinator_button{nullptr};
    QPushButton* m_returned_session_button{nullptr};
    QPushButton* m_join_clipboard_button{nullptr};
    QPushButton* m_join_file_button{nullptr};

    // Draft pages
    QLabel* m_participants_hint{nullptr};
    QLabel* m_settings_title{nullptr};
    QLabel* m_settings_hint{nullptr};
    QLabel* m_review_title{nullptr};
    QLabel* m_review_hint{nullptr};
    QTableWidget* m_share_table{nullptr};
    QPushButton* m_add_share_button{nullptr};
    QPushButton* m_remove_share_button{nullptr};
    QPushButton* m_my_address_button{nullptr};
    QPushButton* m_my_refund_button{nullptr};
    QPushButton* m_my_reward_button{nullptr};
    QComboBox* m_my_share_combo{nullptr};
    QLabel* m_my_share_hint{nullptr};
    QLineEdit* m_my_owner_edit{nullptr};
    QLineEdit* m_my_refund_edit{nullptr};
    QLineEdit* m_my_reward_edit{nullptr};
    QLineEdit* m_service_edit{nullptr};
    OperatorKeyWidget* m_operator_widget{nullptr};
    QLabel* m_operator_field_label{nullptr};
    QLabel* m_operator_secret_label{nullptr};
    QLabel* m_operator_session_key_label{nullptr};
    QLineEdit* m_secret_holder_edit{nullptr};
    QLineEdit* m_voting_edit{nullptr};
    QPushButton* m_my_voting_button{nullptr};
    QDoubleSpinBox* m_operator_reward_spin{nullptr};
    QSpinBox* m_early_period_spin{nullptr};
    QLabel* m_early_period_hint{nullptr};
    BitcoinAmountField* m_early_penalty_field{nullptr};
    QLabel* m_early_penalty_hint{nullptr};
    QListWidget* m_validation_list{nullptr};
    QLabel* m_sum_label{nullptr};
    QGroupBox* m_fee_box{nullptr};
    BitcoinAmountField* m_fee_amount_field{nullptr};
    QComboBox* m_fee_utxo_combo{nullptr};
    QPushButton* m_add_funding_button{nullptr};
    QPushButton* m_remove_funding_button{nullptr};
    QPushButton* m_refresh_funding_button{nullptr};
    QListWidget* m_contrib_list{nullptr};
    QLabel* m_funding_status_label{nullptr};
    QPushButton* m_freeze_button{nullptr};
    QLabel* m_review_summary{nullptr};

    // Frozen/Signing page
    QLabel* m_term_sheet{nullptr};
    QTableWidget* m_sig_table{nullptr};
    QPushButton* m_sign_button{nullptr};
    QPushButton* m_unfreeze_button{nullptr};

    // Combined page
    QLabel* m_combined_status{nullptr};
    QPushButton* m_combine_button{nullptr};
    QListWidget* m_funding_sig_list{nullptr};
    QPushButton* m_sign_funding_button{nullptr};

    // Broadcast page
    QLabel* m_broadcast_summary{nullptr};
    QPushButton* m_broadcast_button{nullptr};
    QLabel* m_success_label{nullptr};

    // Dead page
    QLabel* m_dead_label{nullptr};
};

#endif // BITCOIN_QT_SHAREDMNCREATEDIALOG_H
