// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformpage.h>

#include <chainparams.h>
#include <platform/client.h>
#include <platform/params.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/platform/createusernamewizard.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>

#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

PlatformPage::PlatformPage(QWidget* parent) :
    QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    m_stack = new QStackedWidget(this);
    outer->addWidget(m_stack);

    // Page 0: welcome / upsell.
    auto* welcome_page = new QWidget(this);
    auto* wl = new QVBoxLayout(welcome_page);
    auto* title = new QLabel(tr("DashPay"), welcome_page);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 20);
    m_welcome = new QLabel(welcome_page);
    m_welcome->setWordWrap(true);
    m_create_button = new QPushButton(tr("Create a username"), welcome_page);
    connect(m_create_button, &QPushButton::clicked, this, &PlatformPage::openWizard);
    wl->addStretch();
    wl->addWidget(title, 0, Qt::AlignHCenter);
    wl->addWidget(m_welcome, 0, Qt::AlignHCenter);
    wl->addWidget(m_create_button, 0, Qt::AlignHCenter);
    wl->addStretch();
    m_stack->addWidget(welcome_page);

    // Page 1: dashboard.
    auto* dash_page = new QWidget(this);
    auto* dl = new QVBoxLayout(dash_page);
    m_dashboard_username = new QLabel(dash_page);
    GUIUtil::setFont({m_dashboard_username}, GUIUtil::FontWeight::Bold, 18);
    m_dashboard_status = new QLabel(dash_page);
    m_dashboard_status->setWordWrap(true);
    dl->addStretch();
    dl->addWidget(m_dashboard_username, 0, Qt::AlignHCenter);
    dl->addWidget(m_dashboard_status, 0, Qt::AlignHCenter);
    dl->addStretch();
    m_stack->addWidget(dash_page);

    if (!platform::GetParams(Params().NetworkIDString())) {
        m_welcome->setText(tr("Dash Platform is not available on this network."));
        m_create_button->setEnabled(false);
    } else {
        m_welcome->setText(tr("Register a unique username, set up a profile, and connect with "
                              "contacts on Dash Platform."));
    }

    GUIUtil::updateFonts();
}

PlatformPage::~PlatformPage() = default;

void PlatformPage::setWalletModel(WalletModel* wallet_model)
{
    walletModel = wallet_model;
    maybeCreateService();
}

void PlatformPage::setClientModel(ClientModel* client_model)
{
    clientModel = client_model;
    maybeCreateService();
}

void PlatformPage::maybeCreateService()
{
    if (m_service || !walletModel || !clientModel) return;
    const auto params{platform::GetParams(Params().NetworkIDString())};
    if (!params) return;

    auto client{platform::MakeGrpcWebPlatformClient(*params)};
    if (!client) return;
    m_service = std::make_unique<PlatformService>(*walletModel, *clientModel, std::move(client), this);
    connect(m_service.get(), &PlatformService::identityStateChanged, this, &PlatformPage::refresh);
    refresh();
}

void PlatformPage::openWizard()
{
    if (!m_service) return;
    CreateUsernameWizard wizard(*m_service, *walletModel, this);
    wizard.exec();
    refresh();
}

void PlatformPage::refresh()
{
    if (!m_service) {
        m_stack->setCurrentIndex(0);
        return;
    }
    using State = IdentityFlow::State;
    const auto& rec{m_service->identityFlow().record()};
    if (rec.state == State::NONE) {
        m_stack->setCurrentIndex(0);
        return;
    }
    m_stack->setCurrentIndex(1);
    if (rec.state == State::REGISTERED) {
        m_dashboard_username->setText(QString::fromStdString(rec.label));
        m_dashboard_status->setText(tr("Your username is registered on Dash Platform."));
    } else if (rec.state == State::CONTESTED_PENDING) {
        m_dashboard_username->setText(QString::fromStdString(rec.label));
        m_dashboard_status->setText(tr("Premium name — a masternode vote is in progress."));
    } else if (rec.state == State::FAILED) {
        m_dashboard_username->setText(tr("Registration failed"));
        m_dashboard_status->setText(QString::fromStdString(rec.last_error));
    } else {
        m_dashboard_username->setText(QString::fromStdString(rec.label));
        m_dashboard_status->setText(tr("Registration in progress…"));
    }
}
