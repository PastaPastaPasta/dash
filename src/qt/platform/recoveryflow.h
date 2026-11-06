// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_RECOVERYFLOW_H
#define BITCOIN_QT_PLATFORM_RECOVERYFLOW_H

#include <primitives/transaction.h>
#include <qt/walletmodel.h>

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <set>
#include <string>

class ContactFlow;
class IdentityFlow;
class PlatformService;
class UsernameFlow;

/**
 * "Find my identity": the explicit recovery of a wallet's DashPay state from
 * its seed, and the first DashPay click. Under one passphrase it records the
 * wallet's consent, looks up the identity registered under the seed's first
 * identity key (index 0), lists the funding payments the wallet made, and
 * keeps the wallet unlocked until the contact requests we sent are read and
 * the receiving chain each recipient pays is imported. The flows it drives do
 * the work: IdentityFlow the lookup and the funding inventory, UsernameFlow
 * the usernames, ContactFlow the requests, the chains and the rescans the
 * wallet then owes.
 *
 * It says what it found and what it could not, part by part, and Try again
 * (find() again) retries every part that failed. A username of the identity
 * still waiting for its masternode vote is read here: a read that fails
 * leaves it unknown, never "none".
 */
class RecoveryFlow : public QObject
{
    Q_OBJECT

public:
    enum class Step {
        IDLE,
        FINDING, //!< from the passphrase until our requests are read and their chains imported
    };
    //! What is known of a username of the identity waiting for its vote.
    enum class Pending {
        UNKNOWN, //!< not read: the identity's names are not proved none yet
        FAILED,  //!< the last read failed; pendingError() says why
        NONE,
        FOUND, //!< pendingName() waits for its vote
    };

    RecoveryFlow(PlatformService& service, IdentityFlow& identity_flow, UsernameFlow& username_flow,
                 ContactFlow& contact_flow);

    Step step() const { return m_step; }
    Pending pending() const { return m_pending; }
    const std::optional<std::string>& pendingName() const { return m_pending_name; }
    const QString& pendingError() const { return m_pending_error; }
    //! Whether Find ran since the service started.
    bool ran() const { return m_ran; }
    //! Whether a part of the recovery could not be finished; find() retries it.
    bool incomplete() const;
    //! What Find found and could not do, one line each; empty before it ran
    //! unless a part is incomplete.
    QStringList report() const;

    //! Ask for the passphrase, start DashPay if it was not, and find the
    //! identity, its usernames, its contacts and the wallet's funding
    //! payments, retrying whatever failed before.
    void find();

Q_SIGNALS:
    void changed();

private:
    void update();
    //! Release the passphrase once the identity is looked up, and our
    //! requests are read and their chains imported, or can't be.
    void advance();
    void readPending();
    void finish(const QString& error = {});

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    UsernameFlow& m_username_flow;
    ContactFlow& m_contact_flow;
    Step m_step{Step::IDLE};
    bool m_ran{false};
    QString m_error;
    std::optional<WalletModel::UnlockContext> m_unlock;
    std::weak_ptr<void> m_pending_call;
    Pending m_pending{Pending::UNKNOWN};
    std::optional<std::string> m_pending_name;
    QString m_pending_error;
    //! The funding payments found in the wallet since Find ran.
    std::set<COutPoint> m_found_seen;
    //! The first block the rescan the wallet owes needs and this node does
    //! not have, read whenever a rescan pass ends.
    std::optional<int> m_missing_from;
    bool m_rescanning{false};
};

#endif // BITCOIN_QT_PLATFORM_RECOVERYFLOW_H
