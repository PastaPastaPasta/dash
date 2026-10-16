// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/platformtests.h>

#include <qt/test/masternodetestutil.h>

#include <common/args.h>
#include <common/settings.h>
#include <evo/assetlocktx.h>
#include <evo/dmn_types.h>
#include <evo/specialtx.h>
#include <evo/types.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <netbase.h>
#include <qt/clientfeeds.h>
#include <qt/guiutil.h>
#include <qt/masternodemodel.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformpage.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <script/standard.h>
#include <test/util/platform_client.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/overloaded.h>
#include <util/time.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using MasternodeTestUtil::GuiModels;
using MasternodeTestUtil::MakeCoinbaseWallet;
using MasternodeTestUtil::MakeTestWallet;
using MasternodeTestUtil::WalletGuard;
using Gate = PlatformService::Gate;

namespace {
CService Ipv4Evonode() { return LookupNumeric("203.0.113.7", 443); }
CService Ipv4Evonode2() { return LookupNumeric("203.0.113.8", 443); }
CService Ipv6Evonode() { return LookupNumeric("2001:db8::7", 443); }
CService OnionEvonode() { return LookupNumeric("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion", 443); }
CService I2pEvonode() { return LookupNumeric("udhdrtrcetjm5sxzskjyr5ztpeszydbh4dpl3pl4utgqqw2v4jna.b32.i2p", 443); }
//! -proxy=127.0.0.1:9050 with -proxyrandomize.
platform::ProxyConfig Tor() { return {LookupNumeric("127.0.0.1", 9050), true}; }

//! A masternode list entry: an evonode with these Platform HTTPS endpoints,
//! or a regular masternode with none.
class ListedMasternode final : public interfaces::MnEntry
{
public:
    ListedMasternode(std::vector<CService> https, bool banned) :
        interfaces::MnEntry(CDeterministicMNCPtr{}),
        m_https(std::move(https)),
        m_banned(banned)
    {
    }

    bool isBanned() const override { return m_banned; }
    CService getNetInfoPrimary() const override { return {}; }
    std::vector<CService> getPlatformHTTPSAddrs() const override { return m_https; }
    MnType getType() const override { return m_https.empty() ? MnType::Regular : MnType::Evo; }
    UniValue toJson() const override { return UniValue{UniValue::VOBJ}; }
    const CKeyID& getKeyIdOwner() const override { return m_key; }
    const CKeyID& getKeyIdVoting() const override { return m_key; }
    const COutPoint& getCollateralOutpoint() const override { return m_collateral; }
    const CScript& getScriptPayout() const override { return m_script; }
    std::vector<CScript> getScriptPayouts() const override { return {}; }
    const CScript& getScriptOperatorPayout() const override { return m_script; }
    const int32_t& getLastPaidHeight() const override { return m_height; }
    const int32_t& getPoSePenalty() const override { return m_height; }
    const int32_t& getRegisteredHeight() const override { return m_height; }
    const uint16_t& getOperatorReward() const override { return m_reward; }
    const uint256& getProTxHash() const override { return m_hash; }

private:
    std::vector<CService> m_https;
    bool m_banned;
    CKeyID m_key;
    COutPoint m_collateral;
    CScript m_script;
    int32_t m_height{0};
    uint16_t m_reward{0};
    uint256 m_hash;
};

std::shared_ptr<MasternodeEntry> Listed(std::vector<CService> https, bool banned = false)
{
    return std::make_shared<MasternodeEntry>(std::make_shared<ListedMasternode>(std::move(https), banned),
                                             "collateral", /*next_payment_height=*/0);
}

//! 110 blocks: the coinbase key's first coins are mature.
struct MatureChain : TestChain100Setup {
    explicit MatureChain(ChainType chain_type) :
        TestChain100Setup{chain_type}
    {
        mineBlocks(10);
    }
};

//! On a MatureChain the wallet holds the coinbase key's coins.
template <typename ChainSetup>
std::shared_ptr<wallet::CWallet> MakeFixtureWallet(interfaces::Node& node, wallet::WalletContext& context,
                                                   const ChainSetup& chain)
{
    if constexpr (std::is_same_v<ChainSetup, MatureChain>) {
        return MakeCoinbaseWallet(node, context, chain, "platform", /*encrypt=*/false);
    } else {
        return MakeTestWallet(node, context, "platform");
    }
}

//! One test's node, wallet and models; the wallet has started DashPay unless
//! told otherwise. The node context is reset only after the models are gone.
template <typename ChainSetup>
struct FixtureWithChain {
    ChainSetup chain{ChainType::REGTEST};
    struct ContextReset {
        interfaces::Node& node;
        ~ContextReset() { node.setContext(nullptr); }
    } reset;
    interfaces::Node& node;
    wallet::WalletContext& context;
    std::shared_ptr<wallet::CWallet> wallet;
    WalletGuard guard;
    GuiModels models;
    WalletModel wallet_model;

    explicit FixtureWithChain(interfaces::Node& node_in, bool consented = true) :
        reset{node_in},
        node(node_in),
        context((node.setContext(&chain.m_node), *node.walletLoader().context())),
        wallet(MakeFixtureWallet(node, context, chain)),
        guard(context, wallet),
        models(node),
        wallet_model(interfaces::MakeWallet(context, wallet), models.client)
    {
        if (consented) Assert(wallet_model.wallet().writePlatformData(PlatformService::CONSENT_RECORD, {1}));
    }

    void attach(PlatformPage& page)
    {
        page.setWalletModel(&wallet_model);
        page.setClientModel(&models.client);
    }
    //! The masternode feed read the list again (on masternodeListChanged).
    void listRead() { Q_EMIT models.client.feedMasternode()->dataReady(); }
    //! The test node has no UI interface: the notification is sent by hand.
    void setNetworkActive(bool active)
    {
        node.setNetworkActive(active);
        Q_EMIT models.client.networkActiveChanged(active);
    }
};
//! A synced node: 100 recent blocks.
using Fixture = FixtureWithChain<TestChain100Setup>;
//! A node in initial block download: only the old genesis block.
using SyncingFixture = FixtureWithChain<TestingSetup>;
//! A synced node and a wallet with coins to fund asset locks with.
using FundedFixture = FixtureWithChain<MatureChain>;

//! What a service reads instead of the node, and the clients it made.
struct FakeSources {
    PlatformNetworkSettings settings;
    std::vector<CService> evonodes{Ipv4Evonode()};
    bool refuse{false};
    //! Run on each new client, to script the answers to the calls it gets first.
    std::function<void(FakePlatformClient&)> script;
    std::vector<platform::ClientConfig> made; //!< one per client asked for
    FakePlatformClient* fake{nullptr};        //!< the latest one

