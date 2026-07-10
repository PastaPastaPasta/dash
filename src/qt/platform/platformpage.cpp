// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformpage.h>

#include <chainparams.h>
#include <platform/params.h>
#include <qt/guiutil.h>

#include <QLabel>
#include <QVBoxLayout>

PlatformPage::PlatformPage(QWidget* parent) :
    QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);

    auto* titleLabel = new QLabel(tr("DashPay"), this);
    GUIUtil::setFont({titleLabel}, GUIUtil::FontWeight::Bold, 20);

    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    if (platform::GetParams(Params().NetworkIDString())) {
        statusLabel->setText(tr("Usernames, profiles and contacts are coming to this wallet. "
                                "This page is under construction."));
    } else {
        statusLabel->setText(tr("Dash Platform is not available on this network."));
    }

    layout->addStretch();
    layout->addWidget(titleLabel, 0, Qt::AlignHCenter);
    layout->addWidget(statusLabel, 0, Qt::AlignHCenter);
    layout->addStretch();

    GUIUtil::updateFonts();
}

PlatformPage::~PlatformPage() = default;

void PlatformPage::setWalletModel(WalletModel* wallet_model)
{
    walletModel = wallet_model;
}

void PlatformPage::setClientModel(ClientModel* client_model)
{
    clientModel = client_model;
}
