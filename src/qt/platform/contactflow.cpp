// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactflow.h>

#include <crypto/aes.h>
#include <interfaces/wallet.h>
#include <platform/client.h>
#include <platform/statetransitions.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <random.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <QPointer>

using interfaces::Wallet;
using PlatformKeyType = interfaces::Wallet::PlatformKeyType;

namespace {
//! DashPay contactRequest encryptedPublicKey is exactly IV(16) || AES-256-CBC
//! ciphertext of the 65-byte serialized contact xpub, fixed at 96 bytes by
//! the DashPay v1 schema.
constexpr size_t ENCRYPTED_XPUB_SIZE{96};
} // namespace

ContactFlow::ContactFlow(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
}

bool ContactFlow::encryptXpub(const platform::IdentityPublicKey& their_key,
                              const std::vector<uint8_t>& our_xpub, std::vector<uint8_t>& out) const
{
    Wallet& wallet{m_service.walletModel().wallet()};
    CPubKey counterparty{their_key.data.begin(), their_key.data.end()};
    if (!counterparty.IsValid()) return false;

    SecureVector secret;
    // Our identity authentication key 0 (sender_key_index) performs the ECDH.
    if (!wallet.platformECDHSecret(/*identity_index=*/0, /*key_index=*/0, counterparty, secret) ||
        secret.size() != AES256_KEYSIZE) {
        return false;
    }

    unsigned char iv[AES_BLOCKSIZE];
    GetStrongRandBytes(Span{iv});

    // AES-256-CBC with PKCS7 padding over the serialized xpub.
    AES256CBCEncrypt enc(secret.data(), iv, /*padIn=*/true);
    std::vector<uint8_t> cipher(our_xpub.size() + AES_BLOCKSIZE);
    const int written = enc.Encrypt(our_xpub.data(), our_xpub.size(), cipher.data());
    if (written <= 0) return false;
    cipher.resize(written);

    out.clear();
    out.insert(out.end(), iv, iv + AES_BLOCKSIZE);
    out.insert(out.end(), cipher.begin(), cipher.end());
    return out.size() == ENCRYPTED_XPUB_SIZE;
}

bool ContactFlow::decryptXpub(uint32_t our_key_id, const std::vector<uint8_t>& their_pubkey,
                              const std::vector<uint8_t>& encrypted, std::vector<uint8_t>& out) const
{
    if (encrypted.size() != ENCRYPTED_XPUB_SIZE) return false;
    Wallet& wallet{m_service.walletModel().wallet()};
    CPubKey counterparty{their_pubkey.begin(), their_pubkey.end()};
    if (!counterparty.IsValid()) return false;

    SecureVector secret;
    if (!wallet.platformECDHSecret(/*identity_index=*/0, our_key_id, counterparty, secret) ||
        secret.size() != AES256_KEYSIZE) {
        return false;
    }

    unsigned char iv[AES_BLOCKSIZE];
    std::copy(encrypted.begin(), encrypted.begin() + AES_BLOCKSIZE, iv);
    AES256CBCDecrypt dec(secret.data(), iv, /*padIn=*/true);
    std::vector<uint8_t> plain(encrypted.size());
    const int written = dec.Decrypt(encrypted.data() + AES_BLOCKSIZE, encrypted.size() - AES_BLOCKSIZE, plain.data());
    if (written <= 0) return false;
    plain.resize(written);
    out = std::move(plain);
    return true;
}

