// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H
#define BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H

#include <consensus/amount.h>

#include <QWizard>

class PlatformService;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTimer;
QT_END_NAMESPACE

/**
 * Guided flow for registering a username: name entry with live
 * availability + contested-name warnings, a cost confirmation, wallet
 * unlock, and a live progress view bound to the IdentityFlow state machine.
 * The wizard can be closed at any point after the funding step — the flow
 * continues headless in PlatformService and the dashboard shows progress.
 */
class CreateUsernameWizard : public QWizard
{
    Q_OBJECT

public:
    CreateUsernameWizard(PlatformService& service, WalletModel& wallet_model, QWidget* parent = nullptr);

private:
    PlatformService& m_service;
    WalletModel& m_wallet_model;
};

//! Page 1: username entry + debounced availability check.
class UsernameEntryPage : public QWizardPage
{
    Q_OBJECT

public:
    UsernameEntryPage(PlatformService& service, QWidget* parent = nullptr);
    bool isComplete() const override;
    int nextId() const override;

    QString username() const;
    bool contested() const { return m_contested; }

private Q_SLOTS:
    void onTextChanged();
    void onAvailability(const QString& normalized_label, bool available, bool contested);

private:
    PlatformService& m_service;
    QLineEdit* m_input{nullptr};
    QLabel* m_status{nullptr};
    QTimer* m_debounce{nullptr};
    bool m_available{false};
    bool m_contested{false};
    QString m_checked_normalized;
};

//! Page 2: cost confirmation + wallet unlock trigger.
class UsernameCostPage : public QWizardPage
{
    Q_OBJECT

public:
    UsernameCostPage(PlatformService& service, WalletModel& wallet_model, QWidget* parent = nullptr);
    void initializePage() override;
    bool validatePage() override;

    CAmount fundingAmount() const { return m_funding_amount; }

private:
    PlatformService& m_service;
    WalletModel& m_wallet_model;
    QLabel* m_summary{nullptr};
    QLabel* m_warning{nullptr};
    CAmount m_funding_amount{0};
};

//! Page 3: live progress bound to IdentityFlow.
class UsernameProgressPage : public QWizardPage
{
    Q_OBJECT

public:
    UsernameProgressPage(PlatformService& service, QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;

private Q_SLOTS:
    void refresh();

private:
    PlatformService& m_service;
    QPlainTextEdit* m_log{nullptr};
    QLabel* m_headline{nullptr};
    bool m_done{false};
};

#endif // BITCOIN_QT_PLATFORM_CREATEUSERNAMEWIZARD_H
