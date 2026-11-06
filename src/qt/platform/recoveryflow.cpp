// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/recoveryflow.h>

#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <platform/types.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/platform/usernameflow.h>
#include <wallet/platformtypes.h>

#include <utility>
#include <variant>
#include <vector>

namespace {
bool Locked(const WalletModel& wallet_model)
{
    const auto status{wallet_model.getEncryptionStatus()};
    return status == WalletModel::Locked || status == WalletModel::UnlockedForMixingOnly;
}
} // namespace

RecoveryFlow::RecoveryFlow(PlatformService& service, IdentityFlow& identity_flow, UsernameFlow& username_flow,
                           ContactFlow& contact_flow) :
    m_service(service),
    m_identity_flow(identity_flow),
    m_username_flow(username_flow),
    m_contact_flow(contact_flow)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &RecoveryFlow::update);
    connect(&m_identity_flow, &IdentityFlow::changed, this, &RecoveryFlow::update);
    connect(&m_username_flow, &UsernameFlow::changed, this, &RecoveryFlow::update);
    connect(&m_contact_flow, &ContactFlow::changed, this, &RecoveryFlow::update);
    update();
}

void RecoveryFlow::find()
{
    if (m_step != Step::IDLE) return;
    // Keeps other writes out while the passphrase dialog runs its event loop.
    m_step = Step::FINDING;
    m_ran = true;
    m_error.clear();
    Q_EMIT changed();
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) return finish(tr("The wallet stayed locked, so DashPay could not look for your identity."));
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay request is on its way. Try again once it is done."));
    }
    if (!m_service.consented() && !m_service.start()) {
        return finish(tr("The wallet could not save that DashPay is started."));
    }
    if (m_service.gate() != PlatformService::Gate::NONE) {
        return finish(tr("DashPay can't reach Dash Platform now, so it did not look for your identity. Find it "
                         "again once DashPay is on."));
    }
    // Held until our requests are read, so their chains are imported under it.
    m_unlock.emplace(std::move(unlock));
    // Every part again, those that failed included: the lookup also lists
    // the wallet's funding payments, which needs the passphrase.
    m_identity_flow.lookUp();
    m_username_flow.retryNames();
    if (m_pending == Pending::FAILED) m_pending = Pending::UNKNOWN;
    m_contact_flow.refresh();
    if (m_contact_flow.rescanFailed()) m_contact_flow.rescanOwed();
    update();
}

void RecoveryFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_unlock.reset();
    m_error = error;
    Q_EMIT changed();
}

void RecoveryFlow::update()
{
    if (!m_identity_flow.identity()) {
        m_pending = Pending::UNKNOWN;
        m_pending_name.reset();
        m_pending_error.clear();
    }
    if (m_ran) {
        for (const IdentityFlow::Found& found : m_identity_flow.found()) {
            m_found_seen.insert(found.funding.outpoint());
        }
    }
    // A rescan pass ended: whether what the wallet still owes needs blocks
    // this node does not have.
    const bool rescanning{m_contact_flow.rescanning()};
    if (std::exchange(m_rescanning, rescanning) && !rescanning) {
        interfaces::Wallet& wallet{m_service.walletModel().wallet()};
        const auto from{wallet.owedRescanFrom()};
        m_missing_from = from ? wallet.rescanUnavailableFrom(*from) : std::nullopt;
    }
    readPending();
    advance();
    Q_EMIT changed();
}

void RecoveryFlow::advance()
{
    // Nothing is decided while the passphrase dialog runs its event loop.
    if (m_step != Step::FINDING || !m_unlock) return;
    if (m_service.gate() != PlatformService::Gate::NONE) {
        return finish(tr("DashPay stopped before your identity and contacts were read. Find them again once it is "
                         "on."));
    }
    if (m_identity_flow.step() == IdentityFlow::Step::LOOKING_UP) return;
    // Neither looked up nor failed: the lookup waits for a client.
    if (!m_identity_flow.lookedUp() && m_identity_flow.error().isEmpty()) return;
    // Read, our requests' chains are imported at once, under this unlock.
    if (m_identity_flow.identity() && !m_contact_flow.sentRead() && m_contact_flow.sentError().isEmpty()) return;
    finish();
}

void RecoveryFlow::readPending()
{
    const auto& identity{m_identity_flow.identity()};
    platform::PlatformClient* client{m_service.client()};
    // Only an identity proved to have no username can be waiting for one.
    if (m_pending != Pending::UNKNOWN || !identity || !client || !m_pending_call.expired() ||
        m_username_flow.names() != UsernameFlow::Names::NONE) {
        return;
    }
    const auto read = [this](platform::Result<std::vector<platform::Contest>> result) {
        if (!result.ok()) {
            // Unknown, not "none": Register waits for it.
            LogPrintf("DashPay: reading the identity's pending usernames failed: %s\n", result.status.message);
            m_pending = Pending::FAILED;
            m_pending_error = PlatformUi::Describe(result.status);
        } else if (result.value.empty()) {
            m_pending = Pending::NONE;
        } else {
            m_pending = Pending::FOUND;
            m_pending_name = result.value.front().normalized_label;
        }
        update();
    };
    client->pendingContests(identity->id, PlatformUi::Tracked<std::vector<platform::Contest>>(m_pending_call, read));
}

