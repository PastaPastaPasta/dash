// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/usernameflow.h>

#include <logging.h>
#include <qt/clientmodel.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <util/time.h>

#include <algorithm>
#include <utility>

namespace {
//! The header shows the first of the identity's names.
constexpr uint32_t NAMES_PAGE_LIMIT{10};
} // namespace

UsernameFlow::UsernameFlow(PlatformService& service, IdentityFlow& identity_flow) :
    m_service(service),
    m_identity_flow(identity_flow)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &UsernameFlow::update);
    connect(&m_identity_flow, &IdentityFlow::changed, this, &UsernameFlow::update);
    // A failed read is retried once its wait has passed, at the next block.
    connect(&m_service.clientModel(), &ClientModel::numBlocksChanged, this,
            [this](int, const QDateTime&, const QString&, double, SyncType header, SynchronizationState) {
                if (header == SyncType::BLOCK_SYNC) readNames();
            });
    update();
}

void UsernameFlow::update()
{
    // The identity flow forgets the identity when a gate closes, which is
    // also what drops a write: once the identity is found again, its names
    // say whether a dropped registration landed.
    if (!m_identity_flow.identity()) {
        m_names = Names::UNKNOWN;
        m_username.reset();
        m_names_error.clear();
        m_names_failures = 0;
        m_names_retry_at = 0;
    }
    // A call the client dropped never calls back.
    if (m_step == Step::CHECKING && m_call.expired()) {
        resolve();
    } else if (m_step == Step::REGISTERING && m_call.expired()) {
        finish();
    } else if (m_step == Step::PREPARING && m_identity_flow.step() == IdentityFlow::Step::IDLE) {
        if (m_identity_flow.identity()) {
            send(*m_identity_flow.identity());
        } else {
            // Creating it failed or was declined; the identity flow says why.
            finish();
        }
    }
    readNames();
    Q_EMIT changed();
}

void UsernameFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_unlock.reset();
    m_error = error;
    readNames();
    Q_EMIT changed();
}

void UsernameFlow::readNames()
{
    const auto& identity{m_identity_flow.identity()};
    platform::PlatformClient* client{m_service.client()};
    if (m_step != Step::IDLE || namesRead() || !identity || !m_names_call.expired() || !client) return;
    if (m_names == Names::FAILED && GetTime<std::chrono::seconds>().count() < m_names_retry_at) return;
    client->namesOfIdentity(identity->id, NAMES_PAGE_LIMIT, /*max_pages=*/1,
                            PlatformUi::Tracked<platform::Names>(m_names_call, [this](auto result) {
                                m_username.reset();
                                if (result.ok() && !result.value.names.empty()) {
                                    m_names = Names::FOUND;
                                    m_username = result.value.names.front().label;
                                } else if (result.ok() && result.value.complete) {
                                    m_names = Names::NONE;
                                } else {
                                    // Not "none": a failed or partial read proves nothing.
                                    LogPrintf("DashPay: reading the identity's usernames failed: %s\n",
                                              result.status.message);
                                    m_names = Names::FAILED;
                                    m_names_error = PlatformUi::Describe(result.status);
                                    const std::chrono::seconds wait{std::min<std::chrono::seconds>(
                                        NAMES_RETRY * (int64_t{1} << std::min(m_names_failures, 16)), NAMES_RETRY_MAX)};
                                    ++m_names_failures;
                                    m_names_retry_at = GetTime<std::chrono::seconds>().count() + wait.count();
                                    Q_EMIT changed();
                                    return;
                                }
                                m_names_error.clear();
                                m_names_failures = 0;
                                Q_EMIT changed();
                            }));
}

void UsernameFlow::retryNames()
{
    if (m_names != Names::FAILED) return;
    m_names_retry_at = 0;
    readNames();
}

void UsernameFlow::registerName(const std::string& label)
{
    if (m_step != Step::IDLE || !m_service.client()) return;
    // Not while the identity's names are unknown: one may be registered.
    if (m_identity_flow.identity() && !namesRead()) return;
    if (!platform::IsValidUsername(label)) {
        return finish(tr("A username has 3 to 63 letters, digits and hyphens, starts and ends with a letter or "
                         "digit, and has no two hyphens in a row."));
    }
    // Contested names need a masternode vote; registering them comes later.
    if (platform::IsContestedUsername(label)) {
        return finish(tr("This is a premium username, which a later version of Dash Core will register. Add a digit "
                         "from 2 to 9, or use 20 characters or more."));
    }
    m_label = label;
    m_step = Step::CHECKING;
    m_error.clear();
    resolve();
    Q_EMIT changed();
}

void UsernameFlow::resolve()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return finish();
    client->resolveName(m_label, PlatformUi::Tracked<platform::Identifier>(m_call, [this](auto result) {
                            onResolved(std::move(result));
                        }));
}

void UsernameFlow::onResolved(platform::Result<platform::Identifier> result)
{
    const auto& identity{m_identity_flow.identity()};
    if (result.ok()) {
        if (identity && result.value == identity->id) {
            m_names = Names::FOUND;
            m_username = m_label;
            return finish();
        }
        return finish(tr("“%1” is taken.").arg(QString::fromStdString(m_label)));
    }
    if (result.status.kind != platform::StatusKind::NOT_FOUND) return finish(PlatformUi::Describe(result.status));

    m_unlock.emplace(m_service.walletModel().requestUnlock());
    if (!m_unlock->isValid()) return finish(tr("The wallet stayed locked, so no username was registered."));
    // The passphrase dialog may have let another flow start.
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay payment is on its way. Try again once it is done."));
    }
    m_step = Step::PREPARING;
    if (!m_identity_flow.identity()) m_identity_flow.create();
    update();
}

void UsernameFlow::send(const platform::Identity& identity)
{
    platform::PlatformClient* client{m_service.client()};
    // Paused for a rebuild: changed() comes back here.
    if (!client) return;
    const auto key{PlatformUi::DocumentSigningKey(identity)};
    if (!key) return finish(tr("Your identity has no key DashPay can register a username with."));
    platform::Signing signing{m_service.walletModel().wallet(), IdentityFlow::IDENTITY_INDEX,
                              PlatformUi::KeepUnlocked(std::move(*m_unlock))};
    m_unlock.reset();
    m_step = Step::REGISTERING;
    client->registerName(m_label, identity.id, *key,
                         platform::ContestPolicy{.kind = platform::ContestPolicy::Kind::REFUSE}, std::move(signing),
                         m_service.trackWrite<platform::Identifier>(PlatformUi::Tracked<platform::Identifier>(
                             m_call, [this](auto result) { onRegistered(std::move(result)); })));
}

void UsernameFlow::onRegistered(platform::Result<platform::Identifier> result)
{
    if (!result.ok()) {
        LogPrintf("DashPay: username registration failed: %s\n", result.status.message);
        // It may have landed all the same: the names, read again, say. A
        // contested label was refused before anything was written.
        if (result.status.kind != platform::StatusKind::CONTESTED) m_names = Names::UNKNOWN;
        return finish(PlatformUi::Describe(result.status));
    }
    m_names = Names::FOUND;
    m_username = m_label;
    finish();
}
