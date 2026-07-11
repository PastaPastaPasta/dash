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
    resize(400, 340);
    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(tr("Your profile is public: anyone on Dash Platform can see it. "
                               "It helps contacts recognize you."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout();

    m_display_name = new QLineEdit(this);
    m_display_name->setMaxLength(25);
    m_display_name->setPlaceholderText(tr("e.g. your name or nickname"));
    form->addRow(tr("Display name"), m_display_name);

    m_public_message = new QPlainTextEdit(this);
    m_public_message->setFixedHeight(60);
    form->addRow(tr("Public message"), m_public_message);

    auto* counter = new QLabel(QStringLiteral("0/140"), this);
    counter->setAlignment(Qt::AlignRight);
    form->addRow(QString{}, counter);
    connect(m_public_message, &QPlainTextEdit::textChanged, this, [this, counter] {
        const int length{m_public_message->toPlainText().size()};
        counter->setText(QStringLiteral("%1/140").arg(length));
        counter->setStyleSheet(length > 140 ? QStringLiteral("color: red;") : QString{});
    });

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
    connect(&m_service, &PlatformService::profileLoaded, this,
            [this](const QString&, const QString& display, const QString& message, const QString& avatar) {
        m_display_name->setText(display);
        m_public_message->setPlainText(message);
        m_avatar_url->setText(avatar);
    });
    if (const auto id{m_service.myIdentityId()}) m_service.loadProfile(*id);
}

void ProfileDialog::save()
{
    if (m_public_message->toPlainText().size() > 140) {
        m_status->setText(tr("Public message is limited to 140 characters."));
        return;
    }
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