    PlatformService::Sources get()
    {
        return {
            [this](const platform::ClientConfig& config, platform::Executor) -> std::unique_ptr<platform::PlatformClient> {
                made.push_back(config);
                if (refuse) return nullptr;
                auto client{std::make_unique<FakePlatformClient>()};
                fake = client.get();
                if (script) script(*client);
                return client;
            },
            [this] { return evonodes; },
            [this] { return settings; },
        };
    }
};

//! A read whose callback owns the result: it expires once the client drops
//! the call without calling back.
std::weak_ptr<int> PendingRead(platform::PlatformClient& client)
{
    auto owner{std::make_shared<int>(0)};
    client.getIdentity({}, [owner](platform::Result<platform::Identity>) { Assert(false); });
    return owner;
}

//! A tracked write whose callback owns the result and sets `done` when it
//! runs: it expires if the client drops the call instead.
std::weak_ptr<int> PendingWrite(PlatformService& service, interfaces::Wallet& wallet, bool& done)
{
    auto owner{std::make_shared<int>(0)};
    service.client()->registerName("alice", {}, 0, platform::ContestPolicy{}, platform::Signing{wallet, 0, nullptr},
                                   service.trackWrite<platform::Identifier>(
                                       [owner, &done](platform::Result<platform::Identifier>) { done = true; }));
    return owner;
}

QStringList VisibleTexts(const QWidget& page)
{
    QStringList texts;
    for (const auto* label : page.findChildren<QLabel*>()) {
        if (label->isVisibleTo(&page)) texts << label->text();
    }
    return texts;
}

QPushButton* VisibleButton(const QWidget& page, const char* name)
{
    auto* button{page.findChild<QPushButton*>(name)};
    return button && button->isVisibleTo(&page) ? button : nullptr;
}

platform::Result<platform::Identity> Found(uint8_t id, uint64_t balance)
{
    platform::Result<platform::Identity> result;
    result.status.kind = platform::StatusKind::OK;
    result.value.id.fill(id);
    result.value.balance = balance;
    return result;
}

platform::Result<platform::Identity> Found(const platform::Identifier& id, uint64_t balance)
{
    auto result{Found(0, balance)};
    result.value.id = id;
    return result;
}

platform::Result<platform::Identity> NotFound()
{
    platform::Result<platform::Identity> result;
    result.status.kind = platform::StatusKind::NOT_FOUND;
    return result;
}

template <typename T>
platform::Result<T> Unavailable()
{
    platform::Result<T> result;
    result.status.kind = platform::StatusKind::UNAVAILABLE;
    return result;
}

//! A funding call's answer: the status the binding gives with the outcome.
template <typename T>
platform::FundingResult<T> Funded(platform::FundingOutcome outcome, uint16_t last_fee_increase, T value = {})
{
    using Outcome = platform::FundingOutcome;
    platform::FundingResult<T> result;
    result.funding = {outcome, last_fee_increase};
    switch (outcome) {
    case Outcome::OK:
        result.status.kind = platform::StatusKind::OK;
        result.value = std::move(value);
        break;
    case Outcome::PLATFORM_BEHIND_CORE:
        result.status = {platform::StatusKind::CONSENSUS, 10506, "core chain locked height too high"};
        break;
    case Outcome::REPORTED_CONSUMED:
    case Outcome::REJECTED:
        result.status = {platform::StatusKind::CONSENSUS, 10504, "asset lock outpoint already consumed"};
        break;
    case Outcome::NEEDS_CHAINLOCK_PROOF:
    case Outcome::UNAVAILABLE:
    case Outcome::SUBMITTED:
        result.status.kind = platform::StatusKind::UNAVAILABLE;
        break;
    }
    return result;
}

platform::Result<platform::AssetLockState> LockState(platform::AssetLockState::State state)
{
    platform::Result<platform::AssetLockState> result;
    result.status.kind = platform::StatusKind::OK;
    result.value.state = state;
    return result;
}

//! The identity a registration lock's credit output registers.
platform::Identifier Registered(const uint256& txid) { return *Assert(platform::IdentityIdFromAssetLock({txid, 0})); }

//! Sets the mock time while it lives.
struct MockTime {
    explicit MockTime(std::chrono::seconds time) { SetMockTime(time); }
    ~MockTime() { SetMockTime(0); }
};

//! The node has the asset lock's InstantSend lock at once.
std::optional<platform::AssetLockProof> InstantProof(const CTransactionRef& tx)
{
    return platform::InstantAssetLockProof{tx, {1}, 0};
}

//! Confirms, or declines, each asset lock the flow would send, and keeps
//! what it was asked.
struct Confirmations {
    bool accept{true};
    std::vector<std::pair<CAmount, CAmount>> asked; //!< amount, fee
    //! What else happens while the dialog is open.
    std::function<void()> meanwhile;

    IdentityFlow::Confirm get()
    {
        return [this](CAmount amount, CAmount fee) {
            asked.emplace_back(amount, fee);
            if (meanwhile) meanwhile();
            return accept;
        };
    }
};

uint256 ProofTxid(const platform::AssetLockProof& proof)
{
    return std::visit(util::Overloaded{[](const platform::InstantAssetLockProof& p) { return p.transaction->GetHash(); },
                                       [](const platform::ChainAssetLockProof& p) { return p.out_point.hash; }},
                      proof);
}

//! The key the asset lock's credit output pays, as a script.
CScript CreditScript(interfaces::Wallet& wallet, const uint256& txid)
{
    const CTransactionRef tx{wallet.getTx(txid)};
    Assert(tx && tx->nType == TRANSACTION_ASSET_LOCK);
    return Assert(GetTxPayload<CAssetLockPayload>(*tx))->getCreditOutputs().at(0).scriptPubKey;
}

CScript FundingScript(interfaces::Wallet& wallet, const wallet::PlatformKeyRequest& key)
{
    const auto pubkey{wallet.getPlatformPubKey(key)};
    Assert(pubkey);
    return GetScriptForDestination(PKHash{pubkey.value});
}

//! Restores the ArgsManager settings a test forced.
struct SettingsRestore {
    common::Settings saved;
    SettingsRestore()
    {
        gArgs.LockSettings([&](common::Settings& settings) { saved = settings; });
    }
    ~SettingsRestore()
    {
        gArgs.LockSettings([&](common::Settings& settings) { settings = saved; });
    }
};

//! Makes IPv4 and IPv6 unreachable while it lives.
struct NoClearnet {
    NoClearnet()
    {
        g_reachable_nets.Remove(NET_IPV4);
        g_reachable_nets.Remove(NET_IPV6);
    }
    ~NoClearnet()
    {
        g_reachable_nets.Add(NET_IPV4);
        g_reachable_nets.Add(NET_IPV6);
    }
};
} // namespace

