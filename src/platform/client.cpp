// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>

#include <chainparams.h>
#include <logging.h>
#include <platform/marshal.h>
#include <platform/signer.h>
#include <sync.h>
#include <util/chaintype.h>
#include <util/overloaded.h>
#include <util/thread.h>

#include <dash/sdk/ffi.h>

#include <cassert>
#include <condition_variable>
#include <deque>
#include <exception>
#include <thread>
#include <utility>
#include <variant>

namespace platform {

namespace {

//! Enough for a write in flight not to hold up the reads a wallet load makes.
constexpr int WORKER_THREADS{4};

//! A call. Run() makes the blocking SDK call on a worker; Deliver() releases
//! the unlock scope and runs the callback on the executor. A task is
//! destroyed on the executor or by shutdown(), never on a worker.
class Task
{
public:
    virtual ~Task() = default;
    virtual void Run() = 0;
    virtual void Deliver() = 0;
};

//! R is the call's result: a Result, or a FundingResult for identity funding.
template <typename R>
class Call final : public Task
{
public:
    //! initial is the result when the call throws before it returns one.
    Call(std::function<R()> call, std::function<void(R)> cb, std::unique_ptr<UnlockScope> unlock, R initial) :
        m_call(std::move(call)),
        m_cb(std::move(cb)),
        m_unlock(std::move(unlock)),
        m_result(std::move(initial))
    {
    }

    void Run() override
    {
        try {
            m_result = m_call();
        } catch (const std::exception& e) {
            // A rust::Str or rust::String refused a string that is not UTF-8.
            m_result.status.message = e.what();
        }
    }

    void Deliver() override
    {
        // Relock first, so that a write the callback starts unlocks afresh.
        m_unlock.reset();
        m_cb(std::move(m_result));
    }

private:
    std::function<R()> m_call;
    std::function<void(R)> m_cb;
    std::unique_ptr<UnlockScope> m_unlock;
    R m_result;
};

//! A funding call's result when Core fails before calling the binding: no
//! attempt started, so the report is one below the floor, as the binding's.
template <typename T>
FundingResult<T> NotStarted(uint16_t fee_increase_floor)
{
    FundingResult<T> result;
    result.funding.last_user_fee_increase = fee_increase_floor > 0 ? fee_increase_floor - 1 : 0;
    return result;
}

//! The binding hands the asset lock's key reference back to the signer,
//! which signs with the funding key itself.
uint32_t KeyIndex(const wallet::AssetLockFundingKey& key)
{
    return std::visit(util::Overloaded{[](const wallet::RegistrationFundingKey& k) { return k.identity_index; },
                                       [](const wallet::TopupFundingKey& k) { return k.funding_index; },
                                       [](const wallet::InvitationFundingKey& k) { return k.invitation_index; }},
                      key);
}

class TaskQueue
{
public:
    //! A task pushed after Close() is destroyed here.
    void Push(std::unique_ptr<Task> task) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        {
            LOCK(m_mutex);
            if (m_closed) return;
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    //! The next task, waiting for one if wait is set; nullptr once closed.
    std::unique_ptr<Task> Pop(bool wait) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WAIT_LOCK(m_mutex, lock);
        if (wait) m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_closed || !m_tasks.empty(); });
        if (m_closed || m_tasks.empty()) return nullptr;
        auto task{std::move(m_tasks.front())};
        m_tasks.pop_front();
        return task;
    }

    //! Destroys the queued tasks here, outside the lock. False if it was
    //! already closed.
    bool Close() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        // cppcheck-suppress variableScope
        std::deque<std::unique_ptr<Task>> dropped;
        {
            LOCK(m_mutex);
            if (m_closed) return false;
            m_closed = true;
            dropped.swap(m_tasks);
        }
        m_cv.notify_all();
        return true;
    }

private:
    Mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::unique_ptr<Task>> m_tasks GUARDED_BY(m_mutex);
    bool m_closed GUARDED_BY(m_mutex){false};
};

class SdkClient final : public PlatformClient
{
public:
    SdkClient(rust::Box<dash_sdk::PlatformClient> sdk, Executor executor) :
        m_sdk(std::move(sdk)),
        m_executor(std::move(executor))
    {
        for (int i = 0; i < WORKER_THREADS; ++i) {
            m_workers.emplace_back(&util::TraceThread, "platform", [this] { WorkerLoop(); });
        }
    }

    ~SdkClient() override { shutdown(); }

