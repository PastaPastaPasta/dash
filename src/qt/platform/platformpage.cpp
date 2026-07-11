// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformpage.h>

#include <chainparams.h>
#include <platform/client.h>
#include <platform/params.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platform/contactpickerdialog.h>
#include <qt/platform/contactspage.h>
#include <qt/platform/createusernamewizard.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/profiledialog.h>
#include <qt/platform/usernamesearchdialog.h>
#include <qt/walletmodel.h>
#include <util/strencodings.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int AVATAR_SIZE{56};

void SetMinimumLineHeight(QLabel* label, int lines = 1)
{
    label->setMinimumHeight(label->fontMetrics().lineSpacing() * lines +
                            2 * (label->margin() + label->frameWidth()));
}
} // namespace

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
    m_welcome->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_welcome->setAlignment(Qt::AlignHCenter);
    m_welcome_steps = new QLabel(welcome_page);
    m_welcome_steps->setWordWrap(true);
    m_welcome_steps->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_welcome_steps->setTextFormat(Qt::RichText);
    m_create_button = new QPushButton(tr("Create a username"), welcome_page);
    connect(m_create_button, &QPushButton::clicked, this, &PlatformPage::openWizard);
    wl->addStretch();
    wl->addWidget(title, 0, Qt::AlignHCenter);
    wl->addWidget(m_welcome);
    wl->addWidget(m_welcome_steps, 0, Qt::AlignHCenter);
    wl->addSpacing(8);
    wl->addWidget(m_create_button, 0, Qt::AlignHCenter);
    wl->addStretch();
    m_stack->addWidget(welcome_page);

    // Page 1: dashboard.
    auto* dash_page = new QWidget(this);
    auto* dl = new QVBoxLayout(dash_page);

    // Profile header: avatar | username + profile line | edit button.
    auto* header = new QHBoxLayout();
    m_avatar = new QLabel(dash_page);
    m_avatar->setFixedSize(AVATAR_SIZE, AVATAR_SIZE);
    header->addWidget(m_avatar);
    header->addSpacing(12);
    auto* header_text = new QVBoxLayout();
    m_dashboard_username = new QLabel(dash_page);
    GUIUtil::setFont({m_dashboard_username}, GUIUtil::FontWeight::Bold, 18);
    m_dashboard_username->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_dashboard_profile = new QLabel(dash_page);
    m_dashboard_profile->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_dashboard_profile->setWordWrap(true);
    m_dashboard_balance = new QLabel(dash_page);
    m_dashboard_balance->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_dashboard_balance->setToolTip(tr("Credits pay Dash Platform fees such as profile updates and "
                                       "contact requests. They were funded by your username "
                                       "registration and are separate from your wallet balance."));
    m_dashboard_balance->hide();
    header_text->addStretch();
    header_text->addWidget(m_dashboard_username);
    header_text->addWidget(m_dashboard_profile);
    header_text->addWidget(m_dashboard_balance);
    header_text->addStretch();
    header->addLayout(header_text, /*stretch=*/1);
    auto* edit_profile = new QPushButton(tr("Edit profile"), dash_page);
    edit_profile->setToolTip(tr("Set the display name, message and picture other DashPay users see"));
    connect(edit_profile, &QPushButton::clicked, this, [this] {
        if (m_service) { ProfileDialog(*m_service, this).exec(); refresh(); }
    });
    header->addWidget(edit_profile, 0, Qt::AlignTop);
    dl->addLayout(header);

    // Registration progress (hidden once registered).
    auto* status_row = new QHBoxLayout();
    m_dashboard_status = new QLabel(dash_page);
    m_dashboard_status->setWordWrap(true);
    m_dashboard_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    status_row->addWidget(m_dashboard_status, /*stretch=*/1);
    m_progress_button = new QPushButton(tr("View progress…"), dash_page);
    connect(m_progress_button, &QPushButton::clicked, this, &PlatformPage::openWizard);
    status_row->addWidget(m_progress_button);
    dl->addLayout(status_row);

    // Attention banner for pending incoming contact requests.
    m_pending_banner = new QLabel(dash_page);
    m_pending_banner->setWordWrap(true);
    m_pending_banner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_pending_banner->hide();
    dl->addWidget(m_pending_banner);

    // Quick actions.
    auto* actions = new QHBoxLayout();
    m_send_button = new QPushButton(tr("Send Dash to a contact"), dash_page);
    m_send_button->setToolTip(tr("Pay one of your contacts by username — no address needed"));
    connect(m_send_button, &QPushButton::clicked, this, &PlatformPage::openContactPicker);
    auto* find_button = new QPushButton(tr("Find people"), dash_page);
    find_button->setToolTip(tr("Search for other DashPay users by username and send contact requests"));
    connect(find_button, &QPushButton::clicked, this, [this] {
        if (m_service) { UsernameSearchDialog(*m_service, this).exec(); m_service->refreshContacts(); }
    });
    actions->addWidget(m_send_button);
    actions->addWidget(find_button);
    actions->addStretch();
    dl->addLayout(actions);

    m_stack->addWidget(dash_page);
    m_dashboard_layout = dl;

    if (!platform::GetParams(Params().NetworkIDString())) {
        m_welcome->setText(tr("Dash Platform is not available on this network."));
        m_welcome_steps->hide();
        m_create_button->setEnabled(false);
    } else {
        m_welcome->setText(tr("DashPay lets you send and receive Dash using names instead of addresses."));
        m_welcome_steps->setText(
            "<ol style='margin:0'>"
            "<li>" + tr("Register a unique username on Dash Platform.").toHtmlEscaped() + "</li>"
            "<li>" + tr("Connect with people you know by exchanging contact requests.").toHtmlEscaped() + "</li>"
            "<li>" + tr("Pay contacts by username — every payment goes to a fresh address only "
                        "the two of you can link.").toHtmlEscaped() + "</li>"
            "</ol>"
            "<p>" + tr("Registration makes a small payment from this wallet to fund your identity.").toHtmlEscaped() + "</p>");
    }

    GUIUtil::updateFonts();
    SetMinimumLineHeight(m_welcome);
    SetMinimumLineHeight(m_dashboard_username);
    SetMinimumLineHeight(m_dashboard_profile);
    SetMinimumLineHeight(m_dashboard_status);
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
    Q_EMIT platformServiceReady(m_service.get());
    connect(m_service.get(), &PlatformService::identityStateChanged, this, &PlatformPage::refresh);
    connect(m_service.get(), &PlatformService::contestedNameState, this,
            [this](const QString& normalized_label, const platform::ContestedNameState& state,
                   const QString& error) {
                using State = IdentityFlow::State;
                if (!m_service || m_service->identityFlow().record().state != State::CONTESTED_PENDING) return;
                if (QString::fromStdString(m_service->identityFlow().record().normalized_label) !=
                    normalized_label) return;
                if (!error.isEmpty() ||
                    state.status != platform::ContestedNameState::Status::CONTEST_IN_PROGRESS) return;
                uint32_t my_votes{0}, best_other{0};
                const auto my_id{m_service->identityFlow().record().identity_id};
                for (const auto& [identity, votes] : state.contenders) {
                    if (identity == my_id) my_votes = votes;
                    else best_other = std::max(best_other, votes);
                }
                m_dashboard_status->setText(
                    tr("Masternodes are voting — you: %1, best rival: %2, abstain: %3, lock: %4. "
                       "The name may still be awarded to someone else.")
                        .arg(my_votes).arg(best_other).arg(state.abstain_votes).arg(state.lock_votes));
            });
    connect(m_service.get(), &PlatformService::profileLoaded, this,
            [this](const QString& identity_hex, const QString& display_name,
                   const QString& public_message, const QString& /*avatar_url*/) {
        const auto my_id{m_service->myIdentityId()};
        if (!my_id || QString::fromStdString(HexStr(*my_id)) != identity_hex) return;
        QStringList parts;
        if (!display_name.isEmpty()) parts << display_name;
        if (!public_message.isEmpty()) parts << public_message;
        m_dashboard_profile->setText(parts.join(QStringLiteral(" — ")));
        if (m_dashboard_profile->text().isEmpty()) {
            m_dashboard_profile->setText(tr("No profile yet — add a display name so contacts recognize you."));
        }
    });
    connect(m_service.get(), &PlatformService::profileLoadFailed, this,
            [this](const QString& identity_hex, const QString& /*error*/) {
        const auto my_id{m_service->myIdentityId()};
        if (!my_id || QString::fromStdString(HexStr(*my_id)) != identity_hex) return;
        // Keep any previously loaded profile text; only replace the
        // loading placeholder so the header never sticks on "Loading…".
        if (m_dashboard_profile->text() == tr("Loading profile…")) {
            m_dashboard_profile->setText(tr("Profile could not be loaded — will retry."));
        }
        QTimer::singleShot(30'000, this, [this] {
            if (!m_service) return;
            if (const auto id{m_service->myIdentityId()}) m_service->loadProfile(*id);
        });
    });
    connect(m_service.get(), &PlatformService::identityBalanceLoaded, this,
            [this](quint64 credits) {
        // 1 duff carries 1000 platform credits.
        const CAmount duffs{static_cast<CAmount>(credits / 1000)};
        const auto unit{walletModel->getOptionsModel()->getDisplayUnit()};
        m_dashboard_balance->setText(tr("Platform balance: %1 credits (≈%2)")
                                         .arg(QString::number(credits),
                                              BitcoinUnits::formatWithUnit(unit, duffs)));
        m_dashboard_balance->show();
    });
    connect(m_service.get(), &PlatformService::contactsUpdated, this,
            [this](const QVector<QPair<QString, QString>>& incoming,
                   const QVector<QPair<QString, QString>>& /*outgoing*/) {
        // Incoming requests we have not accepted yet (no imported friendship key).
        int pending{0};
        for (const auto& in : incoming) {
            if (m_service->readRecord("contact/key/" + in.first.toStdString()).size() != 65) ++pending;
        }
        if (pending > 0) {
            m_pending_banner->setStyleSheet(QString{"QLabel { color: %1; }"}
                .arg(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::ORANGE).name()));
            m_pending_banner->setText(tr("%n person(s) want(s) to connect with you — select the request "
                                         "in the list below and choose Accept.", "", pending));
        }
        m_pending_banner->setVisible(pending > 0);
    });

    // Embed the contacts UI in the dashboard once the service exists.
    m_contacts_page = new ContactsPage(*m_service, this);
    connect(m_contacts_page, &ContactsPage::sendToContact, this, &PlatformPage::sendToContact);
    if (m_dashboard_layout) m_dashboard_layout->addWidget(m_contacts_page);

    refresh();
}