//! The route follows Core's rules for its own connections: -proxy (or none)
//! for IPv4 and IPv6, onion evonodes only through that same proxy, the onion
//! proxy alone under -onlynet=onion, never I2P or CJDNS; and the settings
//! are read from the node's reachable networks.
void PlatformTests::routeSelection()
{
    const platform::ProxyConfig other{LookupNumeric("127.0.0.1", 9150), true};
    const platform::ProxyConfig unix_socket{std::string{"/run/tor/socks"}, false};

    struct Case {
        const char* name;
        bool ipv4, ipv6, onion;
        std::optional<platform::ProxyConfig> clearnet_proxy, onion_proxy;
        bool routed;
        std::optional<platform::ProxyConfig> proxy;
        bool allows_ipv4, allows_ipv6, allows_onion;
    };
    const std::vector<Case> cases{
        {"defaults", true, true, true, std::nullopt, std::nullopt, true, std::nullopt, true, true, false},
        {"-proxy", true, true, true, Tor(), Tor(), true, Tor(), true, true, true},
        {"-proxy -onion", true, true, true, Tor(), other, true, Tor(), true, true, false},
        {"-proxy -noonion", true, true, false, Tor(), std::nullopt, true, Tor(), true, true, false},
        {"-proxy=unix:", true, true, true, unix_socket, unix_socket, true, unix_socket, true, true, true},
        {"-onlynet=ipv4", true, false, false, std::nullopt, std::nullopt, true, std::nullopt, true, false, false},
        {"-onlynet=onion -onion", false, false, true, std::nullopt, Tor(), true, Tor(), false, false, true},
        {"-onlynet=onion, no onion proxy yet", false, false, true, std::nullopt, std::nullopt, false, std::nullopt,
         false, false, false},
        {"-onlynet=i2p", false, false, false, std::nullopt, std::nullopt, false, std::nullopt, false, false, false},
    };
    for (const Case& c : cases) {
        PlatformNetworkSettings settings;
        settings.ipv4 = c.ipv4;
        settings.ipv6 = c.ipv6;
        settings.onion = c.onion;
        settings.clearnet_proxy = c.clearnet_proxy;
        settings.onion_proxy = c.onion_proxy;
        const auto route{PlatformRoute::Choose(settings)};
        QVERIFY2(route.has_value() == c.routed, c.name);
        if (!route) continue;
        QVERIFY2(route->proxy == c.proxy, c.name);
        QVERIFY2(route->allows(Ipv4Evonode()) == c.allows_ipv4, c.name);
        QVERIFY2(route->allows(Ipv6Evonode()) == c.allows_ipv6, c.name);
        QVERIFY2(route->allows(OnionEvonode()) == c.allows_onion, c.name);
        QVERIFY2(!route->allows(I2pEvonode()), c.name);
    }

    // The node's reachable networks, not -onlynet itself.
    const PlatformNetworkSettings defaults{PlatformNetworkSettings::Read(m_node)};
    QVERIFY(defaults.ipv4 && defaults.ipv6);
    QVERIFY(!defaults.clearnet_proxy && !defaults.onion_proxy);
    const NoClearnet no_clearnet;
    const PlatformNetworkSettings onion_only{PlatformNetworkSettings::Read(m_node)};
    QVERIFY(!onion_only.ipv4 && !onion_only.ipv6);
}

//! -connect (with a peer, 0 or negated), -maxconnections=0 and
//! -walletbroadcast=0 disable DashPay for a wallet that started it: no
//! client is made, and the tab names the setting, -blocksonly when that is
//! what turned broadcasting off. So does a wallet with no HD seed to derive
//! Platform keys from.
void PlatformTests::settingsDisableDashPay()
{
    struct Case {
        const char* name;
        common::SettingsValue value;
        Gate gate;
    };
    const std::vector<Case> cases{
        {"connect", "203.0.113.1", Gate::OUTBOUND_DISABLED}, {"connect", "0", Gate::OUTBOUND_DISABLED},
        {"connect", false, Gate::OUTBOUND_DISABLED},         {"maxconnections", "0", Gate::OUTBOUND_DISABLED},
        {"walletbroadcast", "0", Gate::BROADCAST_DISABLED},
    };
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    for (const Case& c : cases) {
        const SettingsRestore restore;
        gArgs.LockSettings([&](common::Settings& settings) { settings.forced_settings[c.name] = c.value; });
        FakeSources sources;
        PlatformPage page{sources.get()};
        f.attach(page);
        QVERIFY2(sources.made.empty(), c.name);
        QVERIFY2(page.service()->gate() == c.gate, c.name);
        QVERIFY2(VisibleTexts(page).join(' ').contains(QString{"-"} + c.name), c.name);
    }
    {
        // As the wallet's parameter interaction leaves them.
        const SettingsRestore restore;
        gArgs.LockSettings([](common::Settings& settings) {
            settings.forced_settings["blocksonly"] = true;
            settings.forced_settings["walletbroadcast"] = false;
        });
        FakeSources sources;
        PlatformPage page{sources.get()};
        f.attach(page);
        QVERIFY(sources.made.empty());
        QVERIFY(page.service()->gate() == Gate::BROADCAST_DISABLED);
        QVERIFY(VisibleTexts(page).join(' ').contains("-blocksonly"));
    }

    // The same wallet with the defaults starts its client.
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    QCOMPARE(sources.made.size(), size_t{1});

    // A wallet without an HD seed can't derive its Platform keys.
    auto blank{
        std::make_shared<wallet::CWallet>(f.node.context()->chain.get(), "blank", wallet::CreateMockableWalletDatabase())};
    blank->LoadWallet();
    blank->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    wallet::AddWallet(f.context, blank);
    WalletGuard blank_guard{f.context, blank};
    WalletModel blank_model{interfaces::MakeWallet(f.context, blank), f.models.client};
    QVERIFY(blank_model.wallet().writePlatformData(PlatformService::CONSENT_RECORD, {1}));
    FakeSources blank_sources;
    PlatformService ineligible{blank_model, f.models.client, blank_sources.get()};
    QVERIFY(blank_sources.made.empty());
    QVERIFY(ineligible.gate() == Gate::WALLET);
    QVERIFY(!ineligible.reason().isEmpty());
}

//! Until the first DashPay click nothing is made that could contact
//! Platform, whatever the node does; the click records the wallet's consent,
//! which later sessions find, and turning DashPay off erases it.
void PlatformTests::nothingBeforeConsent()
{
    Fixture f{m_node, /*consented=*/false};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformPage page{sources.get()};
    f.attach(page);
    f.listRead();
    f.setNetworkActive(false);
    f.setNetworkActive(true);
    QVERIFY(sources.made.empty());
    QVERIFY(page.service()->gate() == Gate::NO_CONSENT);
    QVERIFY(page.findChild<QLabel*>("dashPayNotice")->isVisibleTo(&page));
    QVERIFY(VisibleButton(page, "dashPayStop") == nullptr);

    QPushButton* start{VisibleButton(page, "dashPayStart")};
    QVERIFY(start != nullptr);
    start->click();
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode()});
    QVERIFY(!sources.made.back().proxy);
    QVERIFY(page.service()->client() != nullptr);
    QVERIFY(VisibleButton(page, "dashPayStart") == nullptr);

    // The consent is the wallet's: the next session starts without a click.
    {
        FakeSources next;
        PlatformService service{f.wallet_model, f.models.client, next.get()};
        QCOMPARE(next.made.size(), size_t{1});
    }

    const auto read{PendingRead(*page.service()->client())};
    QPushButton* stop{VisibleButton(page, "dashPayStop")};
    QVERIFY(stop != nullptr);
    stop->click();
    QVERIFY(read.expired());
    QVERIFY(page.service()->client() == nullptr);
    QVERIFY(VisibleButton(page, "dashPayStart") != nullptr);
    FakeSources after;
    PlatformService service{f.wallet_model, f.models.client, after.get()};
    QVERIFY(after.made.empty());
}

