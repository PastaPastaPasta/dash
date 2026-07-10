// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
#define BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H

#include <consensus/amount.h>
#include <platform/client.h>
#include <platform/params.h>
#include <platform/types.h>
#include <uint256.h>

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class ClientModel;
class IdentityFlow;
class WalletModel;

/**
 * Per-wallet orchestrator for all Dash Platform interactions. This is the
 * only object GUI pages talk to. It owns the flows (identity/username
 * registration, contacts), marshals PlatformClient callbacks onto the GUI
 * thread, persists flow state through the wallet's platform data records,
 * and feeds node-local context (evonode endpoints, quorum keys) into the
 * client.
 */
class PlatformService : public QObject
{
    Q_OBJECT

public:
    PlatformService(WalletModel& wallet_model, ClientModel& client_model,
                    std::shared_ptr<platform::PlatformClient> client, QObject* parent = nullptr);
    ~PlatformService() override;

    WalletModel& walletModel() { return m_wallet_model; }
    ClientModel& clientModel() { return m_client_model; }
    platform::PlatformClient& client() { return *m_client; }
    const platform::Params& params() const { return m_params; }

    IdentityFlow& identityFlow() { return *m_identity_flow; }

    //! The registered username of this wallet's identity, if any.
    QString myUsername() const;
    std::optional<platform::Identifier> myIdentityId() const;

    //! Async name availability probe (proof-backed absence check).
    //! Emits nameAvailability().
    void checkNameAvailability(const QString& name);

    //! Async prefix search; emits searchResults().
    void searchNames(const QString& prefix);

    //! Async profile fetch; emits profileLoaded().
    void loadProfile(const platform::Identifier& identity);

    //! Wallet platform-data record helpers (used by the flows).
    bool writeRecord(const std::string& key, const std::vector<unsigned char>& value);
    std::vector<unsigned char> readRecord(const std::string& key) const;

    //! Run a callback on the GUI thread (safe from client threads; dropped
    //! if the service is destroyed first).
    void post(std::function<void()> fn);

Q_SIGNALS:
    void nameAvailability(const QString& normalized_label, bool available, bool contested);
    void searchResults(const QString& prefix, const QVector<QPair<QString, QString>>& results); //!< (label, identity id hex)
    void profileLoaded(const QString& identity_hex, const QString& display_name, const QString& public_message, const QString& avatar_url);
    void identityStateChanged();
    void flowFailed(const QString& step, const QString& error);

private Q_SLOTS:
    void updateNodeContext();

private:
    WalletModel& m_wallet_model;
    ClientModel& m_client_model;
    std::shared_ptr<platform::PlatformClient> m_client;
    platform::Params m_params;

    std::unique_ptr<IdentityFlow> m_identity_flow;
    QTimer* m_tick_timer{nullptr};         //!< drives flow advance/retry
    QTimer* m_context_timer{nullptr};      //!< refreshes endpoints/quorum keys
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMSERVICE_H