bool RecoveryFlow::incomplete() const
{
    // Until the identity is looked up, Find my identity is what is offered.
    if (!m_identity_flow.lookedUp()) return false;
    if (m_ran && !m_identity_flow.listed()) return true;
    for (const IdentityFlow::Found& found : m_identity_flow.found()) {
        if (found.read_failed) return true;
    }
    if (!m_identity_flow.identity()) return false;
    return m_pending == Pending::FAILED || m_username_flow.names() == UsernameFlow::Names::FAILED ||
           !m_contact_flow.sentError().isEmpty() || m_contact_flow.chainsNotImported() > 0 ||
           m_contact_flow.rescanFailed();
}

QStringList RecoveryFlow::report() const
{
    QStringList lines;
    if (!m_error.isEmpty()) lines << m_error;
    const auto& identity{m_identity_flow.identity()};
    if (m_ran && m_step == Step::IDLE && m_identity_flow.lookedUp() && !identity) {
        lines << tr("No DashPay identity was found for this wallet. Dash Core looks for the first identity a "
                    "wallet's seed registers (identity index 0), the one the mobile wallets create.");
    }
    if (identity) {
        if (m_pending == Pending::FAILED) {
            lines << tr("DashPay could not read whether a username of yours is waiting for its masternode vote, so "
                        "it does not offer to register one yet: %1")
                         .arg(m_pending_error);
        }
        const size_t missing{m_contact_flow.chainsNotImported()};
        if (!m_contact_flow.sentError().isEmpty()) {
            lines << tr("DashPay could not read the contact requests you sent, so payments from those contacts may "
                        "be missing: %1")
                         .arg(m_contact_flow.sentError());
        } else if (!m_contact_flow.sentRead()) {
            if (m_ran) lines << tr("Reading the contact requests you sent…");
        } else if (missing > 0 && Locked(m_service.walletModel())) {
            lines << tr("%n contact(s) you sent requests to can pay you at addresses that are not in the wallet yet. "
                        "Unlock the wallet to add them.",
                        nullptr, int(missing));
        } else if (missing > 0) {
            lines << tr("Dash Core could not add the addresses %n contact(s) you sent requests to pay you at.", nullptr,
                        int(missing));
        } else if (m_ran && m_contact_flow.chainsImported() > 0) {
            lines << tr("The addresses %n contact(s) you sent requests to pay you at are in the wallet.", nullptr,
                        int(m_contact_flow.chainsImported()));
        }
        if (m_contact_flow.rescanning()) {
            lines << tr("Rescanning the wallet for payments from your DashPay contacts…");
        } else if (m_contact_flow.rescanFailed()) {
            lines << tr("The wallet rescan for payments from your DashPay contacts did not finish. DashPay runs it "
                        "again later, or now with Try again.");
        }
    }
    if (m_missing_from) {
        lines << tr("Payments from your DashPay contacts in blocks from %1 on are found once this node has those "
                    "blocks again. A pruned node needs pruning turned off and a restart with -reindex.")
                     .arg(*m_missing_from);
    }
    if (m_ran && m_identity_flow.lookedUp() && !m_identity_flow.listed()) {
        lines << tr("DashPay could not list the funding payments in this wallet, so earlier ones are not offered.");
    } else if (m_ran && m_identity_flow.lookedUp()) {
        int unused{0}, checking{0}, abandoned{0}, used{0};
        bool registration{false};
        for (const IdentityFlow::Found& found : m_identity_flow.found()) {
            if (found.inactive) {
                ++abandoned;
            } else if (found.state) {
                ++unused;
                registration |= std::holds_alternative<wallet::RegistrationFundingKey>(found.funding.key);
            } else {
                ++checking;
            }
        }
        for (const COutPoint& outpoint : m_found_seen) {
            used += m_identity_flow.used(outpoint);
        }
        QStringList parts;
        if (unused) parts << tr("%n not used on Dash Platform yet", nullptr, unused);
        if (used) parts << tr("%n already used on Dash Platform", nullptr, used);
        if (checking) parts << tr("%n being checked with Dash Platform", nullptr, checking);
        if (abandoned) parts << tr("%n abandoned, kept in case it is mined", nullptr, abandoned);
        if (!parts.isEmpty()) lines << tr("Funding payments found in this wallet: %1.").arg(parts.join(tr(", ")));
        if (registration && !identity) {
            lines << tr("Create identity sends the identity funding payment found in this wallet, with no new "
                        "payment.");
        }
    }
    return lines;
}