//! A node in initial block download has a stale masternode list: no client.
void PlatformTests::syncingNodeStartsNoClient()
{
    SyncingFixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    f.listRead();
    QVERIFY(sources.made.empty());
    QVERIFY(service.gate() == Gate::SYNCING);
}

//! Turning network activity off destroys the client at once, a write in
//! flight included; turning it on makes a client from the current list.
void PlatformTests::networkOffDestroysClient()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    bool written{false};
    const auto write{PendingWrite(service, f.wallet_model.wallet(), written)};

    QSignalSpy changed{&service, &PlatformService::changed};
    f.setNetworkActive(false);
    QVERIFY(write.expired());
    QVERIFY(!written);
    QVERIFY(service.client() == nullptr);
    QVERIFY(service.gate() == Gate::NETWORK_OFF);
    QCOMPARE(changed.size(), 1);

    sources.evonodes = {Ipv4Evonode2()};
    f.setNetworkActive(true);
    QVERIFY(service.client() != nullptr);
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode2()});
}

//! A changed valid evonode set rebuilds an idle client for the new set; an
//! unchanged one keeps it.
void PlatformTests::evonodeChangeRebuildsWhenIdle()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    const auto read{PendingRead(*service.client())};

    f.listRead();
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(!read.expired());

    QSignalSpy changed{&service, &PlatformService::changed};
    sources.evonodes = {Ipv4Evonode(), Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY((sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode(), Ipv4Evonode2()}));
    QVERIFY(read.expired());
    QVERIFY(service.client() != nullptr);
    QCOMPARE(changed.size(), 1);
}

//! A changed set while a write is in flight pauses new calls and lets the
//! write finish on its client; the rebuild comes before any other call. A
//! set that changes back first unpauses the client as it is, and a set with
//! no evonode left waits for the write too.
void PlatformTests::rebuildWaitsForWriteInFlight()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    FakePlatformClient* first{sources.fake};
    bool written{false};
    const auto write{PendingWrite(service, f.wallet_model.wallet(), written)};
    QSignalSpy changed{&service, &PlatformService::changed};

    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QVERIFY(service.client() == nullptr);
    QCOMPARE(changed.size(), 1);
    sources.evonodes = {Ipv4Evonode()};
    f.listRead();
    QVERIFY(service.client() == first);
    QCOMPARE(changed.size(), 2);

    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QVERIFY(service.client() == nullptr);
    QVERIFY(!write.expired());
    QCOMPARE(sources.made.size(), size_t{1});
    first->deliverAll();
    QVERIFY(written);
    QCoreApplication::processEvents();
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode2()});
    QVERIFY(service.client() != nullptr);

    FakePlatformClient* second{sources.fake};
    bool second_written{false};
    const auto second_write{PendingWrite(service, f.wallet_model.wallet(), second_written)};
    sources.evonodes.clear();
    f.listRead();
    QVERIFY(service.client() == nullptr);
    QVERIFY(!second_write.expired());
    second->deliverAll();
    QVERIFY(second_written);
    QCoreApplication::processEvents();
    QVERIFY(service.gate() == Gate::NO_EVONODES);
    QCOMPARE(sources.made.size(), size_t{2});
}

//! With -proxy the client connects through it, keeping -proxyrandomize, to
//! the masternode list's evonodes that the proxy reaches: IPv4, IPv6 and
//! onion, never I2P, and none that is PoSe-banned.
void PlatformTests::clientUsesTheProxy()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    MasternodeData list;
    list.m_entries = {Listed({Ipv4Evonode(), Ipv6Evonode()}), Listed({OnionEvonode(), I2pEvonode()}),
                      Listed({Ipv4Evonode2()}, /*banned=*/true), Listed({})};
    FakeSources sources;
    sources.settings.onion = true;
    sources.settings.clearnet_proxy = sources.settings.onion_proxy = Tor();
    sources.evonodes = PlatformService::ListedEvonodes(list);
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(sources.made.back().proxy == Tor());
    const auto& endpoints{sources.made.back().endpoints};
    QCOMPARE(endpoints.size(), size_t{3});
    QVERIFY(std::find(endpoints.begin(), endpoints.end(), I2pEvonode()) == endpoints.end());
    QVERIFY(std::find(endpoints.begin(), endpoints.end(), Ipv4Evonode2()) == endpoints.end());
}

//! Under -onlynet=onion the Tor controller sets the onion proxy once it has
//! connected; the next read of the list starts the client through it, to
//! onion evonodes only.
void PlatformTests::onionProxyLaterStartsClient()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.settings.ipv4 = sources.settings.ipv6 = false;
    sources.settings.onion = true;
    sources.evonodes = {Ipv4Evonode(), OnionEvonode()};
    PlatformPage page{sources.get()};
    f.attach(page);
    QVERIFY(sources.made.empty());
    QVERIFY(page.service()->gate() == Gate::NO_ROUTE);
    QVERIFY(VisibleTexts(page).contains(page.service()->reason()));

    sources.settings.onion_proxy = Tor();
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(sources.made.back().proxy == Tor());
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{OnionEvonode()});
}

//! A configuration the SDK refuses is said on the tab and not tried again
//! until the evonode set changes; a set that changes back to it waits for a
//! write in flight like any other change.
void PlatformTests::refusedClientIsShownNotRetried()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.refuse = true;
    PlatformPage page{sources.get()};
    f.attach(page);
    QVERIFY(page.service()->gate() == Gate::CLIENT_FAILED);
    QVERIFY(VisibleTexts(page).contains(page.service()->reason()));
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{1});

    sources.refuse = false;
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY(page.service()->client() != nullptr);

    bool written{false};
    const auto write{PendingWrite(*page.service(), f.wallet_model.wallet(), written)};
    sources.evonodes = {Ipv4Evonode()};
    f.listRead();
    QVERIFY(!write.expired());
    QVERIFY(page.service()->client() == nullptr);
    sources.fake->deliverAll();
    QVERIFY(written);
    QCoreApplication::processEvents();
    QVERIFY(page.service()->gate() == Gate::CLIENT_FAILED);
    QCOMPARE(sources.made.size(), size_t{2});
}