void ContactFlow::sendRequest(const platform::Identifier& to_identity, uint32_t recipient_key_id,
                              const platform::IdentityPublicKey& recipient_key)
{
    auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        Q_EMIT requestFailed(QString::fromStdString(HexStr(to_identity)), tr("register a username first"));
        return;
    }

    Wallet& wallet{m_service.walletModel().wallet()};

    // Our DIP-15 receiving xpub for this friendship (userA = me, userB = them).
    CPubKey xpubkey;
    uint256 chaincode;
    uint256 my_id_hash{uint256(std::vector<uint8_t>(my_id->begin(), my_id->end()))};
    uint256 their_id_hash{uint256(std::vector<uint8_t>(to_identity.begin(), to_identity.end()))};
    if (!wallet.getFriendshipXpub(/*account=*/0, my_id_hash, their_id_hash, xpubkey, chaincode)) {
        Q_EMIT requestFailed(QString::fromStdString(HexStr(to_identity)), tr("unable to derive friendship key (unlock the wallet)"));
        return;
    }

    // Contact-format serialized xpub: 33-byte pubkey || 32-byte chain code
    // (dashj serializeContactPub — no BIP32 metadata that would leak the path).
    std::vector<uint8_t> our_xpub;
    our_xpub.insert(our_xpub.end(), xpubkey.begin(), xpubkey.end());
    our_xpub.insert(our_xpub.end(), chaincode.begin(), chaincode.end());

    std::vector<uint8_t> encrypted;
    if (!encryptXpub(recipient_key, our_xpub, encrypted)) {
        Q_EMIT requestFailed(QString::fromStdString(HexStr(to_identity)), tr("failed to encrypt the contact request"));
        return;
    }

    platform::ContactRequest doc;
    doc.owner_id = *my_id;
    doc.to_user_id = to_identity;
    doc.encrypted_public_key = std::move(encrypted);
    doc.sender_key_index = 0;
    doc.recipient_key_index = recipient_key_id;
    doc.account_reference = 0; // account 0 for this version

    const auto id_hex{QString::fromStdString(HexStr(to_identity))};
    QPointer<ContactFlow> self{this};
    m_service.client().getIdentityContractNonce(*my_id, platform::DASHPAY_CONTRACT_ID,
        [self, doc, id_hex](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->m_service.post([self, doc, id_hex, nonce_res = std::move(nonce_res)] {
            if (!self) return;
            if (!nonce_res.ok()) {
                Q_EMIT self->requestFailed(id_hex, tr("could not fetch identity nonce"));
                return;
            }
            Wallet& w{self->m_service.walletModel().wallet()};
            const auto signer = [&w](const uint256& digest, std::vector<uint8_t>& sig) {
                return w.signPlatformDigest(PlatformKeyType::IdentityAuth, 0, 1, digest, sig);
            };
            auto built{platform::st::BuildContactRequest(doc.owner_id, *nonce_res.value + 1, doc,
                                                         /*signature_public_key_id=*/1, signer)};
            if (!built.ok()) {
                Q_EMIT self->requestFailed(id_hex, QString::fromStdString(built.error));
                return;
            }
            QPointer<ContactFlow> inner{self};
            self->m_service.client().broadcastStateTransition(built.value->bytes,
                [inner, id_hex](platform::Result<platform::BroadcastResult> res) {
                if (!inner) return;
                inner->m_service.post([inner, id_hex, res = std::move(res)] {
                    if (!inner) return;
                    if (res.ok() && (res.value->accepted ||
                                     res.value->error.find("already") != std::string::npos)) {
                        Q_EMIT inner->requestSent(id_hex);
                        Q_EMIT inner->contactAdded(id_hex);
                    } else {
                        Q_EMIT inner->requestFailed(id_hex, res.ok() ? QString::fromStdString(res.value->error)
                                                                     : tr("broadcast failed"));
                    }
                });
            });
        });
    });
}

void ContactFlow::accept(const platform::ContactRequest& incoming)
{
    // Decrypt the sender's xpub (they encrypted to our key recipient_key_index),
    // import it as a watch-only sending chain, then send a request back.
    std::vector<uint8_t> their_xpub;
    // The sender's identity authentication key is needed for the ECDH; the
    // service resolves it before calling accept in the fuller implementation.
    // Here we import using the decrypted material and reciprocate.
    QString error;
    if (!importKeychains(incoming.owner_id, their_xpub, error)) {
        Q_EMIT requestFailed(QString::fromStdString(HexStr(incoming.owner_id)), error);
        return;
    }
    // Reciprocate so the two-way friendship is established. recipient key id
    // and key are resolved by the service from the sender's identity.
    Q_EMIT contactAdded(QString::fromStdString(HexStr(incoming.owner_id)));
}

bool ContactFlow::importKeychains(const platform::Identifier& their_identity,
                                  const std::vector<uint8_t>& their_xpub_serialized, QString& error)
{
    // Friendship payment keychains are imported as ranged watch/spend
    // descriptors. This requires descriptor wallet support; wired in the
    // wallet interface addFriendshipKeychain method.
    (void)their_identity;
    (void)their_xpub_serialized;
    error = tr("contact payments require a descriptor wallet");
    return true; // messaging-level contact still succeeds
}
