// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>
#include <platform/marshal.h>
#include <platform/signer.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <pubkey.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <wallet/context.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace platform;

namespace {

//! The mnemonic of platformkeys_tests' DIP-14 seed.
const SecureString MNEMONIC{"birth kingdom trash renew flavor utility donkey gasp regular alert pave layer"};

struct PlatformClientSetup : public TestingSetup {
    PlatformClientSetup()
    {
        m_context.args = &m_args;
        m_context.chain = m_node.chain.get();
        m_wallet = std::make_shared<wallet::CWallet>(m_node.chain.get(), "", wallet::CreateMockableWalletDatabase());
        m_wallet->LoadWallet();
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
            m_wallet->SetupDescriptorScriptPubKeyMans(MNEMONIC, "");
        }
        m_iface = interfaces::MakeWallet(m_context, m_wallet);
    }

    wallet::WalletContext m_context;
    std::shared_ptr<wallet::CWallet> m_wallet;
    std::unique_ptr<interfaces::Wallet> m_iface;
};

//! The consumer's thread: tasks the client posts wait here until the test
//! runs them.
class TestExecutor
{
public:
    Executor executor()
    {
        return [this](std::function<void()> task) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) {
            LOCK(m_mutex);
            m_tasks.push_back(std::move(task));
        };
    }

    size_t pending() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_tasks.size();
    }

    //! Wait until n tasks are posted.
    bool WaitFor(size_t n) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        for (int i = 0; i < 1000 && pending() < n; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        return pending() >= n;
    }

    //! Run and destroy every posted task on this thread.
    void Drain() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        std::vector<std::function<void()>> tasks;
        WITH_LOCK(m_mutex, tasks.swap(m_tasks));
        for (auto& task : tasks)
            task();
    }

private:
    Mutex m_mutex;
    std::vector<std::function<void()>> m_tasks GUARDED_BY(m_mutex);
};

//! Records the thread it is released on.
class TestUnlock final : public UnlockScope
{
public:
    explicit TestUnlock(std::optional<std::thread::id>& released_on) :
        m_released_on(released_on)
    {
    }
    ~TestUnlock() override { m_released_on = std::this_thread::get_id(); }

private:
    std::optional<std::thread::id>& m_released_on;
};

//! Answers every quorum lookup with `answer`, or throws when `fail` is set,
//! and records what it was asked.
class TestLLMQ final : public interfaces::LLMQ
{
public:
    struct Lookup {
        uint8_t llmq_type;
        uint256 quorum_hash;
        uint32_t core_chain_locked_height;
    };
    std::vector<Lookup> lookups;
    PlatformQuorumResult answer;
    bool fail{false};

    ChainLockInfo getBestChainLock() override { return {}; }
    CreditPoolCounts getCreditPoolCounts() override { return {}; }
    InstantSendCounts getInstantSendCounts() override { return {}; }
    size_t getPendingAssetUnlocks() override { return 0; }
    std::vector<QuorumInfo> getQuorumStats() override { return {}; }
    PlatformQuorumResult getPlatformQuorum(uint8_t llmq_type, const uint256& quorum_hash,
                                           uint32_t core_chain_locked_height) override
    {
        lookups.push_back({llmq_type, quorum_hash, core_chain_locked_height});
        if (fail) throw std::runtime_error{"evodb read failed"};
        return answer;
    }
    std::vector<uint8_t> getInstantSendLock(const uint256&) override { return {}; }
};

