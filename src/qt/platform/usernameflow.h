// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_USERNAMEFLOW_H
#define BITCOIN_QT_PLATFORM_USERNAMEFLOW_H

#include <platform/types.h>
#include <qt/walletmodel.h>

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class IdentityFlow;
class PlatformService;

/**
 * Registers the DashPay username of the wallet's identity, as the mobile
 * wallets do: a label DPNS accepts and that resolves to nobody is registered
 * with one SDK call, signed with the identity's HIGH authentication key. A
 * wallet without an identity creates it first through IdentityFlow, under
 * the same unlock.
 *
 * Nothing is persisted: the identity's names, read from Platform whenever
 * the identity is found, say whether a registration whose answer was lost
 * landed. Registering is offered only once they are proved to be none: a
 * read that fails is not "none", and is retried on the service's and the
 * identity's next updates and on each new block once its wait has passed,
 * each failure doubling the wait.
 */
class UsernameFlow : public QObject
{
    Q_OBJECT

public:
    enum class Step {
        IDLE,
        CHECKING,  //!< resolving the label
        PREPARING, //!< unlocked; waiting for the identity being created, or for a client
        REGISTERING,
    };

    //! What is known of the identity's names.
    enum class Names {
        UNKNOWN, //!< not read since the identity was found
        FAILED,  //!< the last read failed; namesError() says why
        NONE,    //!< proved to be none
        FOUND,   //!< username() is the first
    };

    //! The first wait before a failed read of the names is retried.
    static constexpr std::chrono::seconds NAMES_RETRY{15};
    //! The longest wait between retries.
    static constexpr std::chrono::seconds NAMES_RETRY_MAX{std::chrono::minutes{15}};

    UsernameFlow(PlatformService& service, IdentityFlow& identity_flow);

    Step step() const { return m_step; }
    Names names() const { return m_names; }
    //! Whether the identity's names were read since it was found.
    bool namesRead() const { return m_names == Names::NONE || m_names == Names::FOUND; }
    //! The identity's username; nullopt when it has none or they weren't read.
    const std::optional<std::string>& username() const { return m_username; }
    //! Why the last registration failed; empty when it did not.
    const QString& error() const { return m_error; }
    //! Why the last read of the names failed; empty unless names() is FAILED.
    const QString& namesError() const { return m_names_error; }

    //! Register `label` for the wallet's identity, creating the identity
    //! first if the wallet has none.
    void registerName(const std::string& label);
    //! Read the names again now if the last read failed, whatever the wait.
    void retryNames();

Q_SIGNALS:
    void changed();

private:
    void update();
    void readNames();
    void onResolved(platform::Result<platform::Identifier> result);
    void send(const platform::Identity& identity);
    void onRegistered(platform::Result<platform::Identifier> result);
    void resolve();
    void finish(const QString& error = {});

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    Step m_step{Step::IDLE};
    std::string m_label;
    //! Taken once the label is free, and held until the name is sent: a new
    //! identity is funded and the name signed under it.
    std::optional<WalletModel::UnlockContext> m_unlock;
    //! The step's call and the names read, until they call back or are dropped.
    std::weak_ptr<void> m_call;
    std::weak_ptr<void> m_names_call;
    Names m_names{Names::UNKNOWN};
    std::optional<std::string> m_username;
    QString m_error;
    QString m_names_error;
    //! Consecutive failed reads, and when the next may start (UNIX seconds).
    int m_names_failures{0};
    int64_t m_names_retry_at{0};
};

#endif // BITCOIN_QT_PLATFORM_USERNAMEFLOW_H