//! Shutdown detaches the client model before it and the wallet models are
//! deleted, and unloading a wallet deletes its page: either way consumers
//! are told, and the client goes with the service.
void PlatformTests::detachingClientModelStopsClient()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    std::vector<PlatformService*> told;
    const auto tell = [&told](PlatformService* service) { told.push_back(service); };

    PlatformPage page{sources.get()};
    f.attach(page);
    const auto read{PendingRead(*page.service()->client())};
    QObject::connect(&page, &PlatformPage::serviceChanged, tell);
    page.setClientModel(nullptr);
    QVERIFY(read.expired());
    QVERIFY(page.service() == nullptr);
    QCOMPARE(told, std::vector<PlatformService*>{nullptr});

    told.clear();
    auto unloaded{std::make_unique<PlatformPage>(sources.get())};
    f.attach(*unloaded);
    const auto unloaded_read{PendingRead(*unloaded->service()->client())};
    QObject::connect(unloaded.get(), &PlatformPage::serviceChanged, tell);
    unloaded.reset();
    QVERIFY(unloaded_read.expired());
    QCOMPARE(told, std::vector<PlatformService*>{nullptr});
}

//! A first lookup Platform did not answer, or whose proof this node could
//! not check yet, says so and can be asked again. Creating an identity: the
//! user sees the amount and the exact fee first, and declining sends nothing.
//! Accepting commits an asset lock paying the registration funding key and
//! records it; once the node has a lock for it the identity is registered
//! with that proof, and is then the wallet's and the record is gone.
void PlatformTests::registrationFundsAndRegisters()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Unavailable<platform::Identity>(), NotFound()};
        client.registered_identities = {Funded(platform::FundingOutcome::OK, 0, Found(7, 2'999'000'000).value)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    bool locked{false};
    IdentityFlow flow{service, confirmations.get(),
                      [&locked](const CTransactionRef& tx) { return locked ? InstantProof(tx) : std::nullopt; }};
    sources.fake->deliverAll();
    // UNAVAILABLE includes a proof this node has no quorum data to check yet.
    QVERIFY(!flow.lookedUp());
    QVERIFY(flow.error().contains("local quorum data"));
    flow.lookUp();
    sources.fake->deliverAll();
    QVERIFY(flow.lookedUp());
    QVERIFY(!flow.identity());

    const size_t txs{wallet.getWalletTxs().size()};
    confirmations.accept = false;
    flow.create();
    QCOMPARE(confirmations.asked.size(), size_t{1});
    QCOMPARE(confirmations.asked.back().first, IdentityFlow::REGISTRATION_AMOUNT);
    QVERIFY(confirmations.asked.back().second > 0);
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(!flow.funding());
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{0});

    confirmations.accept = true;
    flow.create();
    const auto funding{flow.funding()};
    QVERIFY(funding && !funding->top_up);
    QCOMPARE(funding->amount, IdentityFlow::REGISTRATION_AMOUNT);
    QVERIFY(CreditScript(wallet, funding->txid) == FundingScript(wallet, wallet::RegistrationFundingKey{0}));
    QVERIFY(flow.step() == IdentityFlow::Step::WAITING_FOR_LOCK);
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{0});
    locked = true;
    Q_EMIT f.models.client.instantSendChanged();
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
    QVERIFY(sources.fake->calls.back().identity_index == 0U);
    QVERIFY(ProofTxid(sources.fake->proofs.back()) == funding->txid);
    QCOMPARE(sources.fake->fee_increase_floors.back(), uint16_t{0});

    sources.fake->deliverAll();
    QVERIFY(flow.identity() && flow.identity()->balance == 2'999'000'000U);
    QVERIFY(!flow.funding());
    QVERIFY(flow.fundings().empty());
    QVERIFY(flow.error().isEmpty());
}

//! A registration whose write was dropped (network off) or whose answer
//! never came (restart) keeps its record: the next session offers Continue,
//! which sends the same asset lock again without funding another, each time
//! starting above any fee increase an earlier submission may have used. A
//! node reporting it consumed proves nothing: the record is checked again,
//! a failed read changes nothing, and the identity its lock registers ends
//! it.
void PlatformTests::continueResumesFundingAfterRestart()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    Confirmations confirmations;
    uint256 txid;
    std::vector<uint16_t> floors;
    {
        FakeSources sources;
        sources.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {NotFound()}; };
        PlatformService service{f.wallet_model, f.models.client, sources.get()};
        IdentityFlow flow{service, confirmations.get(), InstantProof};
        sources.fake->deliverAll();
        flow.create();
        QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
        floors.push_back(sources.fake->fee_increase_floors.back());
        txid = Assert(flow.funding())->txid;

        f.setNetworkActive(false);
        QVERIFY(flow.step() == IdentityFlow::Step::IDLE);
        QVERIFY(flow.funding() && flow.funding()->txid == txid);
        f.setNetworkActive(true);
        sources.fake->deliverAll();
        QVERIFY(flow.lookedUp() && !flow.identity());
        flow.resume();
        QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
        QVERIFY(ProofTxid(sources.fake->proofs.back()) == txid);
        floors.push_back(sources.fake->fee_increase_floors.back());
        // Restart before Platform answered.
    }

    const size_t txs{wallet.getWalletTxs().size()};
    const platform::Identifier registered{Registered(txid)};
    FakeSources sources;
    sources.script = [&registered](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {NotFound()};
        client.registered_identities = {Funded<platform::Identity>(platform::FundingOutcome::REPORTED_CONSUMED, 2)};
        client.identities = {Unavailable<platform::Identity>(), Found(registered, 1'000)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    sources.fake->deliverAll();
    QVERIFY(flow.funding() && flow.funding()->txid == txid);
    flow.resume();
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
    QVERIFY(ProofTxid(sources.fake->proofs.back()) == txid);
    floors.push_back(sources.fake->fee_increase_floors.back());
    QCOMPARE(confirmations.asked.size(), size_t{1});
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(floors == (std::vector<uint16_t>{0, 15, 30}));

    sources.fake->deliverAll();
    QVERIFY(!flow.identity());
    QVERIFY(flow.funding() && flow.funding()->state == IdentityFlow::FundingState::CHECK_AGAIN);
    Q_EMIT f.models.client.chainLockChanged();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{1});
    QVERIFY(flow.funding() && flow.funding()->state == IdentityFlow::FundingState::CHECK_AGAIN);
    Q_EMIT f.models.client.chainLockChanged();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{2});
    QVERIFY(sources.fake->calls.back().argument == HexStr(registered));
    QVERIFY(flow.identity() && flow.identity()->id == registered);
    QVERIFY(flow.fundings().empty());
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
}

