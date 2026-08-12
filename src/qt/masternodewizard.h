// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODEWIZARD_H
#define BITCOIN_QT_MASTERNODEWIZARD_H

#include <consensus/amount.h>
#include <qt/protxsender.h>

#include <QDialog>
#include <QString>
#include <QVector>

#include <memory>

class FeeSourcePicker;
class OperatorKeyWidget;
class QValidatedLineEdit;
class WalletModel;

namespace interfaces {
class Node;
} // namespace interfaces

QT_BEGIN_NAMESPACE
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QVBoxLayout;
QT_END_NAMESPACE

//! Multi-page dialog registering a masternode or evonode: collects collateral,
//! service, key, payout and fee-source details, then issues the matching
//! "protx register_*" RPC through ProTxSender. Supports funding the collateral
//! from the wallet, referencing an exact-denomination UTXO already in the
//! wallet, and the external-collateral flow (register_prepare, out-of-band
//! signMessage signature, register_submit).
class RegisterMasternodeWizard : public QDialog
{
    Q_OBJECT

public:
    explicit RegisterMasternodeWizard(interfaces::Node& node, WalletModel* walletModel, QWidget* parent = nullptr);
    ~RegisterMasternodeWizard() override;

public Q_SLOTS:
    void reject() override;

private Q_SLOTS:
    void onNext();
    void onBack();
    void onRpcFinished(const ProTxResult& result);

private:
    enum Page : int {
        PageType = 0,
        PageCollateral,
        PageService,
        PageKeys,
        PagePayout,
        PagePlatform,
        PageFee,
        PageReview,
        PageSign,
        PageResult,
    };
    //! Which RPC the in-flight ProTxSender call belongs to
    enum class Stage {
        None,
        Register, //!< register_fund[_evo] / register[_evo], returns a txid
        Prepare,  //!< register_prepare[_evo], returns {tx, collateralAddress, signMessage}
        Submit,   //!< register_submit, returns a txid
    };

    QWidget* createTypePage();
    QWidget* createCollateralPage();
    QWidget* createServicePage();
    QWidget* createKeysPage();
    QWidget* createPayoutPage();
    QWidget* createPlatformPage();
    QWidget* createFeePage();
    QWidget* createReviewPage();
    QWidget* createSignPage();
    QWidget* createResultPage();

    bool isEvo() const;
    bool isExternalCollateral() const;
    bool isFundCollateral() const;
    QString collateralAddress() const;
    QString ownerAddress() const;
    QString votingAddress() const;
    QString freshAddress(QString& err) const;
    CAmount collateralAmount() const;
    //! True when the user typed the generated operator secret's last 4 characters
    bool secretConfirmed() const;
    //! True while the generated operator secret exists nowhere but this dialog,
    //! which is the only case worth gating the last page on
    bool secretGateRequired() const;

    void rebuildOrder();
    void goToPage(Page page);
    void enterPage(Page page);
    bool validatePage(Page page, QString& err);
    void updateButtons();
    void setBusy(bool busy, const QString& busy_text = QString());
    void showError(const QString& message);

    void refreshCollateralCandidates();
    //! Store the generated operator secret in the wallet when that mode is
    //! selected. Call while the wallet is still unlocked by the registration.
    void saveOperatorKeyIfChosen();
    void populateReview();
    //! Width of the review's label column, shared by every section
    int reviewLabelWidth(const QWidget* card) const;
    //! One line saying where the operator key comes from and where it will live
    QString operatorKeyProvenance() const;
    void populateResult(const QString& pro_tx_hash);
    UniValue buildRegisterParams() const;
    void startRegistration();
    void startSubmit();

    interfaces::Node& m_node;
    WalletModel* const m_walletModel;
    ProTxSender m_sender;
    Stage m_stage{Stage::None};
    struct UnlockHolder;
    std::unique_ptr<UnlockHolder> m_unlock;

    QVector<Page> m_order;
    int m_pos{0};
    bool m_busy{false};
    bool m_registered{false};
    //! Whether addMasternodeOperatorKey() stored the generated secret
    bool m_operator_key_saved{false};
    //! Why storing the generated secret failed, empty when it did not fail
    QString m_operator_save_error;
    //! Why storing the generated secret failed; empty when it was not attempted
    QString m_operator_store_error;

    QStackedWidget* m_pages;
    QLabel* m_error_label;
    QPushButton* m_back_button;
    QPushButton* m_next_button;
    QPushButton* m_cancel_button;

    // Type page
    QRadioButton* m_type_regular;
    QRadioButton* m_type_evo;
    // Collateral page
    QRadioButton* m_col_fund;
    QRadioButton* m_col_wallet;
    QRadioButton* m_col_external;
    QWidget* m_col_fund_box;
    QWidget* m_col_wallet_box;
    QWidget* m_col_external_box;
    QValidatedLineEdit* m_col_address;
    QComboBox* m_col_utxo_combo;
    QLabel* m_col_utxo_none;
    QLineEdit* m_col_txid;
    QSpinBox* m_col_vout;
    // Service page
    QLineEdit* m_service_edit;
    // Keys page
    QValidatedLineEdit* m_owner_edit;
    QValidatedLineEdit* m_voting_edit;
    OperatorKeyWidget* m_operator_widget;
    // Payout page
    QValidatedLineEdit* m_payout_edit;
    QDoubleSpinBox* m_operator_reward;
    QLabel* m_reward_warning;
    // Platform page. Pre-v24 ProTxs only store bare platform ports; the
    // ADDR:PORT list form requires a version 3 ProTx (v24 active).
    QLineEdit* m_platform_nodeid;
    QWidget* m_platform_addr_box;
    QLineEdit* m_platform_p2p;
    QLineEdit* m_platform_https;
    QWidget* m_platform_port_box;
    QSpinBox* m_platform_p2p_port;
    QSpinBox* m_platform_https_port;
    // Fee page
    FeeSourcePicker* m_fee_picker;
    QLabel* m_fee_explain;
    // Review page. The sections are rebuilt on every entry, so the layout owns
    // the rows rather than a single rich-text label.
    QWidget* m_review_container;
    QVBoxLayout* m_review_layout;
    // Sign page (external collateral)
    QLabel* m_sign_address_label;
    QPlainTextEdit* m_sign_message;
    QPlainTextEdit* m_sig_edit;
    QString m_prepared_tx;
    // Result page
    QLabel* m_result_label;
    QLabel* m_result_hash;
    QLabel* m_result_tx_note;
    QWidget* m_secret_box;
    QLabel* m_secret_note;
    QLineEdit* m_secret_edit;
    QLineEdit* m_conf_line_edit;
    QWidget* m_confirm_box;
    QLineEdit* m_confirm_edit;
    QLabel* m_next_steps;
};

#endif // BITCOIN_QT_MASTERNODEWIZARD_H
