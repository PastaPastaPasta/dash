// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
#define BITCOIN_QT_PLATFORM_PLATFORMPAGE_H

#include <QWidget>

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
QT_END_NAMESPACE

/** DashPay (Dash Platform) page: usernames, profiles and contacts for a
 *  single wallet. Only built with --enable-platform-gui.
 */
class PlatformPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlatformPage(QWidget* parent = nullptr);
    ~PlatformPage() override;

    void setWalletModel(WalletModel* wallet_model);
    void setClientModel(ClientModel* client_model);

private:
    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};

    QLabel* statusLabel{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