//! Each top-up funds an asset lock paying a top-up key not used before,
//! adds it to the identity the wallet found, and shows the new balance. A
//! payment abandoned while its write was dropped is kept, in case it is
//! mined, but not offered to Continue; once the wallet takes it back,
//! Continue sends it.
void PlatformTests::topUpUsesAFreshFundingKey()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(5, 100)};
        const auto balance{Funded<uint64_t>(platform::FundingOutcome::OK, 0, 1'000'000'100)};
        client.top_ups = {balance, balance};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    sources.fake->deliverAll();
    QVERIFY(flow.identity());

    for (uint32_t index : {0U, 1U}) {
        flow.topUp(IdentityFlow::MIN_TOP_UP);
        const auto funding{flow.funding()};
        QVERIFY(funding && funding->top_up == flow.identity()->id);
        QCOMPARE(std::get<wallet::TopupFundingKey>(funding->key).funding_index, index);
        QVERIFY(CreditScript(wallet, funding->txid) == FundingScript(wallet, wallet::TopupFundingKey{index}));
        QVERIFY(sources.fake->calls.back().method == "topUpIdentity");
        QVERIFY(sources.fake->calls.back().argument == HexStr(flow.identity()->id));
        QVERIFY(ProofTxid(sources.fake->proofs.back()) == funding->txid);
        sources.fake->deliverAll();
        QVERIFY(!flow.funding());
    }
    QCOMPARE(flow.identity()->balance, uint64_t{1'000'000'100});
    QCOMPARE(confirmations.asked.back().first, IdentityFlow::MIN_TOP_UP);

    flow.topUp(IdentityFlow::MIN_TOP_UP);
    const uint256 abandoned{Assert(flow.funding())->txid};
    f.setNetworkActive(false);
    QVERIFY(wallet.abandonTransaction(abandoned));
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QVERIFY(!flow.funding());
    QCOMPARE(flow.fundings().size(), size_t{1});
    QVERIFY(flow.inactive(flow.fundings().front()));
    flow.resume();
    QCOMPARE(sources.fake->countCalls("topUpIdentity"), size_t{0});
    QVERIFY(flow.error().isEmpty());

    QVERIFY(wallet.commitTransaction(wallet.getTx(abandoned), {}, {}));
    QVERIFY(flow.funding() && flow.funding()->txid == abandoned);
    flow.resume();
    QCOMPARE(sources.fake->countCalls("topUpIdentity"), size_t{1});
    QVERIFY(ProofTxid(sources.fake->proofs.back()) == abandoned);
    sources.fake->deliverAll();
    QVERIFY(flow.fundings().empty());
}

//! What Platform answers decides what happens to a lock, and the record
//! keeps it whatever the answer. Behind Core, the top-up waits, still
//! unlocked, and is sent again after the next ChainLock. Needing a ChainLock
//! proof, it is sent again with one for the same outpoint once its block is
//! ChainLocked, and keeps needing one. Nothing settled, Continue sends it
//! again. Refused, an unauthenticated "already consumed" included, it is set
//! aside without stopping a new top-up. Every submission starts above the
//! fee increase of the one before, even one reported lower, unless that is a
//! day old, and none funds a second lock. A call that built nothing is no
//! attempt: the day counts from the one before it. A top-up answered at the
//! last fee step is not read again on its own until a step is free.
void PlatformTests::fundingOutcomesKeepTheLock()
{
    using Outcome = platform::FundingOutcome;
    using State = IdentityFlow::FundingState;
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(5, 100)};
        client.top_ups = {Funded<uint64_t>(Outcome::PLATFORM_BEHIND_CORE, 14),
                          Funded<uint64_t>(Outcome::NEEDS_CHAINLOCK_PROOF, 15),
                          Funded<uint64_t>(Outcome::UNAVAILABLE, 3),
                          Funded<uint64_t>(Outcome::UNAVAILABLE, 17),
                          Funded<uint64_t>(Outcome::UNAVAILABLE, 17),
                          Funded<uint64_t>(Outcome::REJECTED, 0),
                          Funded<uint64_t>(Outcome::SUBMITTED, 100)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    bool chain_locked{false};
    IdentityFlow flow{service, confirmations.get(), InstantProof,
                      [&chain_locked](const CTransactionRef& tx) -> std::optional<platform::ChainAssetLockProof> {
                          if (!chain_locked) return std::nullopt;
                          return platform::ChainAssetLockProof{120, COutPoint{tx->GetHash(), 0}};
                      }};
    FakePlatformClient& fake{*sources.fake};
    fake.deliverAll();
    QVERIFY(flow.identity());
    flow.topUp(IdentityFlow::MIN_TOP_UP);
    const uint256 txid{Assert(flow.funding())->txid};
    const size_t txs{wallet.getWalletTxs().size()};

    fake.deliverAll();
    QVERIFY(flow.step() == IdentityFlow::Step::WAITING_FOR_LOCK);
    QVERIFY(flow.funding()->state == State::PLATFORM_BEHIND);
    Q_EMIT f.models.client.instantSendChanged();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{1});
    Q_EMIT f.models.client.chainLockChanged();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{2});

    fake.deliverAll();
    QVERIFY(flow.step() == IdentityFlow::Step::WAITING_FOR_LOCK);
    QVERIFY(flow.funding()->state == State::NEEDS_CHAINLOCK);
    Q_EMIT f.models.client.chainLockChanged();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{2});
    chain_locked = true;
    Q_EMIT f.models.client.chainLockChanged();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{3});
    QVERIFY(std::get<platform::ChainAssetLockProof>(fake.proofs.back()).out_point == COutPoint(txid, 0));

    fake.deliverAll();
    QVERIFY(flow.step() == IdentityFlow::Step::IDLE);
    QVERIFY(flow.funding() && flow.funding()->state == State::NEEDS_CHAINLOCK);
    QVERIFY(!flow.error().isEmpty());
    flow.resume();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{4});
    QVERIFY(std::holds_alternative<platform::ChainAssetLockProof>(fake.proofs.back()));
    fake.deliverAll();
    const auto attempted{GetTime<std::chrono::seconds>()};
    {
        const MockTime later{attempted + IdentityFlow::FEE_INCREASE_MEMORY / 2};
        flow.resume();
        QCOMPARE(fake.countCalls("topUpIdentity"), size_t{5});
        fake.deliverAll();
    }
    {
        const MockTime later{attempted + IdentityFlow::FEE_INCREASE_MEMORY};
        flow.resume();
        QCOMPARE(fake.countCalls("topUpIdentity"), size_t{6});
    }
    QVERIFY(fake.fee_increase_floors == (std::vector<uint16_t>{0, 15, 16, 17, 18, 0}));

    fake.deliverAll();
    QVERIFY(!flow.funding());
    const auto kept{flow.fundings()};
    QVERIFY(kept.size() == 1 && kept.front().txid == txid && kept.front().state == State::SET_ASIDE);
    QVERIFY(!flow.error().isEmpty());
    for (const auto& proof : fake.proofs) {
        QVERIFY(ProofTxid(proof) == txid);
    }
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    flow.topUp(IdentityFlow::MIN_TOP_UP);
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{7});
    QVERIFY(ProofTxid(fake.proofs.back()) != txid);
    fake.deliverAll();
    QVERIFY(flow.funding() && flow.funding()->state == State::CONFIRMING);
    const size_t reads{fake.countCalls("assetLockState")};
    for (int chain_lock{0}; chain_lock < 3; ++chain_lock) {
        Q_EMIT f.models.client.chainLockChanged();
        fake.deliverAll();
    }
    QCOMPARE(fake.countCalls("assetLockState"), reads);
}