    void getIdentity(const Identifier& id, Callback<Identity> cb) override
    {
        Submit<Result<Identity>>([this, id] { return marshal::FromFfi(m_sdk->get_identity(id)); }, std::move(cb));
    }

    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<Identity> cb) override
    {
        Submit<Result<Identity>>(
            [this, pubkey_hash] { return marshal::FromFfi(m_sdk->get_identity_by_pubkey_hash(pubkey_hash)); },
            std::move(cb));
    }

    void resolveName(const std::string& label, Callback<Identifier> cb) override
    {
        Submit<Result<Identifier>>([this, label] { return marshal::FromFfi(m_sdk->resolve_name(label)); }, std::move(cb));
    }

    void namesOfIdentity(const Identifier& identity, uint32_t page_limit, uint32_t max_pages, Callback<Names> cb) override
    {
        Submit<Result<Names>>(
            [this, identity, page_limit, max_pages] {
                return marshal::FromFfi(m_sdk->names_of_identity(identity, page_limit, max_pages));
            },
            std::move(cb));
    }

    void pendingContests(const Identifier& identity, Callback<std::vector<Contest>> cb) override
    {
        Submit<Result<std::vector<Contest>>>(
            [this, identity] { return marshal::FromFfi(m_sdk->pending_contests_for_identity(identity)); }, std::move(cb));
    }

    void contactRequests(const Identifier& identity, ContactDirection direction, uint64_t since_ms,
                         Callback<ContactRequestPage> cb) override
    {
        const auto ffi_direction{direction == ContactDirection::SENT ? dash_sdk::ContactDirection::Sent
                                                                     : dash_sdk::ContactDirection::Received};
        Submit<Result<ContactRequestPage>>(
            [this, identity, ffi_direction, since_ms] {
                return marshal::FromFfi(m_sdk->contact_requests(identity, ffi_direction, since_ms), since_ms);
            },
            std::move(cb));
    }

    void assetLockState(const COutPoint& outpoint, Callback<AssetLockState> cb) override
    {
        Submit<Result<AssetLockState>>(
            [this, outpoint] {
                return marshal::FromFfi(m_sdk->asset_lock_state(marshal::ToFfi(outpoint.hash), outpoint.n));
            },
            std::move(cb));
    }

    void registerIdentity(std::vector<IdentityPublicKey> keys, AssetLockProof proof, uint16_t fee_increase_floor,
                          Signing signing, FundingCallback<Identity> cb) override
    {
        Submit<FundingResult<Identity>>(
            [this, keys = std::move(keys), proof = std::move(proof), fee_increase_floor, &wallet = signing.wallet,
             index = signing.identity_index] {
                const auto ffi_keys{marshal::ToFfi(keys)};
                return marshal::FromFfi(
                    m_sdk->register_identity({ffi_keys.data(), ffi_keys.size()}, marshal::ToFfi(proof),
                                             /*asset_lock_key_ref=*/index, fee_increase_floor,
                                             MakeWalletSigner(wallet, index, wallet::RegistrationFundingKey{index})));
            },
            std::move(cb), std::move(signing.unlock), NotStarted<Identity>(fee_increase_floor));
    }

    void topUpIdentity(const Identifier& identity, AssetLockProof proof, wallet::AssetLockFundingKey funding,
                       uint16_t fee_increase_floor, Signing signing, FundingCallback<uint64_t> cb) override
    {
        Submit<FundingResult<uint64_t>>(
            [this, identity, proof = std::move(proof), funding, fee_increase_floor, &wallet = signing.wallet,
             index = signing.identity_index] {
                return marshal::FromFfi(m_sdk->top_up_identity(identity, marshal::ToFfi(proof),
                                                               /*asset_lock_key_ref=*/KeyIndex(funding),
                                                               fee_increase_floor,
                                                               MakeWalletSigner(wallet, index, funding)));
            },
            std::move(cb), std::move(signing.unlock), NotStarted<uint64_t>(fee_increase_floor));
    }

    void registerName(const std::string& label, const Identifier& identity, uint32_t key_id, ContestPolicy policy,
                      Signing signing, Callback<Identifier> cb) override
    {
        Submit<Result<Identifier>>(
            [this, label, identity, key_id, policy, &wallet = signing.wallet, index = signing.identity_index] {
                return marshal::FromFfi(m_sdk->register_dpns_name(label, identity, key_id, marshal::ToFfi(policy),
                                                                  MakeWalletSigner(wallet, index, std::nullopt)));
            },
            std::move(cb), std::move(signing.unlock));
    }

    void sendContactRequest(const ContactRequestInput& input, Signing signing, Callback<Identifier> cb) override
    {
        Submit<Result<Identifier>>(
            [this, input, &wallet = signing.wallet, index = signing.identity_index] {
                return marshal::FromFfi(m_sdk->send_contact_request(marshal::ToFfi(input),
                                                                    MakeWalletSigner(wallet, index, std::nullopt),
                                                                    MakeContactKeys(wallet, index, input.sender)));
            },
            std::move(cb), std::move(signing.unlock));
    }

    void shutdown() override
    {
        if (!m_calls.Close()) return;
        // Cancels the calls in flight and waits for them to return.
        m_sdk->shutdown();
        for (auto& worker : m_workers)
            worker.join();
        // Finished calls the executor has not run yet die here, unlock
        // scopes included, without their callbacks.
        m_results->Close();
    }

