// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
#define BITCOIN_QT_PLATFORM_PLATFORMPAGE_H

#include <QWidget>

#include <memory>

class ClientModel;
class PlatformService;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QStackedWidget;
QT_END_NAMESPACE

/** DashPay (Dash Platform) page: usernames, profiles and contacts for a
 *  single wallet. Only built with --enable-platform-gui.
 *
 *  Shows a welcome/upsell panel when the wallet has no identity, and a
 *  dashboard (username, registration progress, navigation) once registration
 *  has started. The per-wallet PlatformService is created lazily the first
 *  time both models are available and the feature is usable on this network.
 */
class PlatformPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlatformPage(QWidget* parent = nullptr);
    ~PlatformPage() override;

    void setWalletModel(WalletModel* wallet_model);
    void setClientModel(ClientModel* client_model);

private Q_SLOTS:
    void openWizard();
    void refresh();

private:
    void maybeCreateService();

    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};
    std::unique_ptr<PlatformService> m_service;

    QStackedWidget* m_stack{nullptr};
    QLabel* m_welcome{nullptr};
    QLabel* m_dashboard_username{nullptr};
    QLabel* m_dashboard_status{nullptr};
    QPushButton* m_create_button{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