//! A top-up Platform answered without proving its lock used stays as
//! confirming, never as coins returned: each ChainLock reads Platform's
//! record of the lock, sending it again while unused and ending the record
//! once it is used up. A node reporting a top-up's lock consumed is checked
//! the same way, by Check again.
void PlatformTests::topUpIsSettledByItsLockRecord()
{
    using Outcome = platform::FundingOutcome;
    using LockRecord = platform::AssetLockState::State;
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(5, 100), Found(5, 1'000'100), Found(5, 2'000'100)};
        client.top_ups = {Funded<uint64_t>(Outcome::SUBMITTED, 3), Funded<uint64_t>(Outcome::SUBMITTED, 4),
                          Funded<uint64_t>(Outcome::REPORTED_CONSUMED, 6)};
        client.asset_lock_states = {Unavailable<platform::AssetLockState>(), LockState(LockRecord::NOT_PRESENT),
                                    LockState(LockRecord::FULLY_CONSUMED), LockState(LockRecord::FULLY_CONSUMED)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    FakePlatformClient& fake{*sources.fake};
    fake.deliverAll();
    flow.topUp(IdentityFlow::MIN_TOP_UP);
    const uint256 txid{Assert(flow.funding())->txid};
    fake.deliverAll();
    QVERIFY(flow.step() == IdentityFlow::Step::IDLE);
    QVERIFY(flow.funding() && flow.funding()->state == IdentityFlow::FundingState::CONFIRMING);
    QCOMPARE(flow.identity()->balance, uint64_t{100});
    {
        FakeSources tab;
        tab.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {Found(5, 100)}; };
        PlatformPage page{tab.get()};
        f.attach(page);
        tab.fake->deliverAll();
        const QString texts{VisibleTexts(page).join(' ')};
        QVERIFY(texts.contains("Top-up sent; waiting for Platform to confirm the payment was used"));
        QVERIFY(!texts.contains("coins"));
        QVERIFY(VisibleButton(page, "dashPayContinue") &&
                VisibleButton(page, "dashPayContinue")->text() == "Check again");
        QVERIFY(VisibleButton(page, "dashPayTopUp") == nullptr);
    }

    Q_EMIT f.models.client.chainLockChanged();
    fake.deliverAll();
    QCOMPARE(fake.countCalls("assetLockState"), size_t{1});
    QVERIFY(fake.calls.back().argument == COutPoint(txid, 0).ToStringShort());
    QVERIFY(flow.funding() && flow.funding()->state == IdentityFlow::FundingState::CONFIRMING);
    Q_EMIT f.models.client.chainLockChanged();
    fake.deliverAll();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{2});
    QVERIFY(ProofTxid(fake.proofs.back()) == txid);
    QCOMPARE(fake.fee_increase_floors.back(), uint16_t{4});
    QVERIFY(flow.funding() && flow.funding()->state == IdentityFlow::FundingState::CONFIRMING);
    Q_EMIT f.models.client.chainLockChanged();
    fake.deliverAll();
    QVERIFY(flow.fundings().empty());
    QVERIFY(flow.note().contains("used on Dash Platform"));
    QCOMPARE(flow.identity()->balance, uint64_t{1'000'100});

    flow.topUp(IdentityFlow::MIN_TOP_UP);
    fake.deliverAll();
    QVERIFY(flow.funding() && flow.funding()->state == IdentityFlow::FundingState::CHECK_AGAIN);
    flow.resume();
    fake.deliverAll();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{3});
    QVERIFY(flow.fundings().empty());
    QCOMPARE(flow.identity()->balance, uint64_t{2'000'100});
}