private:
    template <typename R>
    void Submit(std::function<R()> call, std::function<void(R)> cb, std::unique_ptr<UnlockScope> unlock = nullptr,
                R initial = {})
    {
        m_calls.Push(std::make_unique<Call<R>>(std::move(call), std::move(cb), std::move(unlock), std::move(initial)));
    }

    void WorkerLoop()
    {
        while (auto task{m_calls.Pop(/*wait=*/true)}) {
            task->Run();
            m_results->Push(std::move(task));
            // Each posted closure delivers one finished call; it owns none.
            m_executor([results = m_results] {
                if (auto result{results->Pop(/*wait=*/false)}) result->Deliver();
            });
        }
    }

    const rust::Box<dash_sdk::PlatformClient> m_sdk;
    const Executor m_executor;
    TaskQueue m_calls;
    const std::shared_ptr<TaskQueue> m_results{std::make_shared<TaskQueue>()};
    std::vector<std::thread> m_workers;
};

dash_sdk::Network SdkNetwork()
{
    switch (Params().GetChainType()) {
    case ChainType::MAIN:
        return dash_sdk::Network::Mainnet;
    case ChainType::TESTNET:
        return dash_sdk::Network::Testnet;
    case ChainType::DEVNET:
        return dash_sdk::Network::Devnet;
    case ChainType::REGTEST:
        return dash_sdk::Network::Regtest;
    }
    assert(false);
}

} // namespace

std::unique_ptr<PlatformClient> MakeSdkPlatformClient(const ClientConfig& config, interfaces::LLMQ& llmq, Executor executor)
{
    try {
        dash_sdk::Config sdk_config;
        sdk_config.network = SdkNetwork();
        sdk_config.platform_llmq_type = static_cast<uint8_t>(Params().GetConsensus().llmqTypePlatform);
        sdk_config.proxy = marshal::ToFfi(config.proxy);
        for (const CService& endpoint : config.endpoints) {
            sdk_config.endpoints.push_back("https://" + endpoint.ToStringAddrPort());
        }
        return std::make_unique<SdkClient>(dash_sdk::new_platform_client(sdk_config, MakeCoreContext(llmq)),
                                           std::move(executor));
    } catch (const std::exception& e) {
        LogPrintf("Platform client: the SDK refused its configuration: %s\n", e.what());
        return nullptr;
    }
}

bool IsValidUsername(const std::string& label)
{
    try {
        return dash_sdk::is_valid_username(label);
    } catch (const std::exception&) {
        return false;
    }
}

bool IsContestedUsername(const std::string& label)
{
    try {
        return dash_sdk::is_contested_username(label);
    } catch (const std::exception&) {
        return true;
    }
}

std::optional<Identifier> IdentityIdFromAssetLock(const COutPoint& outpoint)
{
    const auto id{
        marshal::FromFfi(dash_sdk::identity_id_from_asset_lock_outpoint(marshal::ToFfi(outpoint.hash), outpoint.n))};
    if (!id.ok()) return std::nullopt;
    return id.value;
}

std::optional<wallet::FriendshipXpub> DecryptContactXpub(const std::array<uint8_t, 32>& shared_secret,
                                                         Span<const uint8_t> encrypted_public_key)
{
    try {
        const auto xpub{
            dash_sdk::decrypt_contact_xpub(shared_secret, {encrypted_public_key.data(), encrypted_public_key.size()})};
        return wallet::FriendshipXpub{CPubKey{xpub.public_key}, ChainCode{xpub.chain_code}, xpub.parent_fingerprint};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace platform
