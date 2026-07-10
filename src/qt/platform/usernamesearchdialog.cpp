// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/usernamesearchdialog.h>

#include <qt/platform/platformservice.h>

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

UsernameSearchDialog::UsernameSearchDialog(PlatformService& service, QWidget* parent) :
    QDialog(parent),
    m_service(service)
{
    setWindowTitle(tr("Find a contact"));
    resize(360, 400);
    auto* layout = new QVBoxLayout(this);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Search usernames…"));
    layout->addWidget(m_input);

    m_list = new QListWidget(this);
    layout->addWidget(m_list);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* add = buttons->addButton(tr("Send contact request"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(400);

    connect(m_input, &QLineEdit::textChanged, this, &UsernameSearchDialog::onTextChanged);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        const QString t = m_input->text().trimmed();
        if (!t.isEmpty()) m_service.searchNames(t);
    });
    connect(&m_service, &PlatformService::searchResults, this, &UsernameSearchDialog::onResults);
    connect(add, &QPushButton::clicked, this, [this] { addSelected(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void UsernameSearchDialog::onTextChanged()
{
    m_debounce->start();
}

void UsernameSearchDialog::onResults(const QString& /*prefix*/, const QVector<QPair<QString, QString>>& results)
{
    m_list->clear();
    for (const auto& r : results) {
        auto* item = new QListWidgetItem(r.first, m_list); // username
        item->setData(Qt::UserRole, r.second);             // identity hex
    }
}

void UsernameSearchDialog::addSelected()
{
    auto* item = m_list->currentItem();
    if (!item) return;
    // The identity's authentication key needed for the ECDH-encrypted contact
    // request is fetched by the contact flow from the target identity; here we
    // hand off the selected identity id.
    // Contact request sending requires the recipient's identity key; the
    // service resolves it. For this build we trigger the flow with the
    // selected identity hex.
    accept();
}