//! A registration lock is used by the identity it registers. Reported
//! consumed without it, the lock is sent again while Platform has not used
//! it; used up without it, the record ends as a payment used on Platform with
//! no derived identity, and Create is offered again. A lock beside a
//! different identity found under the wallet's MASTER key is kept.
void PlatformTests::registrationIsSettledByItsIdentity()
{
    using Outcome = platform::FundingOutcome;
    using LockRecord = platform::AssetLockState::State;
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {NotFound(), Found(5, 100)};
        client.registered_identities = {Funded<platform::Identity>(Outcome::REPORTED_CONSUMED, 0),
                                        Funded<platform::Identity>(Outcome::REPORTED_CONSUMED, 1)};
        client.identities = {NotFound(), NotFound()};
        client.asset_lock_states = {LockState(LockRecord::NOT_PRESENT), LockState(LockRecord::FULLY_CONSUMED)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    FakePlatformClient& fake{*sources.fake};
    fake.deliverAll();
    flow.create();
    const uint256 txid{Assert(flow.funding())->txid};
    fake.deliverAll();
    QVERIFY(flow.funding() && flow.funding()->state == IdentityFlow::FundingState::CHECK_AGAIN);

    Q_EMIT f.models.client.chainLockChanged();
    fake.deliverAll();
    QVERIFY(fake.calls.at(fake.calls.size() - 3).argument == HexStr(Registered(txid)));
    QCOMPARE(fake.countCalls("registerIdentity"), size_t{2});
    QVERIFY(ProofTxid(fake.proofs.back()) == txid);
    QVERIFY(fake.fee_increase_floors == (std::vector<uint16_t>{0, 1}));

    Q_EMIT f.models.client.chainLockChanged();
    fake.deliverAll();
    QVERIFY(flow.fundings().empty());
    QVERIFY(!flow.identity());
    QVERIFY(flow.note().contains("Funding payment used on Platform; no derived identity found"));

    flow.create();
    const uint256 kept{Assert(flow.funding())->txid};
    QVERIFY(kept != txid);
    fake.deliverAll();
    flow.lookUp();
    fake.deliverAll();
    QVERIFY(flow.identity());
    QVERIFY(!flow.funding());
    QVERIFY(flow.fundings().size() == 1 && flow.fundings().front().txid == kept);
}

//! The passphrase given to fund keeps an encrypted wallet unlocked through
//! the waits for the payment's lock and Platform's answer, and no longer: a
//! gate closing while the passphrase or confirmation dialog is open sends
//! nothing, one closing after the payment was sent keeps its record, and
//! shutdown relocks it. A payment the network refuses is abandoned and its
//! record erased. Behind Core through every fee step, the wait ends with the
//! wallet relocked, the record kept and the time it is sent again named;
//! until then no ChainLock sends it, and Continue only reads Platform's
//! record of the lock, without the passphrase; after it, Continue waits for
//! the next ChainLock again and sends it from fee increase 0.
void PlatformTests::closedGateNeverCommits()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    std::function<void()> unlocking;
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
        if (unlocking) unlocking();
    });
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(5, 100)};
        client.top_ups = {Funded<uint64_t>(platform::FundingOutcome::OK, 0, 1'000'000'100)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    bool locked{false};
    auto flow{std::make_unique<IdentityFlow>(service, confirmations.get(), [&locked](const CTransactionRef& tx) {
        return locked ? InstantProof(tx) : std::nullopt;
    })};
    flow->lookUp();
    sources.fake->deliverAll();
    QVERIFY(flow->identity());
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    const size_t txs{wallet.getWalletTxs().size()};
    const auto reopen{[&] {
        f.setNetworkActive(true);
        sources.fake->deliverAll();
        return flow->identity().has_value();
    }};

    unlocking = [&] { f.setNetworkActive(false); };
    flow->topUp(IdentityFlow::MIN_TOP_UP);
    unlocking = nullptr;
    QVERIFY(confirmations.asked.empty());
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(flow->fundings().empty());
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(reopen());

    confirmations.meanwhile = [&] { f.setNetworkActive(false); };
    flow->topUp(IdentityFlow::MIN_TOP_UP);
    confirmations.meanwhile = nullptr;
    QCOMPARE(confirmations.asked.size(), size_t{1});
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(flow->fundings().empty());
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(reopen());

    f.wallet->SetBroadcastTransactions(true);
    confirmations.meanwhile = [&] { f.wallet->m_default_max_tx_fee = 1; };
    flow->topUp(IdentityFlow::MIN_TOP_UP);
    confirmations.meanwhile = nullptr;
    f.wallet->m_default_max_tx_fee = wallet::DEFAULT_TRANSACTION_MAXFEE;
    f.wallet->SetBroadcastTransactions(false);
    QCOMPARE(wallet.getWalletTxs().size(), txs + 1);
    QVERIFY(flow->fundings().empty());
    QVERIFY(!flow->error().isEmpty());
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);

    flow->topUp(IdentityFlow::MIN_TOP_UP);
    QVERIFY(flow->step() == IdentityFlow::Step::WAITING_FOR_LOCK);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Unlocked);
    f.setNetworkActive(false);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QCOMPARE(flow->fundings().size(), size_t{1});
    QVERIFY(reopen());

    flow->resume();
    QVERIFY(flow->step() == IdentityFlow::Step::WAITING_FOR_LOCK);
    locked = true;
    Q_EMIT f.models.client.instantSendChanged();
    QVERIFY(flow->step() == IdentityFlow::Step::SENDING);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Unlocked);
    sources.fake->deliverAll();
    QVERIFY(flow->fundings().empty());
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);

    locked = false;
    flow->topUp(IdentityFlow::MIN_TOP_UP);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Unlocked);
    flow.reset();
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);

    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(5, 100)};
        for (const uint16_t last : {14, 29, 44, 59, 74, 89, 100}) {
            client.top_ups.push_back(Funded<uint64_t>(platform::FundingOutcome::PLATFORM_BEHIND_CORE, last));
        }
        client.asset_lock_states = {LockState(platform::AssetLockState::State::NOT_PRESENT)};
    };
    f.setNetworkActive(false);
    f.setNetworkActive(true);
    const auto start{GetTime<std::chrono::seconds>()};
    const MockTime clock{start};
    flow = std::make_unique<IdentityFlow>(service, confirmations.get(), InstantProof);
    flow->lookUp();
    sources.fake->deliverAll();
    QVERIFY(flow->identity() && flow->funding());
    flow->resume();
    sources.fake->deliverAll();
    for (int hour{1}; hour <= 6; ++hour) {
        QVERIFY(flow->step() == IdentityFlow::Step::WAITING_FOR_LOCK);
        QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Unlocked);
        SetMockTime(start + std::chrono::hours{hour});
        Q_EMIT f.models.client.chainLockChanged();
        sources.fake->deliverAll();
    }
    QVERIFY(sources.fake->fee_increase_floors == (std::vector<uint16_t>{0, 15, 30, 45, 60, 75, 90}));
    QVERIFY(flow->step() == IdentityFlow::Step::IDLE);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(flow->funding() && flow->funding()->state == IdentityFlow::FundingState::PLATFORM_BEHIND);
    const auto last_attempt{start + std::chrono::hours{6}};
    const QString again{GUIUtil::dateTimeStr((last_attempt + IdentityFlow::FEE_INCREASE_MEMORY).count())};
    QVERIFY(flow->error().contains(again));
    for (int chain_lock{0}; chain_lock < 20; ++chain_lock) {
        Q_EMIT f.models.client.chainLockChanged();
        sources.fake->deliverAll();
    }
    QCOMPARE(sources.fake->countCalls("topUpIdentity"), size_t{7});
    QCOMPARE(sources.fake->countCalls("assetLockState"), size_t{0});

    int asked{0};
    unlocking = [&] { ++asked; };
    SetMockTime(start + std::chrono::hours{10});
    flow->resume();
    sources.fake->deliverAll();
    QCOMPARE(asked, 0);
    QCOMPARE(sources.fake->countCalls("assetLockState"), size_t{1});
    QCOMPARE(sources.fake->countCalls("topUpIdentity"), size_t{7});
    QVERIFY(flow->error().contains(again));
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    SetMockTime(last_attempt + IdentityFlow::FEE_INCREASE_MEMORY);
    flow->resume();
    QCOMPARE(asked, 1);
    QVERIFY(flow->step() == IdentityFlow::Step::WAITING_FOR_LOCK);
    Q_EMIT f.models.client.chainLockChanged();
    QCOMPARE(sources.fake->countCalls("topUpIdentity"), size_t{8});
    QCOMPARE(sources.fake->fee_increase_floors.back(), uint16_t{0});
    unlocking = nullptr;
    flow.reset();
}

//! An encrypted wallet derives its MASTER key only unlocked: the tab does
//! not ask for the passphrase on its own but offers the lookup, and once
//! the identity is found it shows its balance and offers a top-up. A client
//! rebuilt while the passphrase dialog is open gets one lookup, shown as
//! running; later clients look it up again without the passphrase.
void PlatformTests::lockedWalletLooksUpOnUnlock()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    int unlocks{0};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        f.setNetworkActive(false);
        f.setNetworkActive(true);
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {Found(3, 4'000'000)}; };
    PlatformPage page{sources.get()};
    f.attach(page);
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{0});
    QCOMPARE(unlocks, 0);
    QVERIFY(VisibleButton(page, "dashPayCreateIdentity") == nullptr);
    QVERIFY(!page.findChild<QLabel*>("dashPayIdentity")->isVisibleTo(&page));

    QPushButton* look_up{VisibleButton(page, "dashPayLookUp")};
    QVERIFY(look_up != nullptr);
    look_up->click();
    QCOMPARE(unlocks, 1);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QCOMPARE(sources.made.size(), size_t{2});
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QVERIFY(VisibleButton(page, "dashPayLookUp") == nullptr);
    sources.fake->deliverAll();
    QVERIFY(page.findChild<QLabel*>("dashPayIdentity")->isVisibleTo(&page));
    QVERIFY(VisibleButton(page, "dashPayTopUp") != nullptr);
    QVERIFY(VisibleButton(page, "dashPayLookUp") == nullptr);
    QVERIFY(VisibleButton(page, "dashPayCreateIdentity") == nullptr);

    f.setNetworkActive(false);
    f.setNetworkActive(true);
    QCOMPARE(sources.made.size(), size_t{3});
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QCOMPARE(unlocks, 1);
}
