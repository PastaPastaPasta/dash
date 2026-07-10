// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactspage.h>

#include <qt/platform/contactsmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/profiledialog.h>
#include <qt/platform/usernamesearchdialog.h>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

ContactsPage::ContactsPage(PlatformService& service, QWidget* parent) :
    QWidget(parent),
    m_service(service)
{
    auto* layout = new QVBoxLayout(this);

    auto* buttons = new QHBoxLayout();
    auto* add = new QPushButton(tr("Find contacts"), this);
    auto* profile = new QPushButton(tr("Edit profile"), this);
    auto* refresh = new QPushButton(tr("Refresh"), this);
    buttons->addWidget(add);
    buttons->addWidget(profile);
    buttons->addStretch();
    buttons->addWidget(refresh);
    layout->addLayout(buttons);

    m_model = new ContactsModel(m_service, this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->verticalHeader()->hide();
    layout->addWidget(m_view);

    connect(add, &QPushButton::clicked, this, &ContactsPage::openSearch);
    connect(profile, &QPushButton::clicked, this, [this] { ProfileDialog(m_service, this).exec(); });
    connect(refresh, &QPushButton::clicked, this, [this] { m_model->refresh(); });

    m_model->refresh();
}

void ContactsPage::openSearch()
{
    UsernameSearchDialog(m_service, this).exec();
    m_model->refresh();
}
