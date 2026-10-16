// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_CLIENT_H
#define BITCOIN_PLATFORM_CLIENT_H

#include <platform/types.h>
#include <span.h>
#include <wallet/platformtypes.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace interfaces {
class LLMQ;
class Wallet;
} // namespace interfaces

namespace platform {

//! Keeps the wallet unlocked while a write signs. The consumer implements it
//! (dash-qt wraps WalletModel::UnlockContext). The client destroys it on the
//! executor just before the write's callback runs, or in shutdown().
class UnlockScope
{
public:
    virtual ~UnlockScope() = default;
};

//! The wallet a write signs with. Identity keys are
//! IdentityAuthKey{identity_index, key id}. The wallet must stay loaded
//! until the client has shut down.
struct Signing {
    // cppcheck-suppress uninitMemberVarNoCtor
    interfaces::Wallet& wallet;
    uint32_t identity_index{0};
    std::unique_ptr<UnlockScope> unlock;
};

//! Runs a task later on the consumer's thread, never inline (dash-qt queues
//! it with QMetaObject::invokeMethod). The client calls it from several
//! worker threads at once until shutdown() returns. A task owns nothing: one
//! that runs after shutdown() does nothing.
using Executor = std::function<void(std::function<void()>)>;

struct ClientConfig {
    std::optional<ProxyConfig> proxy; //!< nullopt: connect directly
    //! Evonode Platform HTTPS endpoints, fixed for the client's lifetime.
    std::vector<CService> endpoints;

    friend bool operator==(const ClientConfig&, const ClientConfig&) = default;
};

//! Asynchronous Dash Platform client. Each call runs on a worker thread and
//! its callback runs through the executor, never on a worker. Writes sign
//! through the wallet in Signing; private keys stay in the wallet.
class PlatformClient
{
public:
    template <typename T>
    using Callback = std::function<void(Result<T>)>;
    template <typename T>
    using FundingCallback = std::function<void(FundingResult<T>)>;

    virtual ~PlatformClient() = default;

    virtual void getIdentity(const Identifier& id, Callback<Identity> cb) = 0;
    //! The identity whose key hashes to pubkey_hash (HASH160).
    virtual void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<Identity> cb) = 0;
    //! The identity a DPNS name resolves to; the SDK normalizes the label.
    virtual void resolveName(const std::string& label, Callback<Identifier> cb) = 0;
    //! The names of an identity, reading at most max_pages pages of page_limit.
    virtual void namesOfIdentity(const Identifier& identity, uint32_t page_limit, uint32_t max_pages,
                                 Callback<Names> cb) = 0;
    //! Fails if the vote state of any of the contests cannot be read.
    virtual void pendingContests(const Identifier& identity, Callback<std::vector<Contest>> cb) = 0;
    //! One read of the contact requests sent by or to identity, created
    //! strictly after since_ms (0: all). An incomplete page is continued by
    //! a call from its next_since_ms. UNAVAILABLE, with the binding's
    //! message, when more requests share one creation time than one read
    //! returns: that does not clear on a retry from the same since_ms.
    //! Requests whose key indices or account reference exceed 32 bits are
    //! left out.
    virtual void contactRequests(const Identifier& identity, ContactDirection direction, uint64_t since_ms,
                                 Callback<ContactRequestPage> cb) = 0;
    //! Platform's record of an asset lock's credit output: outpoint names
    //! the transaction and the credit output, as an asset lock proof does.
    virtual void assetLockState(const COutPoint& outpoint, Callback<AssetLockState> cb) = 0;

    //! Create an identity with keys, funded by an asset lock paying
    //! RegistrationFundingKey{signing.identity_index}. No attempt uses a
    //! user_fee_increase below fee_increase_floor.
    virtual void registerIdentity(std::vector<IdentityPublicKey> keys, AssetLockProof proof,
                                  uint16_t fee_increase_floor, Signing signing, FundingCallback<Identity> cb) = 0;
    //! Add the asset lock paying funding to the identity's balance; the
    //! value is the new balance. Any of the wallet's funding keys can top
    //! up, a registration lock another identity left unused included. No
    //! attempt uses a user_fee_increase below fee_increase_floor.
    virtual void topUpIdentity(const Identifier& identity, AssetLockProof proof, wallet::AssetLockFundingKey funding,
                               uint16_t fee_increase_floor, Signing signing, FundingCallback<uint64_t> cb) = 0;
    //! Register a DPNS name, signed with identity key key_id, treating a
    //! contested label as policy says. The result is the domain document id.
    virtual void registerName(const std::string& label, const Identifier& identity, uint32_t key_id,
                              ContestPolicy policy, Signing signing, Callback<Identifier> cb) = 0;
    //! The result is the contact request document id.
    virtual void sendContactRequest(const ContactRequestInput& input, Signing signing, Callback<Identifier> cb) = 0;

    //! Drop queued calls, cancel and wait for running ones, and destroy
    //! every call not yet delivered, unlock scopes included. No callback runs
    //! afterwards, and later calls are dropped: a dropped call never calls
    //! back. Call it, or destroy the client, on the executor's thread (a
    //! result callback may), never while holding cs_main or cs_wallet, which
    //! the SDK's wallet and quorum callbacks take.
    virtual void shutdown() = 0;
};

//! The client over dash-sdk-cxx, verifying each proof against the quorum key
//! llmq.getPlatformQuorum() gives for the proof's signed Core height; llmq
//! must outlive the client. Network and Platform LLMQ type come from
//! Params(). Returns nullptr when the SDK refuses the configuration, such as
//! a proxy it cannot use: it never falls back to connecting directly.
std::unique_ptr<PlatformClient> MakeSdkPlatformClient(const ClientConfig& config, interfaces::LLMQ& llmq,
                                                      Executor executor);

//! DPNS rules, as the SDK applies them.
bool IsValidUsername(const std::string& label);
//! Fails closed: true when the SDK cannot answer.
bool IsContestedUsername(const std::string& label);

//! The id of the identity an asset lock's credit output registers, as
//! Platform derives it; nullopt when the SDK cannot answer.
std::optional<Identifier> IdentityIdFromAssetLock(const COutPoint& outpoint);

//! Decrypt a contact request's encryptedPublicKey with the ECDH secret.
//! The public key is not validated: check IsFullyValid() before use.
std::optional<wallet::FriendshipXpub> DecryptContactXpub(const std::array<uint8_t, 32>& shared_secret,
                                                         Span<const uint8_t> encrypted_public_key);

} // namespace platform

#endif // BITCOIN_PLATFORM_CLIENT_H
