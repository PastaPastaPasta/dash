// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTFLOW_H
#define BITCOIN_QT_PLATFORM_CONTACTFLOW_H

#include <platform/types.h>
#include <uint256.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

class PlatformService;

/**
 * Sends a DashPay contact request and imports the resulting friendship
 * keychains so payments to/from the contact are tracked by the wallet.
 *
 * A contact request carries our DIP-15 receiving xpub, ECDH-encrypted
 * (AES-256-CBC) to the recipient's identity key so only they can read it.
 * Accepting an incoming request is symmetric: we decrypt their xpub, import
 * it as a watch-only sending chain, and send our own request back.
 *
 * Per-contact state is persisted under "contact/out/<id>" and
 * "contact/in/<id>" platform data records so requests survive restarts and
 * are re-derivable (the request document itself is on Platform; the local
 * record is a cursor).
 */
class ContactFlow : public QObject
{
    Q_OBJECT

public:
    explicit ContactFlow(PlatformService& service, QObject* parent = nullptr);

    //! Send a contact request to the given identity. Requires an unlocked
    //! wallet (ECDH + xpub derivation). Emits requestSent/requestFailed.
    void sendRequest(const platform::Identifier& to_identity, uint32_t recipient_key_id,
                     const platform::IdentityPublicKey& recipient_key);

    //! Accept an incoming request: decrypt the sender's xpub, import the
    //! sending keychain, and send a request back.
    void accept(const platform::ContactRequest& incoming);

    //! Import our receiving keychain and (for accept) their sending keychain
    //! into the wallet so addresses are watched.
    bool importKeychains(const platform::Identifier& their_identity,
                         const std::vector<uint8_t>& their_xpub_serialized, QString& error);

Q_SIGNALS:
    void requestSent(const QString& to_identity_hex);
    void requestFailed(const QString& to_identity_hex, const QString& error);
    void contactAdded(const QString& identity_hex);

private:
    //! ECDH+AES helpers (encrypt our xpub for them / decrypt theirs for us).
    bool encryptXpub(const platform::IdentityPublicKey& their_key, const std::vector<uint8_t>& our_xpub,
                     std::vector<uint8_t>& out) const;
    bool decryptXpub(uint32_t our_key_id, const std::vector<uint8_t>& their_pubkey,
                     const std::vector<uint8_t>& encrypted, std::vector<uint8_t>& out) const;

    PlatformService& m_service;
};

#endif // BITCOIN_QT_PLATFORM_CONTACTFLOW_H