//! Whether another thread takes mutex within 30 s.
bool HeldElsewhere(RecursiveMutex& mutex)
{
    for (int i = 0; i < 30'000; ++i) {
        {
            TRY_LOCK(mutex, available);
            if (!available) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(platform_client_tests, PlatformClientSetup)

//! Through the real client with no endpoints: callbacks run on the
//! consumer's executor, never on a worker, a write's unlock scope being
//! released there just before its callback; shutdown() waits for a call
//! that is inside a wallet callback and releases its unlock scope; and no
//! callback runs after shutdown().
BOOST_AUTO_TEST_CASE(client_threads_and_shutdown)
{
    TestLLMQ llmq;
    TestExecutor executor;
    auto client{MakeSdkPlatformClient(ClientConfig{}, llmq, executor.executor())};
    BOOST_REQUIRE(client);
    const auto consumer{std::this_thread::get_id()};

    // Results wait for the executor; a write keeps the wallet unlocked until
    // its result is delivered.
    std::optional<StatusKind> read_status;
    std::optional<std::thread::id> read_thread;
    client->getIdentity(Identifier{}, [&](Result<Identity> result) {
        read_status = result.status.kind;
        read_thread = std::this_thread::get_id();
    });
    std::optional<std::thread::id> unlock_released_on;
    std::optional<std::thread::id> unlock_released_before_callback;
    client->registerName("alice", Identifier{}, /*key_id=*/1, ContestPolicy{},
                         Signing{*m_iface, 0, std::make_unique<TestUnlock>(unlock_released_on)},
                         [&](Result<Identifier>) { unlock_released_before_callback = unlock_released_on; });
    BOOST_REQUIRE(executor.WaitFor(2));
    BOOST_CHECK(!read_status);
    BOOST_CHECK(!unlock_released_on);
    executor.Drain();
    BOOST_CHECK(read_status == StatusKind::UNAVAILABLE);
    BOOST_CHECK(read_thread == consumer);
    BOOST_CHECK(unlock_released_before_callback == consumer);

    // A registration blocked in the wallet signer: shutdown() must not
    // return before the signer does. The holder keeps the wallet's first
    // Platform key source locked, so the signer stops inside the wallet
    // holding cs_wallet, which is how this thread sees it enter.
    const auto pubkey{m_iface->getPlatformPubKey(wallet::IdentityAuthKey{0, 0})};
    BOOST_REQUIRE(pubkey);
    IdentityPublicKey key;
    key.data.assign(pubkey.value.begin(), pubkey.value.end());
    auto& source{*Assert(dynamic_cast<wallet::DescriptorScriptPubKeyMan*>(*m_wallet->GetActiveScriptPubKeyMans().begin()))};
    std::promise<void> locked;
    std::promise<void> entered;
    std::atomic_bool releasing{false};
    std::thread holder{[&] {
        LOCK(source.cs_desc_man);
        locked.set_value();
        entered.get_future().wait();
        // Longer than the SDK runtime's own 2 s stop timeout.
        std::this_thread::sleep_for(std::chrono::milliseconds{3500});
        releasing = true;
    }};
    locked.get_future().wait();
    unlock_released_on.reset();
    bool write_called{false};
    client->registerIdentity({key}, ChainAssetLockProof{}, /*fee_increase_floor=*/0,
                             Signing{*m_iface, 0, std::make_unique<TestUnlock>(unlock_released_on)},
                             [&](FundingResult<Identity>) { write_called = true; });
    const bool signer_entered{HeldElsewhere(m_wallet->cs_wallet)};
    entered.set_value();
    client->shutdown();
    const bool shutdown_waited{releasing};
    holder.join();
    BOOST_CHECK(signer_entered);
    BOOST_CHECK(shutdown_waited);

    // shutdown() released the write's unlock here; its result, handed to the
    // executor, no longer calls back.
    BOOST_CHECK(unlock_released_on == consumer);
    executor.Drain();
    BOOST_CHECK(!write_called);

    // Calls after shutdown() are dropped on the calling thread.
    unlock_released_on.reset();
    client->registerName("alice", Identifier{}, 1, ContestPolicy{},
                         Signing{*m_iface, 0, std::make_unique<TestUnlock>(unlock_released_on)},
                         [&](Result<Identifier>) { write_called = true; });
    BOOST_CHECK(unlock_released_on == consumer);
    BOOST_CHECK_EQUAL(executor.pending(), 0U);
    BOOST_CHECK(!write_called);
}

//! The signer answers with the wallet's RFC6979 compact signature over
//! SHA256d(signable) for each published identity key, by identity index,
//! and over the sighash for the asset lock's funding key. Expected values
//! are computed independently in Python (stdlib HMAC-SHA512 BIP32 and the
//! functional test framework's secp256k1) from the DIP-14 seed, on the
//! mainnet coin type that TestingSetup's chain uses. An identity key is
//! signed for only when the key Platform records is ours.
BOOST_AUTO_TEST_CASE(wallet_signer_vectors)
{
    std::vector<uint8_t> signable(64);
    for (size_t i = 0; i < signable.size(); ++i)
        signable[i] = i;
    std::array<uint8_t, 65> sig;

    const struct {
        uint32_t identity_index;
        uint32_t key_id;
        const char* signature;
    } identity_keys[]{
        {0, 0,
         "2001450b14681ad74139462b838ff42b46f4f742a1e41134949084a10b62589c64471aed532f76c7d5878439baf57166f98ad85c0358a"
         "5fa59ea65c8c196fc2e22"},
        {0, 1,
         "1ff1882b86979e490de505496ef06d0c79d91f2deccd3e2bd575ddb0f0ab0481bc508d0e4f79611fc15d4e779cd9c19d8f5291a0255bf"
         "475f37d9ef662971b5412"},
        {0, 2,
         "1ffed3a6bcc4cec33c32a9149c7c94c43643a2c0a58d7e55a57cba85b817331a1910715c1670978a4ce8e1f6d1fa062cdf6cf40e54a8f"
         "941711dde613a0e9d002e"},
        {0, 3,
         "1fc40d13035242431cfb237c0e68bfbd0b3246600ee216b2c5e279158f54a1af276173fc25abfa25ddbd0486f7857d3e61b74a9c228b1"
         "3a51b070937b37de57d7d"},
        {1, 0,
         "20b4be2547b6d1a826da05a0b230b4b95ec333341662b2a09fbf2d429b5ad689392dffd24624290e53efe75a58af9f025ad97817365cc"
         "ec1a495a73341301d2e2d"},
    };
    const auto public_key{[&](uint32_t identity_index, uint32_t key_id) {
        const auto pubkey{m_iface->getPlatformPubKey(wallet::IdentityAuthKey{identity_index, key_id})};
        BOOST_REQUIRE(pubkey);
        return std::vector<uint8_t>{pubkey.value.begin(), pubkey.value.end()};
    }};
    for (const auto& row : identity_keys) {
        const auto signer{MakeWalletSigner(*m_iface, row.identity_index, std::nullopt)};
        const auto key_data{public_key(row.identity_index, row.key_id)};
        BOOST_REQUIRE(signer->SignIdentityKey(row.key_id, /*key_type=*/0, {key_data.data(), key_data.size()},
                                              {signable.data(), signable.size()}, sig));
        BOOST_CHECK_EQUAL(HexStr(sig), row.signature);
    }

    // Platform's record of key 0 names another key, a truncated key or none.
    const auto signer{MakeWalletSigner(*m_iface, 0, std::nullopt)};
    auto wrong{public_key(0, 1)};
    BOOST_CHECK(!signer->SignIdentityKey(0, 0, {wrong.data(), wrong.size()}, {signable.data(), signable.size()}, sig));
    wrong = public_key(0, 0);
    wrong.pop_back();
    BOOST_CHECK(!signer->SignIdentityKey(0, 0, {wrong.data(), wrong.size()}, {signable.data(), signable.size()}, sig));
    BOOST_CHECK(!signer->SignIdentityKey(0, 0, {}, {signable.data(), signable.size()}, sig));

    std::array<uint8_t, 32> sighash;
    sighash.fill(0x42);
    const auto registration{MakeWalletSigner(*m_iface, 0, wallet::RegistrationFundingKey{0})};
    BOOST_REQUIRE(registration->SignAssetLock(/*key_ref=*/0, sighash, sig));
    BOOST_CHECK_EQUAL(HexStr(sig),
                      "207982873b927f462833f7ac2a5ddfa56d4f5a224a2f594be99f737530455d5b682da131a012cd943ce5"
                      "e5a85534814189777b5f6be1de65579eb568d247599c1c");
    const auto top_up{MakeWalletSigner(*m_iface, 0, wallet::TopupFundingKey{3})};
    BOOST_REQUIRE(top_up->SignAssetLock(/*key_ref=*/3, sighash, sig));
    BOOST_CHECK_EQUAL(HexStr(sig),
                      "1fe4adc78e2d294bd18f1020be29e6589eb7a5f8e98548097b11e9ff01587e284e3a097070bd7a6ccdbc"
                      "dc1a11c608c8248e1cef5fc34d65d6861fc4a25807566a");

    // A write without an asset lock has no funding key to sign with, and
    // no ECDSA_HASH160 key is signed with yet.
    const auto document{MakeWalletSigner(*m_iface, 0, std::nullopt)};
    BOOST_CHECK(!document->SignAssetLock(0, sighash, sig));
    const auto key_data{public_key(0, 0)};
    BOOST_CHECK(!document->SignIdentityKey(0, /*key_type=*/2, {key_data.data(), key_data.size()},
                                           {signable.data(), signable.size()}, sig));

    // ECDH with our ENCRYPTION key (identity 0, key 2) and the recipient key
    // 7*G: SHA256 of the compressed shared point.
    const auto contact{MakeContactKeys(*m_iface, 0, Identifier{})};
    std::array<uint8_t, 33> recipient_key;
    const auto recipient{ParseHex("025cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc")};
    std::copy(recipient.begin(), recipient.end(), recipient_key.begin());
    std::array<uint8_t, 32> secret;
    BOOST_REQUIRE(contact->SharedSecret(/*sender_key_id=*/2, recipient_key, secret));
    BOOST_CHECK_EQUAL(HexStr(secret), "893d3a804b2252c11400aeddcf83599fc9f3ac8b6067729c1c95455745cd410f");
}

//! The compact xpub published to a recipient is our receiving chain from the
//! sender towards that recipient, not the reverse.
BOOST_AUTO_TEST_CASE(contact_xpub_is_our_receiving_chain)
{
    Identifier sender, recipient;
    sender.fill(0x11);
    recipient.fill(0x22);
    const auto contact{MakeContactKeys(*m_iface, 0, sender)};
    std::array<uint8_t, 69> xpub;
    BOOST_REQUIRE(contact->CompactXpub(recipient, /*account_reference=*/0, xpub));

    const auto chain{m_iface->ensureFriendshipReceivingKeychain(
        {/*account=*/0, uint256{sender}, uint256{recipient}, /*birth_time=*/0})};
    BOOST_REQUIRE(chain);
    wallet::CompactXpub expected;
    BOOST_REQUIRE(wallet::CompactXpubBytes(chain.value, expected));
    BOOST_CHECK(xpub == expected);
}

//! Each lookup asks the node about the quorum at the height the proof
//! carries, and its answer maps one to one. A key of the wrong size or a
//! lookup that throws is no answer: Unavailable, never Invalid.
BOOST_AUTO_TEST_CASE(core_context_maps_each_quorum_lookup)
{
    TestLLMQ llmq;
    const auto context{MakeCoreContext(llmq)};
    const uint256 hash{uint256S("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")};
    std::array<uint8_t, 32> quorum_hash;
    std::copy(hash.begin(), hash.end(), quorum_hash.begin());
    std::array<uint8_t, 48> key;

    llmq.answer = {interfaces::LLMQ::PlatformQuorumStatus::FOUND, std::vector<uint8_t>(48, 0xab)};
    BOOST_CHECK(context->QuorumPublicKey(6, quorum_hash, /*core_chain_locked_height=*/1234, key) ==
                dash_sdk::QuorumLookup::Found);
    BOOST_CHECK(std::all_of(key.begin(), key.end(), [](uint8_t b) { return b == 0xab; }));
    BOOST_REQUIRE_EQUAL(llmq.lookups.size(), 1U);
    BOOST_CHECK_EQUAL(llmq.lookups[0].llmq_type, 6);
    BOOST_CHECK(llmq.lookups[0].quorum_hash == hash);
    BOOST_CHECK_EQUAL(llmq.lookups[0].core_chain_locked_height, 1234U);

    llmq.answer = {interfaces::LLMQ::PlatformQuorumStatus::INVALID, {}};
    BOOST_CHECK(context->QuorumPublicKey(6, quorum_hash, 99, key) == dash_sdk::QuorumLookup::Invalid);
    BOOST_CHECK_EQUAL(llmq.lookups.back().core_chain_locked_height, 99U);
    llmq.answer = {interfaces::LLMQ::PlatformQuorumStatus::UNAVAILABLE, {}};
    BOOST_CHECK(context->QuorumPublicKey(6, quorum_hash, 99, key) == dash_sdk::QuorumLookup::Unavailable);
    llmq.answer = {interfaces::LLMQ::PlatformQuorumStatus::FOUND, std::vector<uint8_t>(47, 0xab)};
    BOOST_CHECK(context->QuorumPublicKey(6, quorum_hash, 99, key) == dash_sdk::QuorumLookup::Unavailable);
    llmq.answer = {interfaces::LLMQ::PlatformQuorumStatus::INVALID, {}};
    llmq.fail = true;
    BOOST_CHECK(context->QuorumPublicKey(6, quorum_hash, 99, key) == dash_sdk::QuorumLookup::Unavailable);
    BOOST_CHECK_EQUAL(llmq.lookups.size(), 5U);

    uint32_t height;
    BOOST_CHECK(!context->PlatformActivationHeight(height));

    // The client reports a proof the node proves bad as REJECTED, and one it
    // cannot judge as UNAVAILABLE.
    dash_sdk::IdResult result;
    result.status.kind = dash_sdk::StatusKind::Rejected;
    BOOST_CHECK(marshal::FromFfi(result).status.kind == StatusKind::REJECTED);
    result.status.kind = dash_sdk::StatusKind::Unavailable;
    BOOST_CHECK(marshal::FromFfi(result).status.kind == StatusKind::UNAVAILABLE);
}

//! Each call reads one page: the requests it holds, whether more follow and
//! where they start. A request with an index above 32 bits is left out but
//! still moves the cursor. A failed page keeps the binding's message.
BOOST_AUTO_TEST_CASE(contact_requests_one_page_per_read)
{
    constexpr uint64_t above{uint64_t{1} << 32};
    const auto request{[](uint64_t created_at_ms, uint64_t sender = 1, uint64_t recipient = 1, uint64_t account = 0) {
        dash_sdk::ContactRequest r;
        r.created_at_ms = created_at_ms;
        r.sender_key_index = sender;
        r.recipient_key_index = recipient;
        r.account_reference = account;
        return r;
    }};
    const auto page{[](std::vector<dash_sdk::ContactRequest> requests, bool complete,
                       dash_sdk::StatusKind kind = dash_sdk::StatusKind::Ok, std::string message = {}) {
        dash_sdk::ContactRequestsResult result;
        result.status.kind = kind;
        result.status.message = message;
        for (auto& r : requests)
            result.requests.push_back(std::move(r));
        result.complete = complete;
        return result;
    }};

    const auto first{marshal::FromFfi(page({request(10), request(20, above)}, false), 5)};
    BOOST_REQUIRE(first.ok());
    BOOST_REQUIRE_EQUAL(first.value.requests.size(), 1U);
    BOOST_CHECK_EQUAL(first.value.requests[0].created_at_ms, 10U);
    BOOST_CHECK(!first.value.complete);
    BOOST_CHECK_EQUAL(first.value.next_since_ms, 20U);

    const auto last{marshal::FromFfi(page({request(30), request(40, 1, above), request(50, 1, 1, above)}, true), 20)};
    BOOST_REQUIRE(last.ok());
    BOOST_REQUIRE_EQUAL(last.value.requests.size(), 1U);
    BOOST_CHECK_EQUAL(last.value.requests[0].created_at_ms, 30U);
    BOOST_CHECK(last.value.complete);
    BOOST_CHECK_EQUAL(last.value.next_since_ms, 50U);

    const auto none{marshal::FromFfi(page({}, true), 20)};
    BOOST_REQUIRE(none.ok());
    BOOST_CHECK(none.value.complete);
    BOOST_CHECK_EQUAL(none.value.next_since_ms, 20U);

    const std::string saturated{"contact requests: more than 100 share created_at 20"};
    const auto failed{marshal::FromFfi(page({}, false, dash_sdk::StatusKind::Unavailable, saturated), 20)};
    BOOST_CHECK(failed.status.kind == StatusKind::UNAVAILABLE);
    BOOST_CHECK_EQUAL(failed.status.message, saturated);
    BOOST_CHECK(failed.value.requests.empty());

    // An incomplete page that does not move the cursor would be read again
    // forever.
    BOOST_CHECK(marshal::FromFfi(page({request(10)}, false), 10).status.kind == StatusKind::UNAVAILABLE);
    BOOST_CHECK(marshal::FromFfi(page({}, false), 0).status.kind == StatusKind::UNAVAILABLE);
}

//! A funding call reports what it did with the lock whatever its status, and
//! its status is OK exactly when the lock is settled. An outcome the binding
//! adds later settles nothing. Through the real client, a floor above
//! MAX_FUNDING_FEE_INCREASE builds and signs nothing and answers
//! PLATFORM_BEHIND_CORE, while one at it is attempted.
BOOST_AUTO_TEST_CASE(funding_results_keep_their_report)
{
    dash_sdk::RegisterIdentityResult registered;
    registered.status.kind = dash_sdk::StatusKind::Consensus;
    registered.status.consensus_code = 10506;
    registered.funding = {dash_sdk::FundingOutcome::PlatformBehindCore, 15};
    auto registration{marshal::FromFfi(registered)};
    BOOST_CHECK(registration.status.kind == StatusKind::CONSENSUS);
    BOOST_CHECK_EQUAL(registration.status.consensus_code, 10506U);
    BOOST_CHECK(registration.funding.outcome == FundingOutcome::PLATFORM_BEHIND_CORE);
    BOOST_CHECK_EQUAL(registration.funding.last_user_fee_increase, 15);

    registered.status.kind = dash_sdk::StatusKind::Ok;
    registered.identity.id.fill(7);
    registered.identity.balance = 2'999'000'000;
    registered.funding = {dash_sdk::FundingOutcome::Ok, 3};
    registration = marshal::FromFfi(registered);
    BOOST_REQUIRE(registration.ok());
    BOOST_CHECK(registration.value.id == registered.identity.id);
    BOOST_CHECK_EQUAL(registration.value.balance, 2'999'000'000U);
    BOOST_CHECK(registration.funding.outcome == FundingOutcome::OK);
    BOOST_CHECK_EQUAL(registration.funding.last_user_fee_increase, 3);

    dash_sdk::TopUpIdentityResult topped_up;
    topped_up.status.kind = dash_sdk::StatusKind::Unavailable;
    topped_up.funding = {dash_sdk::FundingOutcome::Submitted, 2};
    auto top_up{marshal::FromFfi(topped_up)};
    BOOST_CHECK(top_up.status.kind == StatusKind::UNAVAILABLE);
    BOOST_CHECK(top_up.funding.outcome == FundingOutcome::SUBMITTED);
    BOOST_CHECK_EQUAL(top_up.funding.last_user_fee_increase, 2);
    topped_up.status.kind = dash_sdk::StatusKind::Ok;
    topped_up.balance = 5'000;
    topped_up.funding = {dash_sdk::FundingOutcome::Ok, 2};
    top_up = marshal::FromFfi(topped_up);
    BOOST_REQUIRE(top_up.ok());
    BOOST_CHECK_EQUAL(top_up.value, 5'000U);

    const std::pair<dash_sdk::FundingOutcome, FundingOutcome> outcomes[]{
        {dash_sdk::FundingOutcome::NeedsChainLockProof, FundingOutcome::NEEDS_CHAINLOCK_PROOF},
        {dash_sdk::FundingOutcome::PlatformBehindCore, FundingOutcome::PLATFORM_BEHIND_CORE},
        {dash_sdk::FundingOutcome::ReportedConsumed, FundingOutcome::REPORTED_CONSUMED},
        {dash_sdk::FundingOutcome::Rejected, FundingOutcome::REJECTED},
        {dash_sdk::FundingOutcome::Unavailable, FundingOutcome::UNAVAILABLE},
        {dash_sdk::FundingOutcome::Submitted, FundingOutcome::SUBMITTED},
        {static_cast<dash_sdk::FundingOutcome>(0xEE), FundingOutcome::UNAVAILABLE},
    };
    topped_up.status.kind = dash_sdk::StatusKind::Unavailable;
    for (const auto& [ffi, outcome] : outcomes) {
        topped_up.funding = {ffi, 9};
        top_up = marshal::FromFfi(topped_up);
        BOOST_CHECK(top_up.status.kind == StatusKind::UNAVAILABLE);
        BOOST_CHECK(top_up.funding.outcome == outcome);
        BOOST_CHECK_EQUAL(top_up.funding.last_user_fee_increase, 9);
    }

    // OK with an outcome that is not OK, or the reverse, settles nothing.
    topped_up.status.kind = dash_sdk::StatusKind::Ok;
    topped_up.balance = 5'000;
    for (const auto ffi : {static_cast<dash_sdk::FundingOutcome>(0xEE), dash_sdk::FundingOutcome::Submitted}) {
        topped_up.funding = {ffi, 9};
        top_up = marshal::FromFfi(topped_up);
        BOOST_CHECK(top_up.status.kind == StatusKind::INTERNAL);
        BOOST_CHECK(top_up.funding.outcome == FundingOutcome::UNAVAILABLE);
        BOOST_CHECK_EQUAL(top_up.value, 0U);
    }
    registered.status.kind = dash_sdk::StatusKind::Unavailable;
    registered.status.message = std::string{"no endpoint answered"};
    registered.funding = {dash_sdk::FundingOutcome::Ok, 3};
    registration = marshal::FromFfi(registered);
    BOOST_CHECK(registration.status.kind == StatusKind::INTERNAL);
    BOOST_CHECK(registration.funding.outcome == FundingOutcome::UNAVAILABLE);
    BOOST_CHECK(registration.status.message.ends_with(": no endpoint answered"));

    TestLLMQ llmq;
    TestExecutor executor;
    auto client{MakeSdkPlatformClient(ClientConfig{}, llmq, executor.executor())};
    BOOST_REQUIRE(client);
    std::optional<FundingReport> above_cap;
    {
        // Every signature takes cs_wallet: held here, a signer the call ran
        // would stop it from answering.
        LOCK(m_wallet->cs_wallet);
        client->topUpIdentity(Identifier{}, ChainAssetLockProof{}, wallet::TopupFundingKey{0},
                              uint16_t{MAX_FUNDING_FEE_INCREASE + 1}, Signing{*m_iface, 0, nullptr},
                              [&](FundingResult<uint64_t> result) { above_cap = result.funding; });
        BOOST_REQUIRE(executor.WaitFor(1));
    }
    executor.Drain();
    BOOST_REQUIRE(above_cap);
    BOOST_CHECK(above_cap->outcome == FundingOutcome::PLATFORM_BEHIND_CORE);
    BOOST_CHECK_EQUAL(above_cap->last_user_fee_increase, MAX_FUNDING_FEE_INCREASE);

    std::optional<FundingReport> at_cap;
    client->topUpIdentity(Identifier{}, ChainAssetLockProof{}, wallet::TopupFundingKey{0}, MAX_FUNDING_FEE_INCREASE,
                          Signing{*m_iface, 0, nullptr}, [&](FundingResult<uint64_t> result) { at_cap = result.funding; });
    BOOST_REQUIRE(executor.WaitFor(1));
    executor.Drain();
    BOOST_REQUIRE(at_cap);
    BOOST_CHECK(at_cap->outcome != FundingOutcome::PLATFORM_BEHIND_CORE);
    client->shutdown();
}

//! A proved record maps to its state; a failed read or a state the binding
//! adds later is no state.
BOOST_AUTO_TEST_CASE(asset_lock_states)
{
    dash_sdk::AssetLockStateResult result;
    result.status.kind = dash_sdk::StatusKind::Ok;
    result.state = dash_sdk::AssetLockState::NotPresent;
    auto state{marshal::FromFfi(result)};
    BOOST_REQUIRE(state.ok());
    BOOST_CHECK(state.value.state == AssetLockState::State::NOT_PRESENT);

    result.state = dash_sdk::AssetLockState::PartiallyConsumed;
    result.remaining_credits = 1'500;
    state = marshal::FromFfi(result);
    BOOST_REQUIRE(state.ok());
    BOOST_CHECK(state.value.state == AssetLockState::State::PARTIALLY_CONSUMED);
    BOOST_CHECK_EQUAL(state.value.remaining_credits, 1'500U);

    result.state = dash_sdk::AssetLockState::FullyConsumed;
    result.remaining_credits = 0;
    state = marshal::FromFfi(result);
    BOOST_REQUIRE(state.ok());
    BOOST_CHECK(state.value.state == AssetLockState::State::FULLY_CONSUMED);

    result.state = static_cast<dash_sdk::AssetLockState>(0xEE);
    BOOST_CHECK(marshal::FromFfi(result).status.kind == StatusKind::INTERNAL);
    result.status.kind = dash_sdk::StatusKind::Unavailable;
    result.state = dash_sdk::AssetLockState::NotPresent;
    BOOST_CHECK(marshal::FromFfi(result).status.kind == StatusKind::UNAVAILABLE);
}

//! A refused contested label is CONTESTED, and a status the binding adds
//! later is INTERNAL. REFUSE is the default policy, and CAP states its cap,
//! 0 included.
BOOST_AUTO_TEST_CASE(contest_policy_and_status)
{
    dash_sdk::IdResult result;
    result.status.kind = dash_sdk::StatusKind::Contested;
    BOOST_CHECK(marshal::FromFfi(result).status.kind == StatusKind::CONTESTED);
    result.status.kind = static_cast<dash_sdk::StatusKind>(0xEE);
    BOOST_CHECK(marshal::FromFfi(result).status.kind == StatusKind::INTERNAL);

    BOOST_CHECK(marshal::ToFfi(ContestPolicy{}).kind == dash_sdk::ContestPolicyKind::Refuse);
    for (const uint64_t cap : {uint64_t{0}, uint64_t{5'000'000}}) {
        const auto policy{marshal::ToFfi(ContestPolicy{ContestPolicy::Kind::CAP, cap})};
        BOOST_CHECK(policy.kind == dash_sdk::ContestPolicyKind::Cap);
        BOOST_CHECK_EQUAL(policy.cap, cap);
    }
}

//! The identity an asset lock registers: sha256d(txid || vout), as Platform's
//! dpp derives it (the same vector as dash-sdk-cxx's own test).
BOOST_AUTO_TEST_CASE(identity_id_from_asset_lock)
{
    uint256 txid;
    for (size_t i = 0; i < txid.size(); ++i)
        *(txid.begin() + i) = i;
    const auto id{IdentityIdFromAssetLock(COutPoint{txid, 1})};
    BOOST_REQUIRE(id);
    BOOST_CHECK_EQUAL(HexStr(*id), "527067ec1a728fb41326298d1cbef291967436d42ffc3df726f228f3e9c503fd");
    BOOST_CHECK(IdentityIdFromAssetLock(COutPoint{txid, 0}) != id);
}

//! An unusable proxy gives no client: it never falls back to a direct
//! connection.
BOOST_AUTO_TEST_CASE(unusable_proxy_gives_no_client)
{
    TestLLMQ llmq;
    TestExecutor executor;
    ClientConfig config;
    config.proxy = ProxyConfig{std::string{}, true};
    BOOST_CHECK(!MakeSdkPlatformClient(config, llmq, executor.executor()));
}

BOOST_AUTO_TEST_SUITE_END()
