// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H
#define BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H

#include <QDialog>
#include <QString>
#include <QVector>

class PlatformService;

QT_BEGIN_NAMESPACE
class QLineEdit;
class QListWidget;
class QTimer;
QT_END_NAMESPACE

/** Search DashPay usernames and send a contact request to the selected user. */
class UsernameSearchDialog : public QDialog
{
    Q_OBJECT

public:
    UsernameSearchDialog(PlatformService& service, QWidget* parent = nullptr);

private Q_SLOTS:
    void onTextChanged();
    void onResults(const QString& prefix, const QVector<QPair<QString, QString>>& results);
    void addSelected();

private:
    PlatformService& m_service;
    QLineEdit* m_input{nullptr};
    QListWidget* m_list{nullptr};
    QTimer* m_debounce{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_USERNAMESEARCHDIALOG_H
