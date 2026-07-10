// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/createusernamewizard.h>

#include <platform/statetransitions.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>

#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int PAGE_ENTRY{0};
constexpr int PAGE_COST{1};
constexpr int PAGE_PROGRESS{2};
constexpr int DEBOUNCE_MS{400};
} // namespace

// ---- Wizard -----------------------------------------------------------------

CreateUsernameWizard::CreateUsernameWizard(PlatformService& service, WalletModel& wallet_model, QWidget* parent) :
    QWizard(parent),
    m_service(service),
    m_wallet_model(wallet_model)
{
    setWindowTitle(tr("Register a DashPay username"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage, true);

    setPage(PAGE_ENTRY, new UsernameEntryPage(service, this));
    setPage(PAGE_COST, new UsernameCostPage(service, wallet_model, this));
    setPage(PAGE_PROGRESS, new UsernameProgressPage(service, this));
}

// ---- Entry page -------------------------------------------------------------

UsernameEntryPage::UsernameEntryPage(PlatformService& service, QWidget* parent) :
    QWizardPage(parent),
    m_service(service)
{
    setTitle(tr("Choose your username"));
    setSubTitle(tr("Your username is registered on Dash Platform and is unique across the network."));

    auto* layout = new QVBoxLayout(this);
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("username"));
    // DPNS labels: 3-63 chars, letters/digits/hyphens, no leading/trailing hyphen.
    m_input->setValidator(new QRegularExpressionValidator(
        QRegularExpression("^[a-zA-Z0-9][a-zA-Z0-9-]{1,61}[a-zA-Z0-9]$"), this));
    layout->addWidget(m_input);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(DEBOUNCE_MS);

    connect(m_input, &QLineEdit::textChanged, this, &UsernameEntryPage::onTextChanged);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        const QString name{m_input->text().trimmed()};
        if (!name.isEmpty()) m_service.checkNameAvailability(name);
    });
    connect(&m_service, &PlatformService::nameAvailability, this, &UsernameEntryPage::onAvailability);
}

void UsernameEntryPage::onTextChanged()
{
    m_available = false;
    m_checked_normalized.clear();
    m_status->setText(tr("Checking availability…"));
    Q_EMIT completeChanged();
    m_debounce->start();
}

void UsernameEntryPage::onAvailability(const QString& normalized_label, bool available, bool contested)
{
    const std::string typed_norm{platform::st::NormalizeLabel(m_input->text().trimmed().toStdString())};
    if (QString::fromStdString(typed_norm) != normalized_label) return; // stale

    m_available = available;
    m_contested = contested;
    m_checked_normalized = normalized_label;
    if (!available) {
        m_status->setText(tr("\"%1\" is already taken.").arg(normalized_label));
    } else if (contested) {
        m_status->setText(tr("\"%1\" is a premium name. Registration triggers a masternode vote "
                             "that can take weeks and may be awarded to someone else.").arg(normalized_label));
    } else {
        m_status->setText(tr("\"%1\" is available.").arg(normalized_label));
    }
    Q_EMIT completeChanged();
}

bool UsernameEntryPage::isComplete() const
{
    return m_available && !m_checked_normalized.isEmpty();
}

int UsernameEntryPage::nextId() const { return PAGE_COST; }

QString UsernameEntryPage::username() const { return m_input->text().trimmed(); }

// ---- Cost page --------------------------------------------------------------

UsernameCostPage::UsernameCostPage(PlatformService& service, WalletModel& wallet_model, QWidget* parent) :
    QWizardPage(parent),
    m_service(service),
    m_wallet_model(wallet_model)
{
    setTitle(tr("Confirm registration"));
    auto* layout = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setStyleSheet("color:#c0392b;");
    layout->addWidget(m_warning);
}

void UsernameCostPage::initializePage()
{
    m_funding_amount = m_service.params().default_identity_funding_amount;
    const auto unit{m_wallet_model.getOptionsModel()->getDisplayUnit()};
    m_summary->setText(tr("Registering your username funds a Dash Platform identity with %1. "
                          "This creates an on-chain asset lock transaction and several Platform "
                          "state transitions.")
                           .arg(BitcoinUnits::formatWithUnit(unit, m_funding_amount)));

    auto* entry = qobject_cast<UsernameEntryPage*>(wizard()->page(PAGE_ENTRY));
    m_warning->setVisible(entry && entry->contested());
    if (entry && entry->contested()) {
        m_warning->setText(tr("This is a premium (contested) name: masternodes will vote on who "
                              "receives it. The process can take weeks and you may not win it."));
    }
}

bool UsernameCostPage::validatePage()
{
    auto* entry = qobject_cast<UsernameEntryPage*>(wizard()->page(PAGE_ENTRY));
    if (!entry) return false;

    WalletModel::UnlockContext ctx{m_wallet_model.requestUnlock()};
    if (!ctx.isValid()) return false;

    QString error;
    if (!m_service.identityFlow().start(entry->username(), m_funding_amount, error)) {
        m_warning->setVisible(true);
        m_warning->setText(error);
        return false;
    }
    return true;
}

// ---- Progress page ----------------------------------------------------------

UsernameProgressPage::UsernameProgressPage(PlatformService& service, QWidget* parent) :
    QWizardPage(parent),
    m_service(service)
{
    setTitle(tr("Registering…"));
    setSubTitle(tr("You can close this window; registration continues in the background."));
    auto* layout = new QVBoxLayout(this);
    m_headline = new QLabel(this);
    m_headline->setWordWrap(true);
    layout->addWidget(m_headline);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    layout->addWidget(m_log);

    connect(&m_service, &PlatformService::identityStateChanged, this, &UsernameProgressPage::refresh);
    connect(&m_service, &PlatformService::flowFailed, this, [this](const QString& step, const QString& err) {
        m_log->appendPlainText(tr("[%1] error: %2").arg(step, err));
    });
}

void UsernameProgressPage::initializePage() { refresh(); }

void UsernameProgressPage::refresh()
{
    using State = IdentityFlow::State;
    const auto& rec{m_service.identityFlow().record()};
    QString label;
    switch (rec.state) {
    case State::FUNDING_SENT:        label = tr("Waiting for the funding transaction to lock…"); break;
    case State::FUNDING_LOCKED:      label = tr("Funding locked. Creating your identity…"); break;
    case State::IDENTITY_BROADCAST:  label = tr("Identity broadcast. Waiting for confirmation…"); break;
    case State::IDENTITY_CONFIRMED:  label = tr("Identity created. Reserving your name…"); break;
    case State::PREORDER_BROADCAST:
    case State::PREORDER_WAIT:        label = tr("Name reserved. Registering the domain…"); break;
    case State::DOMAIN_BROADCAST:    label = tr("Domain broadcast. Confirming…"); break;
    case State::CONTESTED_PENDING:   label = tr("Your premium name is now subject to a masternode vote."); break;
    case State::REGISTERED:          label = tr("Done! Your username \"%1\" is registered.").arg(QString::fromStdString(rec.label)); break;
    case State::FAILED:              label = tr("Registration failed: %1").arg(QString::fromStdString(rec.last_error)); break;
    case State::NONE:                label = tr("Starting…"); break;
    }
    m_headline->setText(label);
    m_log->appendPlainText(label);

    const bool done{rec.state == State::REGISTERED || rec.state == State::FAILED ||
                    rec.state == State::CONTESTED_PENDING};
    if (done != m_done) {
        m_done = done;
        Q_EMIT completeChanged();
    }
}

bool UsernameProgressPage::isComplete() const { return m_done; }