void PlatformPage::openWizard()
{
    if (!m_service) return;
    using State = IdentityFlow::State;
    // A failed registration is cleared first so the wizard restarts from
    // name entry instead of dead-ending on the progress view.
    if (m_service->identityFlow().record().state == State::FAILED) {
        m_service->identityFlow().reset();
    }
    CreateUsernameWizard wizard(*m_service, *walletModel, this);
    const auto state{m_service->identityFlow().record().state};
    if (state != State::NONE && state != State::REGISTERED) {
        // A registration is already underway: jump straight to the progress
        // view instead of asking for a name again.
        wizard.startAtProgress();
    }
    wizard.exec();
    refresh();
}

void PlatformPage::openContactPicker()
{
    if (!m_service) return;
    ContactPickerDialog dlg(*m_service, this);
    if (dlg.exec() && !dlg.selectedUsername().isEmpty()) {
        Q_EMIT sendToContact(dlg.selectedUsername());
    }
}

void PlatformPage::updateAvatar(const QString& name)
{
    const QString source{name.isEmpty() ? QStringLiteral("?") : name};
    // Deterministic per-name hue so each user's placeholder avatar is stable.
    uint hash{0};
    for (const QChar ch : source) hash = hash * 31 + ch.unicode();
    QColor fill;
    fill.setHsv(static_cast<int>(hash % 360), 160, 180);

    const qreal dpr{devicePixelRatioF()};
    QPixmap pixmap{qRound(AVATAR_SIZE * dpr), qRound(AVATAR_SIZE * dpr)};
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(fill);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(0, 0, AVATAR_SIZE, AVATAR_SIZE);
    painter.setPen(Qt::white);
    QFont font{m_avatar->font()};
    font.setBold(true);
    font.setPixelSize(AVATAR_SIZE / 2);
    painter.setFont(font);
    painter.drawText(QRect{0, 0, AVATAR_SIZE, AVATAR_SIZE}, Qt::AlignCenter, source.left(1).toUpper());
    m_avatar->setPixmap(pixmap);
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
    const QString label{QString::fromStdString(rec.label)};
    updateAvatar(label);
    m_dashboard_username->setText(label);
    if (rec.state == State::REGISTERED) {
        m_dashboard_status->hide();
        m_progress_button->hide();
        m_send_button->setEnabled(true);
        if (m_dashboard_profile->text().isEmpty()) {
            m_dashboard_profile->setText(tr("Loading profile…"));
        }
        if (const auto id{m_service->myIdentityId()}) m_service->loadProfile(*id);
        m_service->refreshIdentityBalance();
        m_service->refreshContacts();
    } else {
        m_dashboard_status->show();
        m_send_button->setEnabled(false);
        m_progress_button->setVisible(rec.state != State::CONTESTED_PENDING);
        m_progress_button->setText(rec.state == State::FAILED ? tr("Try again")
                                                              : tr("View progress…"));
        if (rec.state == State::CONTESTED_PENDING) {
            m_dashboard_profile->setText(tr("Premium name — masternodes are voting on your request."));
            m_dashboard_status->setText(tr("The vote can take a while and the name may be awarded to "
                                           "someone else. You will see the result here."));
            m_service->checkContestedNameState(QString::fromStdString(rec.normalized_label));
        } else if (rec.state == State::FAILED) {
            m_dashboard_profile->setText(tr("Registration failed"));
            m_dashboard_status->setText(QString::fromStdString(rec.last_error));
        } else {
            m_dashboard_profile->setText(tr("Registration in progress — contacts unlock once it completes."));
            m_dashboard_status->setText(tr("Registering your username usually takes a few minutes. "
                                           "You can keep using the wallet in the meantime."));
        }
    }
}
