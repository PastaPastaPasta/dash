// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
#define BITCOIN_QT_PLATFORM_CONTACTSPAGE_H

#include <QWidget>

class ContactsModel;
class PlatformService;

QT_BEGIN_NAMESPACE
class QTableView;
QT_END_NAMESPACE

/** Contacts list with buttons to search for and add new contacts. */
class ContactsPage : public QWidget
{
    Q_OBJECT

public:
    ContactsPage(PlatformService& service, QWidget* parent = nullptr);

private Q_SLOTS:
    void openSearch();

private:
    PlatformService& m_service;
    ContactsModel* m_model{nullptr};
    QTableView* m_view{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
