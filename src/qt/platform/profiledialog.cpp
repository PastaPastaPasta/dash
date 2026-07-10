// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/profiledialog.h>

#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

ProfileDialog::ProfileDialog(PlatformService& service, QWidget* parent) :
    QDialog(parent),
    m_service(service)
{
    setWindowTitle(tr("Edit DashPay profile"));
    resize(400, 320);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    m_display_name = new QLineEdit(this);
    m_display_name->setMaxLength(25);
    form->addRow(tr("Display name"), m_display_name);

    m_public_message = new QPlainTextEdit(this);
    m_public_message->setFixedHeight(60);
    form->addRow(tr("Public message"), m_public_message);

    m_avatar_url = new QLineEdit(this);
    m_avatar_url->setPlaceholderText(tr("https://…"));
    form->addRow(tr("Avatar URL"), m_avatar_url);
    layout->addLayout(form);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProfileDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(&m_service, &PlatformService::profileUpdated, this, &ProfileDialog::onProfileUpdated);
}

void ProfileDialog::save()
{
    WalletModel::UnlockContext ctx{m_service.walletModel().requestUnlock()};
    if (!ctx.isValid()) return;
    m_status->setText(tr("Publishing profile…"));
    m_service.updateProfile(m_display_name->text().trimmed(),
                            QString::fromStdString(m_public_message->toPlainText().toStdString()),
                            m_avatar_url->text().trimmed());
}

void ProfileDialog::onProfileUpdated(bool ok, const QString& error)
{
    if (ok) {
        accept();
    } else {
        m_status->setText(tr("Failed: %1").arg(error));
    }
}
