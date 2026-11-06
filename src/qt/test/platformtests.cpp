// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/platformtests.h>

#include <qt/test/masternodetestutil.h>

#include <chain.h>
#include <common/args.h>
#include <common/settings.h>
#include <crypto/aes.h>
#include <evo/assetlocktx.h>
#include <evo/dmn_types.h>
#include <evo/specialtx.h>
#include <evo/types.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <key_io.h>
#include <netbase.h>
#include <qt/bitcoinamountfield.h>
#include <qt/clientfeeds.h>
#include <qt/guiutil.h>
#include <qt/masternodemodel.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformpage.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/recoveryflow.h>
#include <qt/platform/usernameflow.h>
#include <qt/platform/usernamesearchdialog.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <test/util/platform_client.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/overloaded.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleFactory>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using MasternodeTestUtil::GuiModels;
using MasternodeTestUtil::MakeCoinbaseWallet;
using MasternodeTestUtil::MakeTestWallet;
using MasternodeTestUtil::WalletGuard;
using Gate = PlatformService::Gate;
using Purpose = platform::IdentityPublicKey::Purpose;
using Level = platform::IdentityPublicKey::SecurityLevel;

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

//! A committed asset lock paying `key`, as an earlier install of the same
//! seed left it.
uint256 CommitLock(interfaces::Wallet& wallet, const wallet::AssetLockFundingKey& key)
{
    CAmount fee{0};
    const auto tx{wallet.createAssetLockTransaction(IdentityFlow::MIN_TOP_UP, key, wallet::CCoinControl{}, fee)};
    Assert(tx && wallet.commitTransaction(*tx, {}, {}));
    return (*tx)->GetHash();
}

//! A committed asset lock whose two credit outputs pay `key`, as another
//! wallet on the same seed may build it.
uint256 CommitSplitLock(interfaces::Wallet& wallet, const wallet::AssetLockFundingKey& key)
{
    CAmount fee{0};
    const auto base{wallet.createAssetLockTransaction(2 * IdentityFlow::MIN_TOP_UP, key, wallet::CCoinControl{}, fee)};
    Assert(base);
    CMutableTransaction split{**base};
    const auto payload{GetTxPayload<CAssetLockPayload>(split)};
    Assert(payload);
    CTxOut credit{payload->getCreditOutputs().front()};
    credit.nValue /= 2;
    SetTxPayload(split, CAssetLockPayload{{credit, credit}, payload->getVersion()});
    for (auto& input : split.vin)
        input.scriptSig.clear();
    // The longer payload pays a higher fee, from the change.
    for (auto& output : split.vout) {
        if (!output.scriptPubKey.IsUnspendable()) {
            output.nValue -= 10'000;
            break;
        }
    }
    const auto signed_tx{wallet.signTransaction(split)};
    Assert(signed_tx && signed_tx->complete && wallet.commitTransaction(signed_tx->tx, {}, {}));
    return signed_tx->tx->GetHash();
}

//! What a wallet restored from its seed has of DashPay's funding records:
//! nothing.
void ForgetFundingRecords(interfaces::Wallet& wallet)
{
    for (const char* prefix : {IdentityFlow::FUNDING_PREFIX, IdentityFlow::USED_PREFIX, "dashpay/topup-index"}) {
        for (const auto& record : wallet.getPlatformData(prefix)) {
            Assert(wallet.writePlatformData(record.first, {}));
        }
    }
}

//! Sets the mock time while it lives.
struct MockTime {
    explicit MockTime(std::chrono::seconds time) { SetMockTime(time); }
    ~MockTime() { SetMockTime(0); }
};

//! The node's tip moved, as ClientModel reports a new block or header.
void TipChanged(ClientModel& client, SyncType type = SyncType::BLOCK_SYNC)
{
    Q_EMIT client.numBlocksChanged(100, QDateTime::currentDateTime(), QString{}, 1.0, type,
                                   SynchronizationState::POST_INIT);
}

platform::IdentityPublicKey Key(uint32_t id, Purpose purpose, Level level, uint64_t disabled_at = 0)
{
    platform::IdentityPublicKey key;
    key.id = id;
    key.purpose = purpose;
    key.security_level = level;
    key.disabled_at = disabled_at;
    return key;
}

//! The keys Dash Core registers, by id as the mobile wallets do.
std::vector<platform::IdentityPublicKey> RegisteredKeys()
{
    return {Key(0, Purpose::AUTHENTICATION, Level::MASTER), Key(1, Purpose::AUTHENTICATION, Level::CRITICAL),
            Key(2, Purpose::AUTHENTICATION, Level::HIGH),   Key(3, Purpose::TRANSFER, Level::CRITICAL),
            Key(4, Purpose::ENCRYPTION, Level::MEDIUM),     Key(5, Purpose::DECRYPTION, Level::MEDIUM)};
}

platform::Result<platform::Identifier> Resolved(std::optional<uint8_t> owner)
{
    platform::Result<platform::Identifier> result;
    result.status.kind = owner ? platform::StatusKind::OK : platform::StatusKind::NOT_FOUND;
    if (owner) result.value.fill(*owner);
    return result;
}

platform::Result<platform::Names> Names(const std::vector<std::string>& labels)
{
    platform::Result<platform::Names> result;
    result.status.kind = platform::StatusKind::OK;
    result.value.complete = true;
    for (const std::string& label : labels) {
        result.value.names.push_back({label, label, {}, {}});
    }
    return result;
}

//! The read of an identity's username waiting for its vote: `label`, or none.
platform::Result<std::vector<platform::Contest>> Contests(std::optional<std::string> label)
{
    platform::Result<std::vector<platform::Contest>> result;
    result.status.kind = platform::StatusKind::OK;
    if (label) result.value.push_back({*label, 0});
    return result;
}

platform::Result<platform::ContactRequestPage> Requests(std::vector<platform::ContactRequest> requests)
{
    platform::Result<platform::ContactRequestPage> result;
    result.status.kind = platform::StatusKind::OK;
    result.value.requests = std::move(requests);
    result.value.complete = true;
    return result;
}

platform::ContactRequest Request(uint8_t from, uint8_t to, uint64_t created_at_ms)
{
    platform::ContactRequest request;
    request.owner.fill(from);
    request.to_user_id.fill(to);
    request.created_at_ms = created_at_ms;
    return request;
}

//! A request from `from` encrypted, as DIP-15 does, from their key
//! `sender_key` (id 1) to the wallet's identity key `our_key`; it carries
//! `pubkey` as the payment chain's key.
platform::ContactRequest IncomingRequest(interfaces::Wallet& wallet, uint8_t from, uint8_t to, const CKey& sender_key,
                                         uint64_t created_at_ms, Span<const uint8_t> pubkey, uint32_t our_key = 5)
{
    auto request{Request(from, to, created_at_ms)};
    request.sender_key_index = 1;
    request.recipient_key_index = our_key;
    const auto secret{wallet.platformECDHSecret({0, our_key}, sender_key.GetPubKey())};
    Assert(secret && secret.value.size() == AES256_KEYSIZE);
    std::vector<uint8_t> compact(4 + 32, 0x11);
    compact.insert(compact.end(), pubkey.begin(), pubkey.end());
    request.encrypted_public_key.assign(AES_BLOCKSIZE, 0x22);
    request.encrypted_public_key.resize(AES_BLOCKSIZE + 80);
    const AES256CBCEncrypt aes{secret.value.data(), request.encrypted_public_key.data(), /*padIn=*/true};
    Assert(aes.Encrypt(compact.data(), compact.size(), request.encrypted_public_key.data() + AES_BLOCKSIZE) == 80);
    return request;
}

//! A counterparty whose key 1 (`key`, of `purpose`) sent their requests;
//! key 2 receives ours.
platform::Result<platform::Identity> Peer(uint8_t id, const CKey& key, Purpose purpose, uint64_t disabled_at = 0)
{
    auto found{Found(id, 0)};
    auto sender{Key(1, purpose, Level::HIGH, disabled_at)};
    const CPubKey pubkey{key.GetPubKey()};
    sender.data.assign(pubkey.begin(), pubkey.end());
    found.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER), sender,
                               Key(2, Purpose::DECRYPTION, Level::MEDIUM)};
    return found;
}

//! The payment chain IncomingRequest sends for `pubkey`.
wallet::FriendshipXpub PaymentChain(const CPubKey& pubkey)
{
    wallet::FriendshipXpub chain;
    chain.pubkey = pubkey;
    std::fill(chain.chaincode.begin(), chain.chaincode.end(), 0x11);
    chain.parent_fingerprint.fill(0x11);
    return chain;
}

CTxDestination PaymentAddress(const wallet::FriendshipXpub& chain, uint32_t index)
{
    CTxDestination address;
    Assert(wallet::DeriveFriendshipPaymentDestination(chain, index, address));
    return address;
}

//! The wallet sends a payment to `address`, as the Send tab does.
uint256 SendTo(interfaces::Wallet& wallet, const CTxDestination& address)
{
    int change_pos{-1};
    CAmount fee;
    const auto tx{wallet.createTransaction({{GetScriptForDestination(address), COIN, false}}, {}, /*sign=*/true,
                                           change_pos, fee)};
    Assert(tx && wallet.commitTransaction(*tx, {}, {}));
    return (*tx)->GetHash();
}

//! The button in the contacts list row of `id`, or nullptr.
QPushButton* ContactButton(const PlatformPage& page, uint8_t id)
{
    const auto contacts{page.contactFlow()->contacts()};
    const auto* list{page.findChild<QTableWidget*>("dashPayContacts")};
    for (size_t row = 0; row < contacts.size(); ++row) {
        if (contacts[row].id[0] == id) return qobject_cast<QPushButton*>(list->cellWidget(row, 2));
    }
    return nullptr;
}

//! Click Pay on the row of `id`: what it handed to the Send tab, if anything.
std::optional<SendCoinsRecipient> PayRecipient(PlatformPage& page, FakeSources& sources, uint8_t id)
{
    std::optional<SendCoinsRecipient> paid;
    const auto connection{QObject::connect(&page, &PlatformPage::payTo,
                                           [&paid](const SendCoinsRecipient& recipient) { paid = recipient; })};
    if (QPushButton* pay{ContactButton(page, id)}; pay && pay->objectName() == "dashPayPayContact") {
        pay->click();
        sources.fake->deliverAll();
    }
    QObject::disconnect(connection);
    return paid;
}

//! The address PayRecipient() handed over, empty if none was.
QString PayContact(PlatformPage& page, FakeSources& sources, uint8_t id)
{
    const auto paid{PayRecipient(page, sources, id)};
    return paid ? paid->address : QString{};
}

//! Answer the question the wallet asks next with `button`.
void Answer(QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, [button] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* box{qobject_cast<QMessageBox*>(widget)};
            if (box && box->isVisible() && !widget->inherits("SendConfirmationDialog")) box->button(button)->click();
        }
    });
}

//! The text of the send confirmation once it shows; it is cancelled.
void ReadConfirmation(QString& text)
{
    QTimer::singleShot(0, [&text] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog{qobject_cast<SendConfirmationDialog*>(widget)}; dialog && dialog->isVisible()) {
                text = dialog->text();
                dialog->button(QMessageBox::Cancel)->click();
            }
        }
    });
}
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

    QPushButton* find{VisibleButton(page, "dashPayFindIdentity")};
    QVERIFY(find != nullptr);
    find->click();
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode()});
    QVERIFY(!sources.made.back().proxy);
    QVERIFY(page.service()->client() != nullptr);
    QVERIFY(VisibleButton(page, "dashPayFindIdentity") == nullptr);

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
    QVERIFY(VisibleButton(page, "dashPayFindIdentity") != nullptr);
    // Turned off before the identity was found, which is said.
    QVERIFY(page.recoveryFlow()->step() == RecoveryFlow::Step::IDLE);
    QVERIFY(VisibleTexts(page).join(' ').contains("DashPay stopped before your identity"));
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
//! no derived identity, and Create is offered again. A lock refused as a
//! registration, beside a different identity found under the wallet's
//! MASTER key, is offered as a top-up of it, signed with the registration
//! key; refused as a top-up too, it is never offered again.
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
                                        Funded<platform::Identity>(Outcome::REPORTED_CONSUMED, 1),
                                        Funded<platform::Identity>(Outcome::REJECTED, 0)};
        client.identities = {NotFound(), NotFound()};
        client.asset_lock_states = {LockState(LockRecord::NOT_PRESENT), LockState(LockRecord::FULLY_CONSUMED)};
        client.top_ups = {Funded<uint64_t>(Outcome::OK, 0, 1'000'100)};
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
    QVERIFY(flow.fundings().size() == 1 && flow.fundings().front().state == IdentityFlow::FundingState::SET_ASIDE);
    flow.lookUp();
    fake.deliverAll();
    QVERIFY(flow.identity());
    QVERIFY(!flow.funding());
    QVERIFY(flow.fundings().size() == 1 && flow.fundings().front().txid == kept);
    QVERIFY(flow.earlier().size() == 1 && flow.earlier().front().txid == kept);
    {
        FakeSources tab;
        tab.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {Found(5, 100)}; };
        PlatformPage page{tab.get()};
        f.attach(page);
        tab.fake->deliverAll();
        QPushButton* apply{VisibleButton(page, "dashPayApplyEarlier")};
        QVERIFY(apply && apply->text().contains("Apply 1 earlier funding payment"));
    }
    flow.applyEarlier();
    fake.deliverAll();
    QCOMPARE(fake.countCalls("topUpIdentity"), size_t{1});
    QVERIFY(std::holds_alternative<wallet::RegistrationFundingKey>(fake.top_up_keys.back()));
    QVERIFY(ProofTxid(fake.proofs.back()) == kept);
    QVERIFY(flow.fundings().empty());
    QVERIFY(flow.earlier().empty());

    // Another install registered the identity after this one's lookup.
    FakeSources later;
    later.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {NotFound(), Found(5, 1'000'100), Found(5, 1'000'100)};
        client.registered_identities = {Funded<platform::Identity>(Outcome::REJECTED, 0)};
        client.top_ups = {Funded<uint64_t>(Outcome::REJECTED, 0)};
    };
    PlatformService again{f.wallet_model, f.models.client, later.get()};
    IdentityFlow second{again, confirmations.get(), InstantProof};
    later.fake->deliverAll();
    second.create();
    const uint256 refused{Assert(second.fundings().size() == 1) ? second.fundings().front().txid : uint256{}};
    later.fake->deliverAll();
    second.lookUp();
    later.fake->deliverAll();
    QVERIFY(second.earlier().size() == 1 && second.earlier().front().txid == refused);
    second.applyEarlier();
    later.fake->deliverAll();
    QCOMPARE(later.fake->countCalls("topUpIdentity"), size_t{1});
    QVERIFY(second.fundings().size() == 1 && second.fundings().front().top_up &&
            second.fundings().front().state == IdentityFlow::FundingState::SET_ASIDE);
    QVERIFY(second.earlier().empty());
    second.lookUp();
    later.fake->deliverAll();
    QVERIFY(second.earlier().empty());
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

//! A wallet restored from its seed finds what an earlier install funded in
//! its own transactions, and checks each payment against Platform's record
//! before offering it. Create sends the registration lock it finds instead
//! of a new one. Beside a different identity, that lock and the unused top-up
//! locks are offered as one action that adds them to the identity with one
//! unlock; a used-up lock is marked and never offered, a node reporting one
//! consumed leaves it listed to check again, and the next top-up skips every
//! index already paid, however far apart within the gap. Two credit outputs
//! paying one key are two payments. While a payment is still to be applied,
//! Top up is hidden and funds no new lock, one another install sends after
//! the lookup included. The identity a registration lock registers ends it
//! without a submission.
void PlatformTests::restoredWalletResumesItsFunding()
{
    using Outcome = platform::FundingOutcome;
    using LockRecord = platform::AssetLockState::State;
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    Confirmations confirmations;
    uint256 registration;
    {
        FakeSources sources;
        sources.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {NotFound()}; };
        PlatformService service{f.wallet_model, f.models.client, sources.get()};
        IdentityFlow flow{service, confirmations.get(), [](const CTransactionRef&) { return std::nullopt; }};
        sources.fake->deliverAll();
        flow.create();
        registration = Assert(flow.funding())->txid;
    }
    const uint256 top_up_0{CommitLock(wallet, wallet::TopupFundingKey{0})};
    const uint256 split{CommitSplitLock(wallet, wallet::TopupFundingKey{1})};
    const uint256 top_up_3{CommitLock(wallet, wallet::TopupFundingKey{3})};
    CommitLock(wallet, wallet::TopupFundingKey{21});
    ForgetFundingRecords(wallet);
    const size_t txs{wallet.getWalletTxs().size()};
    const size_t asked{confirmations.asked.size()};

    {
        FakeSources sources;
        sources.script = [](FakePlatformClient& client) {
            client.identities_by_pubkey_hash = {NotFound()};
            client.asset_lock_states.assign(6, LockState(LockRecord::NOT_PRESENT));
        };
        PlatformService service{f.wallet_model, f.models.client, sources.get()};
        IdentityFlow flow{service, confirmations.get(), InstantProof};
        sources.fake->deliverAll();
        QCOMPARE(flow.found().size(), size_t{6});
        QVERIFY(flow.earlier().empty());
        flow.create();
        QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
        QVERIFY(ProofTxid(sources.fake->proofs.back()) == registration);
        QCOMPARE(wallet.getWalletTxs().size(), txs);
        QCOMPARE(confirmations.asked.size(), asked);
        sources.fake->deliverAll();
        QVERIFY(flow.funding() && flow.funding()->txid == registration);
    }
    ForgetFundingRecords(wallet);

    {
        FakeSources sources;
        sources.script = [](FakePlatformClient& client) {
            client.identities_by_pubkey_hash = {Found(5, 100), Found(5, 1'000'100), Found(5, 1'000'100)};
            client.asset_lock_states = {LockState(LockRecord::NOT_PRESENT),    LockState(LockRecord::FULLY_CONSUMED),
                                        LockState(LockRecord::NOT_PRESENT),    LockState(LockRecord::NOT_PRESENT),
                                        LockState(LockRecord::NOT_PRESENT),    LockState(LockRecord::FULLY_CONSUMED),
                                        LockState(LockRecord::FULLY_CONSUMED), LockState(LockRecord::NOT_PRESENT)};
            const auto ok{Funded<uint64_t>(Outcome::OK, 0, 1'000'100)};
            client.top_ups = {ok, ok, ok, Funded<uint64_t>(Outcome::REPORTED_CONSUMED, 0), ok};
        };
        PlatformService service{f.wallet_model, f.models.client, sources.get()};
        IdentityFlow flow{service, confirmations.get(), InstantProof};
        {
            FakeSources tab;
            tab.script = [](FakePlatformClient& client) {
                client.identities_by_pubkey_hash = {Found(5, 100)};
                client.asset_lock_states = {LockState(LockRecord::NOT_PRESENT), Unavailable<platform::AssetLockState>(),
                                            LockState(LockRecord::NOT_PRESENT), LockState(LockRecord::NOT_PRESENT),
                                            LockState(LockRecord::NOT_PRESENT), LockState(LockRecord::FULLY_CONSUMED)};
            };
            PlatformPage page{tab.get()};
            f.attach(page);
            tab.fake->deliverAll();
            QPushButton* apply{VisibleButton(page, "dashPayApplyEarlier")};
            QVERIFY(apply);
            QVERIFY(apply->text().contains("Apply 4 earlier funding payment"));
            QVERIFY(VisibleTexts(page).join(' ').contains("checking an earlier funding payment"));
            QVERIFY(!VisibleButton(page, "dashPayTopUp"));
            QVERIFY(!page.findChild<QWidget*>("dashPayTopUpAmount")->isVisibleTo(&page));
        }
        FakePlatformClient& fake{*sources.fake};
        fake.deliverAll();
        const auto earlier{flow.earlier()};
        QCOMPARE(earlier.size(), size_t{4});
        QVERIFY(earlier.front().txid == registration && earlier.back().txid == top_up_3);
        QVERIFY(wallet.getPlatformData(IdentityFlow::USED_PREFIX).size() == 2);
        const size_t paid{wallet.getWalletTxs().size()};
        flow.topUp(IdentityFlow::MIN_TOP_UP);
        QCOMPARE(wallet.getWalletTxs().size(), paid);
        QVERIFY(flow.error().contains("Apply it before topping up"));

        flow.applyEarlier();
        fake.deliverAll();
        QCOMPARE(fake.countCalls("topUpIdentity"), size_t{4});
        QVERIFY(std::holds_alternative<wallet::RegistrationFundingKey>(fake.top_up_keys.at(0)));
        QCOMPARE(std::get<wallet::TopupFundingKey>(fake.top_up_keys.at(3)).funding_index, uint32_t{3});
        QVERIFY(ProofTxid(fake.proofs.at(0)) == registration && ProofTxid(fake.proofs.at(3)) == top_up_3);
        std::set<uint32_t> outputs;
        for (const auto& proof : fake.proofs) {
            const auto& instant{std::get<platform::InstantAssetLockProof>(proof)};
            if (instant.transaction->GetHash() == split) outputs.insert(instant.output_index);
        }
        QVERIFY(outputs == (std::set<uint32_t>{0, 1}));
        for (const uint32_t n : {0U, 1U}) {
            QCOMPARE(wallet.getPlatformData(strprintf("%s%s:%u", IdentityFlow::USED_PREFIX, split.ToString(), n)).size(),
                     size_t{1});
        }
        QCOMPARE(confirmations.asked.size(), asked);
        QVERIFY(flow.earlier().empty());
        QVERIFY(flow.funding() && flow.funding()->txid == top_up_3 &&
                flow.funding()->state == IdentityFlow::FundingState::CHECK_AGAIN);
        flow.resume();
        fake.deliverAll();
        QVERIFY(flow.fundings().empty());
        {
            FakeSources tab;
            tab.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {Found(5, 100)}; };
            PlatformPage page{tab.get()};
            f.attach(page);
            tab.fake->deliverAll();
            QVERIFY(VisibleButton(page, "dashPayTopUp"));
            QVERIFY(!VisibleButton(page, "dashPayApplyEarlier"));
        }

        const size_t applied{wallet.getWalletTxs().size()};
        flow.topUp(IdentityFlow::MIN_TOP_UP);
        QCOMPARE(wallet.getWalletTxs().size(), applied + 1);
        const auto funding{flow.funding()};
        QVERIFY(funding && std::get<wallet::TopupFundingKey>(funding->key).funding_index == 22);
        QVERIFY(CreditScript(wallet, funding->txid) == FundingScript(wallet, wallet::TopupFundingKey{22}));
        QVERIFY(funding->txid != top_up_0);
        fake.deliverAll();
        QVERIFY(flow.fundings().empty());

        // Another install of the seed pays a top-up after this one looked:
        // Top up finds it, funds nothing, and reads it to be applied.
        const uint256 top_up_30{CommitLock(wallet, wallet::TopupFundingKey{30})};
        const size_t elsewhere{wallet.getWalletTxs().size()};
        flow.topUp(IdentityFlow::MIN_TOP_UP);
        QCOMPARE(wallet.getWalletTxs().size(), elsewhere);
        QVERIFY(flow.error().contains("Apply it before topping up"));
        fake.deliverAll();
        QVERIFY(flow.earlier().size() == 1 && flow.earlier().front().txid == top_up_30);
    }
    ForgetFundingRecords(wallet);

    FakeSources sources;
    sources.script = [&registration](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(Registered(registration), 100)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    sources.fake->deliverAll();
    QVERIFY(flow.identity());
    for (const auto& found : flow.found()) {
        QVERIFY(found.funding.txid != registration);
    }
    QCOMPARE(sources.fake->countCalls("registerIdentity") + sources.fake->countCalls("topUpIdentity"), size_t{0});
    for (const auto& call : sources.fake->calls) {
        QVERIFY(call.argument != COutPoint(registration, 0).ToStringShort());
    }
}

//! A payment found abandoned is kept but not offered or read; once the
//! wallet holds it active again, as when it is mined, it is checked and
//! offered like any other.
void PlatformTests::foundAbandonedLockIsOfferedOnceActive()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    const uint256 txid{CommitLock(wallet, wallet::TopupFundingKey{0})};
    QVERIFY(wallet.abandonTransaction(txid));
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(5, 100), Found(5, 100)};
        client.asset_lock_states = {LockState(platform::AssetLockState::State::NOT_PRESENT)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    sources.fake->deliverAll();
    QVERIFY(flow.found().size() == 1 && flow.found().front().inactive);
    QVERIFY(flow.earlier().empty());
    QCOMPARE(sources.fake->countCalls("assetLockState"), size_t{0});

    QVERIFY(wallet.commitTransaction(wallet.getTx(txid), {}, {}));
    flow.lookUp();
    sources.fake->deliverAll();
    QVERIFY(flow.found().size() == 1 && !flow.found().front().inactive);
    QCOMPARE(sources.fake->countCalls("assetLockState"), size_t{1});
    QVERIFY(flow.earlier().size() == 1 && flow.earlier().front().txid == txid);
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

    QPushButton* find{VisibleButton(page, "dashPayFindIdentity")};
    QVERIFY(find != nullptr);
    find->click();
    QCOMPARE(unlocks, 1);
    QCOMPARE(sources.made.size(), size_t{2});
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QVERIFY(VisibleButton(page, "dashPayFindIdentity") == nullptr);
    sources.fake->deliverAll();
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(page.findChild<QLabel*>("dashPayIdentity")->isVisibleTo(&page));
    QVERIFY(VisibleButton(page, "dashPayTopUp") != nullptr);
    QVERIFY(VisibleButton(page, "dashPayFindIdentity") == nullptr);
    QVERIFY(VisibleButton(page, "dashPayCreateIdentity") == nullptr);

    f.setNetworkActive(false);
    f.setNetworkActive(true);
    QCOMPARE(sources.made.size(), size_t{3});
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QCOMPARE(unlocks, 1);
}

//! A wallet without an identity registers its username in one go, under
//! one passphrase: a free label creates the identity first (funded after the
//! same confirmation) and then the name, signed with the identity's HIGH
//! key. A label DPNS rejects and a premium one are refused before anything
//! is looked up or funded.
void PlatformTests::usernameCreatesTheIdentityFirst()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    int unlocks{0};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {NotFound()};
        client.resolved_names = {Resolved(std::nullopt)};
        auto created{Found(7, 2'999'000'000)};
        created.value.public_keys = RegisteredKeys();
        client.registered_identities = {{created, {platform::FundingOutcome::OK, 0}}};
        client.registered_names = {Resolved(1)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow identity_flow{service, confirmations.get(), InstantProof};
    UsernameFlow flow{service, identity_flow};
    identity_flow.lookUp();
    sources.fake->deliverAll();
    QVERIFY(identity_flow.lookedUp() && !identity_flow.identity());
    QCOMPARE(unlocks, 1);

    const size_t txs{wallet.getWalletTxs().size()};
    for (const char* label : {"a", "-alice2-", "alice"}) {
        flow.registerName(label);
        QVERIFY2(flow.step() == UsernameFlow::Step::IDLE, label);
        QVERIFY2(!flow.error().isEmpty(), label);
    }
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{0});
    QVERIFY(confirmations.asked.empty());
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QCOMPARE(unlocks, 1);

    flow.registerName("alice2026");
    QVERIFY(sources.fake->calls.back().method == "resolveName");
    QVERIFY(sources.fake->calls.back().argument == "alice2026");
    sources.fake->deliverAll();
    QCOMPARE(confirmations.asked.size(), size_t{1});
    QCOMPARE(confirmations.asked.back().first, IdentityFlow::REGISTRATION_AMOUNT);
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{1});
    QVERIFY(sources.fake->calls.back().argument == "alice2026");
    QVERIFY(sources.fake->calls.back().identity_index == 0U);
    QVERIFY(sources.fake->name_signers.back().first == identity_flow.identity()->id);
    QCOMPARE(sources.fake->name_signers.back().second, uint32_t{2});
    QVERIFY(flow.username() == "alice2026");
    QVERIFY(flow.error().isEmpty());
    QCOMPARE(unlocks, 2);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
}

//! An identity without a name registers one directly, with no asset lock:
//! not with a disabled key or a key of another purpose, nor a label
//! someone else holds, and always refusing to join a contest: a label
//! Dash Platform would put to a vote is refused, said, and charged nothing.
//! A label that resolves to the identity is its own.
//! A rebuild that drops the lookup of a label looks it up again. The tab
//! header shows the name; registering is offered only once the identity's
//! names were read and it has none, and none of its usernames waits for a
//! masternode vote.
void PlatformTests::usernameForAnExistingIdentity()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    FakeSources sources;
    int clients{0};
    sources.script = [&clients](FakePlatformClient& client) {
        auto found{Found(5, 1'000'000)};
        found.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER),
                                   Key(1, Purpose::AUTHENTICATION, Level::CRITICAL),
                                   Key(2, Purpose::AUTHENTICATION, Level::HIGH, /*disabled_at=*/1),
                                   Key(3, Purpose::ENCRYPTION, Level::HIGH)};
        // The first session's identity has no usable HIGH key; later ones added one.
        if (clients++ == 0) {
            client.resolved_names = {Resolved(std::nullopt), Resolved(5)};
        } else {
            found.value.public_keys.push_back(Key(6, Purpose::AUTHENTICATION, Level::HIGH));
            client.resolved_names = {Resolved(8), Resolved(std::nullopt), Resolved(std::nullopt)};
            platform::Result<platform::Identifier> contested;
            contested.status.kind = platform::StatusKind::CONTESTED;
            client.registered_names = {contested, Resolved(1)};
        }
        client.identities_by_pubkey_hash = {found};
        client.contests = {Contests(std::nullopt)};
        // The client a rebuild makes reads no names: the identity's are known,
        // and a contested label wrote nothing that would change them.
        if (clients < 3) client.names = {Names({})};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    const auto* header{page.findChild<QLabel*>("dashPayUsername")};
    auto* input{page.findChild<QLineEdit*>("dashPayUsernameInput")};
    auto* submit{page.findChild<QPushButton*>("dashPayRegisterUsername")};
    sources.fake->deliverNext();
    QVERIFY(page.identityFlow()->identity());
    QVERIFY(!page.usernameFlow()->namesRead());
    QVERIFY(!submit->isVisibleTo(&page));
    sources.fake->deliverAll();
    QVERIFY(!header->isVisibleTo(&page));
    QVERIFY(input->isVisibleTo(&page) && submit->isVisibleTo(&page));
    const size_t txs{wallet.getWalletTxs().size()};

    input->setText("bob-the-builder-9");
    submit->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{1});
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{0});
    QVERIFY(page.findChild<QLabel*>("dashPayUsernameStatus")->isVisibleTo(&page));

    input->setText("mine-already-2");
    submit->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{0});
    QCOMPARE(header->text(), QString{"mine-already-2"});
    QVERIFY(header->isVisibleTo(&page) && !submit->isVisibleTo(&page));

    // The next session's identity has a usable key and, as read, no name.
    f.setNetworkActive(false);
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QVERIFY(submit->isVisibleTo(&page) && !header->isVisibleTo(&page));
    input->setText("taken-name-2");
    submit->click();
    // A rebuild drops the read: the label is resolved again on the new client.
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{3});
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{1});
    QVERIFY(std::any_of(sources.fake->calls.begin(), sources.fake->calls.end(), [](const auto& call) {
        return call.method == "resolveName" && call.argument == "taken-name-2";
    }));
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{0});
    QVERIFY(submit->isVisibleTo(&page));

    input->setText("vote-on-me-2");
    submit->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{1});
    QVERIFY(sources.fake->contest_policies.back().kind == platform::ContestPolicy::Kind::REFUSE);
    QVERIFY(VisibleTexts(page).join(' ').contains("put this username to a vote"));
    QVERIFY(!header->isVisibleTo(&page) && submit->isVisibleTo(&page));

    input->setText("bob-the-builder-9");
    submit->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{2});
    QVERIFY(sources.fake->calls.back().argument == "bob-the-builder-9");
    QVERIFY(sources.fake->contest_policies.back().kind == platform::ContestPolicy::Kind::REFUSE);
    QVERIFY(sources.fake->name_signers.back().first == page.identityFlow()->identity()->id);
    QCOMPARE(sources.fake->name_signers.back().second, uint32_t{6});
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(header->isVisibleTo(&page));
    QCOMPARE(header->text(), QString{"bob-the-builder-9"});
    QVERIFY(!input->isVisibleTo(&page) && !submit->isVisibleTo(&page));
}

//! One write at a time per wallet, across the three flows: no top-up starts
//! while a username write is in flight, no username is sent while another
//! write is or a contact is being added, and no contact request while a
//! username is being registered. A username write the client dropped is not
//! sent again; the identity's names, read again once the identity is found,
//! show that it landed.
void PlatformTests::droppedUsernameWriteIsReadBack()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    int clients{0};
    sources.script = [&clients](FakePlatformClient& client) {
        auto found{Found(5, 1'000'000'000)};
        found.value.public_keys = RegisteredKeys();
        client.identities_by_pubkey_hash = {found};
        client.names = {Names(clients++ == 0 ? std::vector<std::string>{} : std::vector<std::string>{"carol2026"})};
        client.resolved_names = {Resolved(std::nullopt), Resolved(std::nullopt), Resolved(std::nullopt), Resolved(9)};
        auto contact{Found(9, 0)};
        contact.value.public_keys = {Key(1, Purpose::DECRYPTION, Level::MEDIUM)};
        client.identities = {contact};
        client.sent_requests = {Requests({})};
        client.received_requests = {Requests({})};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow identity_flow{service, confirmations.get(), InstantProof};
    UsernameFlow flow{service, identity_flow};
    ContactFlow contacts{service, identity_flow};
    sources.fake->deliverAll();
    QVERIFY(flow.namesRead() && !flow.username());
    const auto names_sent = [&](const std::string& label) {
        const auto& calls{sources.fake->calls};
        return std::count_if(calls.begin(), calls.end(), [&](const FakePlatformClient::Call& call) {
            return call.method == "registerName" && call.argument == label;
        });
    };

    flow.registerName("carol2026");
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::REGISTERING);
    identity_flow.topUp(IdentityFlow::MIN_TOP_UP);
    QVERIFY(confirmations.asked.empty());
    QVERIFY(!identity_flow.funding());

    f.setNetworkActive(false);
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE);
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QVERIFY(flow.username() == "carol2026");
    QCOMPARE(names_sent("carol2026"), 0);
    QCOMPARE(sources.fake->countCalls("namesOfIdentity"), size_t{1});

    // Another write began while the label was resolved.
    flow.registerName("dave2026");
    bool written{false};
    PendingWrite(service, f.wallet_model.wallet(), written);
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE && !flow.error().isEmpty());
    sources.fake->deliverAll();
    QCOMPARE(names_sent("dave2026"), 0);

    // So did a top-up.
    flow.registerName("erin2026");
    identity_flow.topUp(IdentityFlow::MIN_TOP_UP);
    QVERIFY(identity_flow.step() == IdentityFlow::Step::SENDING);
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE && !flow.error().isEmpty());
    sources.fake->deliverAll();
    QCOMPARE(names_sent("erin2026"), 0);

    // A free label resolved while a contact is being added.
    flow.registerName("frank2026");
    contacts.add("grace2026");
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE && !flow.error().isEmpty());
    QCOMPARE(names_sent("frank2026"), 0);
    // That contact found while a username is being checked.
    sources.fake->deliverNext();
    QVERIFY(contacts.step() == ContactFlow::Step::FETCHING);
    flow.registerName("heidi2026");
    sources.fake->deliverNext();
    QVERIFY(contacts.step() == ContactFlow::Step::IDLE && !contacts.error().isEmpty());
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{0});
}

//! Adding a contact by username sends one request from the identity's first
//! enabled ENCRYPTION key, signed with its HIGH key, to the contact's enabled
//! DECRYPTION key, preferred to an ENCRYPTION key. Our receiving chain
//! towards them is imported first, and the accountReference is over that
//! chain. A label DPNS rejects, our own name, a name nobody holds, a contact
//! with no key to encrypt to and one already asked send nothing. A rebuild
//! that drops the lookup of the name or of the contact makes it again. A
//! request the client dropped is not sent again: our requests, read again,
//! show that it landed.
void PlatformTests::contactRequestUsesTheIdentityKeys()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER), Key(2, Purpose::AUTHENTICATION, Level::HIGH),
                              Key(3, Purpose::ENCRYPTION, Level::MEDIUM, /*disabled_at=*/1),
                              Key(7, Purpose::ENCRYPTION, Level::MEDIUM)};
    auto keyless{Found(8, 0)};
    keyless.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER),
                                 Key(1, Purpose::DECRYPTION, Level::MEDIUM, /*disabled_at=*/1)};
    auto bob{Found(9, 0)};
    bob.value.public_keys = {Key(1, Purpose::ENCRYPTION, Level::MEDIUM),
                             Key(2, Purpose::DECRYPTION, Level::MEDIUM, /*disabled_at=*/1),
                             Key(3, Purpose::DECRYPTION, Level::MEDIUM)};
    auto carol{bob};
    carol.value.id.fill(10);
    FakeSources sources;
    int clients{0};
    sources.script = [&](FakePlatformClient& client) {
        switch (clients++) {
        case 0:
            client.identities_by_pubkey_hash = {ours};
            client.sent_requests = {Requests({}), Requests({Request(5, 9, 1)})};
            client.received_requests = {Requests({})};
            client.resolved_names = {Resolved(5), Resolved(std::nullopt), Resolved(8), Resolved(9), Resolved(9)};
            client.identities = {keyless, bob};
            client.sent_contact_requests = {Resolved(1)};
            break;
        case 1: // rebuilt while carol's name resolved
            client.resolved_names = {Resolved(10)};
            break;
        case 2: // rebuilt while carol's identity was read
            client.identities = {carol};
            break;
        default:
            client.identities_by_pubkey_hash = {ours};
            client.sent_requests = {Requests({Request(5, 9, 1), Request(5, 10, 2)})};
            client.received_requests = {Requests({})};
        }
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow identity_flow{service, confirmations.get()};
    ContactFlow flow{service, identity_flow};
    sources.fake->deliverAll();
    QVERIFY(flow.swept() && flow.contacts().empty());

    for (const char* name : {"-x-", "me2026", "nobody2026", "keyless2026"}) {
        flow.add(name);
        sources.fake->deliverAll();
        QVERIFY2(flow.step() == ContactFlow::Step::IDLE && !flow.error().isEmpty(), name);
    }
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{3});
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{1});
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{0});

    const size_t chains{f.wallet->GetAllScriptPubKeyMans().size()};
    flow.add("bob2026");
    sources.fake->deliverNext();
    sources.fake->deliverNext();
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{1});
    QVERIFY(sources.fake->calls.back().identity_index == 0U);
    const platform::ContactRequestInput input{sources.fake->contact_inputs.back()};
    QVERIFY(input.sender == ours.value.id);
    QVERIFY(input.recipient == bob.value.id);
    QCOMPARE(input.sender_key_index, uint32_t{7});
    QCOMPARE(input.recipient_key_index, uint32_t{3});
    QCOMPARE(input.signing_key_id, uint32_t{2});
    // The chain the request carries was imported: deriving it again adds none.
    QCOMPARE(f.wallet->GetAllScriptPubKeyMans().size(), chains + 1);
    const auto chain{
        wallet.ensureFriendshipReceivingKeychain({0, uint256{ours.value.id}, uint256{bob.value.id}, GetTime()})};
    QVERIFY(chain);
    QCOMPARE(f.wallet->GetAllScriptPubKeyMans().size(), chains + 1);
    CTxDestination first;
    QVERIFY(wallet::DeriveFriendshipPaymentDestination(chain.value, 0, first));
    QVERIFY(wallet.isSpendable(first));
    wallet::CompactXpub compact;
    QVERIFY(wallet::CompactXpubBytes(chain.value, compact));
    const auto reference{wallet.platformAccountReference({0, 7}, compact)};
    QVERIFY(reference);
    QCOMPARE(input.account_reference, reference.value);

    sources.fake->deliverAll();
    QVERIFY(flow.error().isEmpty());
    QCOMPARE(flow.contacts().size(), size_t{1});
    QVERIFY(flow.contacts().front().id == bob.value.id);
    QVERIFY(flow.contacts().front().state == ContactFlow::State::OUTGOING);

    flow.add("bob2026");
    sources.fake->deliverAll();
    QVERIFY(!flow.error().isEmpty());
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{2});

    flow.add("carol2026");
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{1});
    QVERIFY(sources.fake->calls.back().argument == "carol2026");
    sources.fake->deliverNext();
    sources.evonodes = {Ipv4Evonode()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{3});
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{1});
    QVERIFY(sources.fake->calls.back().argument == HexStr(carol.value.id));
    sources.fake->deliverNext();
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{1});
    QVERIFY(sources.fake->contact_inputs.back().recipient == carol.value.id);
    f.setNetworkActive(false);
    QVERIFY(flow.step() == ContactFlow::Step::IDLE);
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QCOMPARE(sources.made.size(), size_t{4});
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{0});
    QCOMPARE(flow.contacts().size(), size_t{2});
    QVERIFY(flow.contacts().back().id == carol.value.id);
}

//! The contacts are what the requests on Platform say, newest per
//! counterparty: a request each way is a contact, theirs alone waits for
//! Accept and ours alone for them. Accepting reads their newest request and
//! takes only one carrying a valid payment key, sent from an enabled
//! ENCRYPTION or AUTHENTICATION key of theirs to one of our keys; our
//! request goes back to their DECRYPTION key, and they are then a contact.
void PlatformTests::contactsAreTheNewestRequests()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    CKey dave_key, erin_key, frank_key, grace_key, heidi_key;
    for (CKey* key : {&dave_key, &erin_key, &frank_key, &grace_key, &heidi_key}) {
        key->MakeNewKey(/*fCompressed=*/true);
    }
    CExtKey chain;
    const std::array<std::byte, 32> seed{};
    chain.SetSeed(seed);
    const CPubKey valid{chain.Neuter().pubkey};
    std::vector<uint8_t> invalid(CPubKey::COMPRESSED_SIZE, 0xff);
    invalid[0] = 0x02;
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    // Bob (0x0b) answered ours; carol (0x0c) was asked twice and didn't.
    // Dave's newest request carries a valid payment key, erin's doesn't;
    // frank sent his from a key since disabled, grace from an AUTHENTICATION
    // key, and heidi to a key we don't have.
    const auto sent{Requests({Request(5, 0x0b, 1), Request(5, 0x0c, 5), Request(5, 0x0c, 9)})};
    auto with_dave{sent};
    with_dave.value.requests.push_back(Request(5, 0x0d, 10));
    auto with_grace{with_dave};
    with_grace.value.requests.push_back(Request(5, 0x10, 11));
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        const auto received{Requests(
            {Request(0x0b, 5, 2), IncomingRequest(wallet, 0x0d, 5, dave_key, 2, invalid),
             IncomingRequest(wallet, 0x0d, 5, dave_key, 8, valid), IncomingRequest(wallet, 0x0e, 5, erin_key, 3, valid),
             IncomingRequest(wallet, 0x0e, 5, erin_key, 7, invalid),
             IncomingRequest(wallet, 0x0f, 5, frank_key, 4, valid), IncomingRequest(wallet, 0x10, 5, grace_key, 4, valid),
             IncomingRequest(wallet, 0x11, 5, heidi_key, 4, valid, /*our_key=*/9)})};
        client.identities_by_pubkey_hash = {ours};
        // Ours, then bob's and carol's as our requests are read, then dave's
        // once his is.
        client.names = {Names({"me2026"}), Names({}), Names({}), Names({"dave2026"})};
        // The first sweep of the received requests comes in two pages.
        auto first_page{received};
        first_page.value.requests.resize(3);
        first_page.value.complete = false;
        first_page.value.next_since_ms = 8;
        auto second_page{received};
        second_page.value.requests.erase(second_page.value.requests.begin(), second_page.value.requests.begin() + 3);
        client.sent_requests = {sent, with_dave, with_grace, with_grace};
        client.received_requests = {first_page, second_page, received};
        client.identities = {Peer(0x0d, dave_key, Purpose::ENCRYPTION), Peer(0x0e, erin_key, Purpose::ENCRYPTION),
                             Peer(0x0f, frank_key, Purpose::ENCRYPTION, /*disabled_at=*/1),
                             Peer(0x10, grace_key, Purpose::AUTHENTICATION), Peer(0x11, heidi_key, Purpose::ENCRYPTION)};
        client.sent_contact_requests = {Resolved(1), Resolved(2)};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    const ContactFlow& flow{*page.contactFlow()};
    using State = ContactFlow::State;
    using Listed = std::vector<std::pair<uint8_t, State>>;
    const auto contacts = [&] {
        Listed contacts;
        for (const auto& contact : flow.contacts()) {
            contacts.emplace_back(contact.id[0], contact.state);
        }
        return contacts;
    };
    QVERIFY(contacts() == (Listed{{0x0d, State::INCOMING},
                                  {0x0e, State::INCOMING},
                                  {0x0f, State::INCOMING},
                                  {0x10, State::INCOMING},
                                  {0x11, State::INCOMING},
                                  {0x0b, State::ESTABLISHED},
                                  {0x0c, State::OUTGOING}}));
    QVERIFY(flow.contacts().front().username == "dave2026");
    QVERIFY(page.findChild<QTableWidget*>("dashPayContacts")->isVisibleTo(&page));
    const auto accept_button = [&](uint8_t id) -> QPushButton* {
        QPushButton* button{ContactButton(page, id)};
        return button && button->objectName() == "dashPayAcceptContact" ? button : nullptr;
    };
    for (uint8_t id : {0x0d, 0x0e, 0x0f, 0x10, 0x11}) {
        QVERIFY(accept_button(id));
    }
    QVERIFY(!accept_button(0x0b) && !accept_button(0x0c));

    const size_t chains{f.wallet->GetAllScriptPubKeyMans().size()};
    for (uint8_t id : {0x0d, 0x0e, 0x0f, 0x10, 0x11}) {
        accept_button(id)->click();
        sources.fake->deliverAll();
        const bool accepted{id == 0x0d || id == 0x10};
        QVERIFY(flow.error().isEmpty() == accepted);
        QVERIFY(page.findChild<QLabel*>("dashPayContactsStatus")->isVisibleTo(&page) != accepted);
    }
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{5});
    QCOMPARE(sources.fake->contact_inputs.size(), size_t{2});
    for (const platform::ContactRequestInput& input : sources.fake->contact_inputs) {
        QCOMPARE(input.recipient_key_index, uint32_t{2});
        QCOMPARE(input.sender_key_index, uint32_t{4});
        QCOMPARE(input.signing_key_id, uint32_t{2});
    }
    QCOMPARE(sources.fake->contact_inputs[0].recipient[0], uint8_t{0x0d});
    QCOMPARE(sources.fake->contact_inputs[1].recipient[0], uint8_t{0x10});
    QCOMPARE(f.wallet->GetAllScriptPubKeyMans().size(), chains + 2);
    QVERIFY(contacts() == (Listed{{0x0e, State::INCOMING},
                                  {0x0f, State::INCOMING},
                                  {0x11, State::INCOMING},
                                  {0x0b, State::ESTABLISHED},
                                  {0x0d, State::ESTABLISHED},
                                  {0x10, State::ESTABLISHED},
                                  {0x0c, State::OUTGOING}}));

    VisibleButton(page, "dashPayRefreshContacts")->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("receivedRequests"), size_t{3});
    QCOMPARE(flow.contacts().size(), size_t{7});
}

//! Each direction is read a page at a time and keeps what it read: a page
//! that fails leaves the contacts of the pages before it, shows the binding's
//! message, and Refresh goes on from where it stopped. Our requests are read
//! whatever happens to theirs, and a contact whose request was read can be
//! accepted. Only the newest incoming requests are listed, with a count of
//! the rest, and only listed contacts have their names read.
void PlatformTests::contactPagesKeepWhatWasRead()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    const std::string saturated{"too many contact requests share one created_at"};
    std::vector<platform::ContactRequest> many;
    for (uint16_t i = 0; i < 500; ++i) {
        auto request{Request(0, 5, 100 + i)};
        request.owner[0] = uint8_t(i & 0xff);
        request.owner[1] = uint8_t((i >> 8) + 1);
        many.push_back(request);
    }
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"})};
        auto first{Requests({Request(0x0b, 5, 10), Request(0x0c, 5, 20)})};
        first.value.complete = false;
        first.value.next_since_ms = 20;
        auto stuck{Unavailable<platform::ContactRequestPage>()};
        stuck.status.message = saturated;
        client.received_requests = {first, stuck, Requests(many)};
        client.sent_requests = {Requests({}), Requests({})};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    QCoreApplication::processEvents();
    const ContactFlow& flow{*page.contactFlow()};
    QVERIFY(flow.sentRead() && !flow.swept());
    QCOMPARE(flow.contacts().size(), size_t{2});
    QVERIFY(flow.contacts().front().id[0] == 0x0c && flow.contacts().back().id[0] == 0x0b);
    QVERIFY(VisibleTexts(page).join(' ').contains(QString::fromStdString(saturated)));
    const auto* list{page.findChild<QTableWidget*>("dashPayContacts")};
    QVERIFY(qobject_cast<QPushButton*>(list->cellWidget(0, 2)));
    QCOMPARE(sources.fake->countCalls("receivedRequests"), size_t{2});
    QVERIFY(sources.fake->request_since.back() == 20);

    const size_t before{sources.fake->calls.size()};
    VisibleButton(page, "dashPayRefreshContacts")->click();
    sources.fake->deliverAll();
    QCoreApplication::processEvents();
    QCOMPARE(sources.fake->countCalls("receivedRequests"), size_t{3});
    QVERIFY(sources.fake->request_since.back() == 20);
    QVERIFY(flow.swept());
    QCOMPARE(flow.moreIncoming(), size_t{402});
    QCOMPARE(flow.contacts().size(), ContactFlow::INCOMING_SHOWN);
    QCOMPARE(flow.contacts().front().id[0], uint8_t{499 & 0xff});
    QCOMPARE(list->rowCount(), int(ContactFlow::INCOMING_SHOWN) + 1);
    // Names are read for listed rows only: the two listed before, again, and
    // the newest incoming.
    std::set<std::string> listed;
    for (const auto& contact : flow.contacts())
        listed.insert(HexStr(contact.id));
    size_t names{0};
    for (size_t call = before; call < sources.fake->calls.size(); ++call) {
        const auto& [method, argument, index]{sources.fake->calls[call]};
        if (method != "namesOfIdentity") continue;
        ++names;
        QVERIFY(listed.count(argument) || argument == HexStr(Request(0x0b, 5, 0).owner) ||
                argument == HexStr(Request(0x0c, 5, 0).owner));
    }
    QVERIFY(names <= ContactFlow::INCOMING_SHOWN + 2);
}

namespace {
//! Our receiving chain towards someone we sent a request to, as the wallet
//! derives it, and its first address.
std::pair<wallet::FriendshipReceivingChain, CTxDestination> OurChain(interfaces::Wallet& wallet,
                                                                     const platform::Identifier& ours,
                                                                     const platform::Identifier& theirs)
{
    const auto chain{wallet.getFriendshipReceivingXpub({0, uint256{ours}, uint256{theirs}, 0})};
    CTxDestination first;
    Assert(chain && wallet::DeriveFriendshipPaymentDestination(chain.value.xpub, 0, first));
    return {chain.value, first};
}

//! Pays 5 DASH to `to` in a new block from a key outside the wallet, funded
//! by coinbase transaction `coinbase`: the wallet finds it only by scanning
//! that block.
void PayFromOutside(FundedFixture& f, size_t coinbase, const CTxDestination& to)
{
    const auto notifications{f.chain.m_node.chain->handleNotifications(f.wallet)};
    CKey outside;
    outside.MakeNewKey(true);
    const CScript miner{GetScriptForRawPubKey(f.chain.coinbaseKey.GetPubKey())};
    const auto funded{f.chain.CreateValidMempoolTransaction(f.chain.m_coinbase_txns[coinbase], 0, int(coinbase) + 1,
                                                            f.chain.coinbaseKey,
                                                            GetScriptForDestination(PKHash{outside.GetPubKey()}),
                                                            6 * COIN, /*submit=*/false)};
    f.chain.CreateAndProcessBlock({funded}, miner);
    const auto payment{f.chain.CreateValidMempoolTransaction(MakeTransactionRef(funded), 0, 2, outside,
                                                             GetScriptForDestination(to), 5 * COIN, /*submit=*/false)};
    f.chain.CreateAndProcessBlock({payment}, miner);
    SyncWithValidationInterfaceQueue();
    Assert(!f.wallet_model.wallet().getTx(payment.GetHash()));
}

//! Counts the rescans the wallet starts.
struct Rescans {
    std::atomic<int> started{0};
    std::unique_ptr<interfaces::Handler> handler;
    explicit Rescans(interfaces::Wallet& wallet) :
        handler{wallet.handleShowProgress([this](const std::string&, int progress) {
            if (progress == 0) ++started;
        })}
    {
    }
};
} // namespace

//! A wallet restored from its seed imports our receiving chain towards
//! everyone we ever sent a request to once our requests are read, though
//! theirs can't be: born at the earliest request, not the newest, so a
//! payment made between two requests is found and counted. One scan covers
//! both chains, and the next session, which the wallet owes nothing, scans
//! nothing.
void PlatformTests::sentRequestsRecoverTheirChains()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    platform::Identifier bob;
    bob.fill(0x0b);
    const auto [chain, address]{OurChain(wallet, ours.value.id, bob)};
    QVERIFY(!chain.birth_time);
    int64_t paid_at;
    {
        const auto notifications{f.chain.m_node.chain->handleNotifications(f.wallet)};
        const auto payment{f.chain.CreateValidMempoolTransaction(f.chain.m_coinbase_txns[1], 0, 2, f.chain.coinbaseKey,
                                                                 GetScriptForDestination(address), 5 * COIN,
                                                                 /*submit=*/false)};
        paid_at = f.chain.CreateAndProcessBlock({payment}, GetScriptForRawPubKey(f.chain.coinbaseKey.GetPubKey())).GetBlockTime();
        SyncWithValidationInterfaceQueue();
    }
    const CAmount before{wallet.getBalance()};
    constexpr int64_t DAY{24 * 60 * 60};
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"})};
        client.sent_requests = {
            Requests({Request(5, 0x0b, uint64_t(paid_at + 2 * DAY) * 1000),
                      Request(5, 0x0b, uint64_t(paid_at - DAY) * 1000), Request(5, 0x0c, uint64_t(paid_at) * 1000)})};
        client.received_requests = {Unavailable<platform::ContactRequestPage>()};
    };
    Rescans rescans{wallet};
    {
        PlatformPage page{sources.get()};
        f.attach(page);
        sources.fake->deliverAll();
        QVERIFY(page.contactFlow()->sentRead() && !page.contactFlow()->swept());
        QTRY_VERIFY(!page.contactFlow()->rescanning());
    }
    QCOMPARE(rescans.started.load(), 1);
    QVERIFY(OurChain(wallet, ours.value.id, bob).first.birth_time == paid_at - DAY);
    QVERIFY(wallet.isSpendable(address));
    QCOMPARE(wallet.getBalance(), before + 5 * COIN);
    QVERIFY(!wallet.owedRescanFrom());

    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    QVERIFY(page.contactFlow()->sentRead());
    QTRY_VERIFY(!page.contactFlow()->rescanning());
    QCOMPARE(rescans.started.load(), 1);
}

//! A locked wallet imports the chains once it is unlocked: the tab offers
//! the unlock, counting the contacts whose payments wait for it. The next
//! session, with the chains held, offers none.
void PlatformTests::lockedWalletImportsChainsOnUnlock()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    platform::Identifier bob;
    bob.fill(0x0b);
    const CTxDestination address{OurChain(wallet, ours.value.id, bob).second};
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    int unlocks{0};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"})};
        client.sent_requests = {Requests({Request(5, 0x0b, 1'000)})};
        client.received_requests = {Requests({})};
    };
    {
        PlatformPage page{sources.get()};
        f.attach(page);
        page.identityFlow()->lookUp();
        sources.fake->deliverAll();
        QCOMPARE(unlocks, 1);
        QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
        QVERIFY(!wallet.isSpendable(address));
        QCOMPARE(page.contactFlow()->chainsToImport(), size_t{1});
        QVERIFY(VisibleTexts(page).join(' ').contains("Unlock the wallet to add them."));
        QVERIFY(VisibleButton(page, "dashPayRetryRecovery"));
        QPushButton* import{VisibleButton(page, "dashPayImportChains")};
        QVERIFY(import);
        import->click();
        QCOMPARE(unlocks, 2);
        QVERIFY(wallet.isSpendable(address));
        QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
        QVERIFY(!VisibleButton(page, "dashPayImportChains"));
        QVERIFY(!VisibleButton(page, "dashPayRetryRecovery"));
        QTRY_VERIFY(!page.contactFlow()->rescanning());
    }

    PlatformPage page{sources.get()};
    f.attach(page);
    page.identityFlow()->lookUp();
    sources.fake->deliverAll();
    QCOMPARE(unlocks, 3);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(page.contactFlow()->sentRead());
    QCOMPARE(page.contactFlow()->chainsToImport(), size_t{0});
    QCOMPARE(page.contactFlow()->chainsImported(), size_t{1});
    QVERIFY(!VisibleButton(page, "dashPayImportChains"));
    QVERIFY(!VisibleTexts(page).join(' ').contains("Unlock the wallet to add them."));
    QVERIFY(!VisibleButton(page, "dashPayRetryRecovery"));
    QTRY_VERIFY(!page.contactFlow()->rescanning());
}

//! On a node missing old blocks the chain is imported all the same, the
//! blocks it has are read, and the tab names the first block it could not.
//! A chain imported later gets that partial scan again, which finds a
//! payment to it in a block the node has. A scan that did not run, because
//! another rescan held the wallet or it was cancelled, names nothing and runs
//! at a block once its wait has passed, finding the payment with no refresh.
void PlatformTests::missingBlocksAreNamed()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    const auto chain_to{[&](uint8_t theirs) {
        platform::Identifier id;
        id.fill(theirs);
        return OurChain(wallet, ours.value.id, id).second;
    }};
    const CTxDestination address{chain_to(0x0b)};
    PayFromOutside(f, 1, chain_to(0x0c));
    PayFromOutside(f, 2, chain_to(0x0d));
    PayFromOutside(f, 3, chain_to(0x0e));
    struct MissingBlocks {
        ChainstateManager& chainman;
        void set(bool have)
        {
            LOCK(::cs_main);
            for (int height{20}; height <= 30; ++height) {
                CBlockIndex* index{chainman.ActiveChain()[height]};
                index->nStatus = have ? index->nStatus | BLOCK_HAVE_DATA : index->nStatus & ~BLOCK_HAVE_DATA;
            }
        }
        explicit MissingBlocks(ChainstateManager& chainman_in) :
            chainman{chainman_in}
        {
            set(false);
        }
        ~MissingBlocks() { set(true); }
    } missing{*f.chain.m_node.chainman};
    // Bob's chain, then Carol's from a later refresh.
    {
        FakeSources sources;
        sources.script = [&](FakePlatformClient& client) {
            client.identities_by_pubkey_hash = {ours};
            client.names = {Names({"me2026"})};
            client.sent_requests = {Requests({Request(5, 0x0b, 0)}), Requests({Request(5, 0x0c, 1'000)})};
            client.received_requests = {Requests({}), Requests({})};
        };
        PlatformPage page{sources.get()};
        f.attach(page);
        sources.fake->deliverAll();
        QTRY_VERIFY(!page.contactFlow()->rescanning());
        QVERIFY(wallet.isSpendable(address));
        QVERIFY(page.contactFlow()->rescanNote().contains("block 20"));
        QVERIFY(VisibleTexts(page).join(' ').contains(page.contactFlow()->rescanNote()));
        // The wallet still owes it, from the block the node lacks on.
        QVERIFY(wallet.owedRescanFrom().has_value());
        const QString report{page.findChild<QLabel*>("dashPayRecoveryStatus")->text()};
        QVERIFY(report.contains("blocks from 20 on") && report.contains("-reindex"));
        const CAmount before{wallet.getBalance()};
        page.contactFlow()->refresh();
        sources.fake->deliverAll();
        QTRY_VERIFY(!page.contactFlow()->rescanning());
        QCOMPARE(wallet.getBalance(), before + 5 * COIN);
    }
    // Dave's, while another rescan holds the wallet; then Erin's, cancelled.
    for (const bool cancel : {false, true}) {
        const uint8_t theirs{cancel ? uint8_t{0x0e} : uint8_t{0x0d}};
        std::atomic<int> cancels{cancel ? 1 : 0};
        const auto canceller{wallet.handleShowProgress([&](const std::string&, int progress) {
            if (progress == 0 && cancels-- > 0) wallet.abortRescan();
        })};
        FakeSources sources;
        sources.script = [&](FakePlatformClient& client) {
            client.identities_by_pubkey_hash = {ours};
            client.names = {Names({"me2026"})};
            client.sent_requests = {Requests({Request(5, 0x0b, 0), Request(5, theirs, 2'000)})};
            client.received_requests = {Requests({})};
        };
        PlatformPage page{sources.get()};
        const CAmount before{wallet.getBalance()};
        {
            wallet::WalletRescanReserver held{*f.wallet};
            if (!cancel) QVERIFY(held.reserve());
            f.attach(page);
            sources.fake->deliverAll();
            QTRY_VERIFY(!page.contactFlow()->rescanning());
        }
        QVERIFY(page.contactFlow()->rescanNote().isEmpty());
        TipChanged(f.models.client);
        QVERIFY(!page.contactFlow()->rescanning());
        QCOMPARE(wallet.getBalance(), before);
        {
            const MockTime later{GetTime<std::chrono::seconds>() + std::chrono::minutes{1}};
            TipChanged(f.models.client);
            QTRY_VERIFY(!page.contactFlow()->rescanning());
        }
        QCOMPARE(wallet.getBalance(), before + 5 * COIN);
        QVERIFY(page.contactFlow()->rescanNote().contains("block 20"));
    }
}

//! Registering is offered only once the identity's names are proved none:
//! a read that fails says why and is not "none", and is retried at the next
//! block once its wait has passed, here finding a name, so Register is never
//! offered in between. A header, or a block within the wait, reads nothing.
void PlatformTests::unreadNamesNeverOfferRegister()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        auto found{Found(5, 1'000'000)};
        found.value.public_keys = RegisteredKeys();
        client.identities_by_pubkey_hash = {found, found, found};
        client.names = {Unavailable<platform::Names>(), Names({"frank2026"})};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    const auto* header{page.findChild<QLabel*>("dashPayUsername")};
    auto* submit{page.findChild<QPushButton*>("dashPayRegisterUsername")};
    sources.fake->deliverAll();
    QVERIFY(page.usernameFlow()->names() == UsernameFlow::Names::FAILED);
    QVERIFY(!submit->isVisibleTo(&page));
    QVERIFY(VisibleTexts(page).contains(page.usernameFlow()->namesError()));
    page.usernameFlow()->registerName("frank-again-2");
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{0});

    TipChanged(f.models.client);
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("namesOfIdentity"), size_t{1});
    QVERIFY(!submit->isVisibleTo(&page));
    {
        const MockTime later{GetTime<std::chrono::seconds>() + UsernameFlow::NAMES_RETRY};
        TipChanged(f.models.client, SyncType::HEADER_SYNC);
        sources.fake->deliverAll();
        QCOMPARE(sources.fake->countCalls("namesOfIdentity"), size_t{1});
        TipChanged(f.models.client);
        sources.fake->deliverAll();
    }
    QCOMPARE(sources.fake->countCalls("namesOfIdentity"), size_t{2});
    QVERIFY(page.usernameFlow()->names() == UsernameFlow::Names::FOUND);
    QCOMPARE(header->text(), QString{"frank2026"});
    QVERIFY(!submit->isVisibleTo(&page));
}

//! Pay on a contact hands the Send tab an address of the payment chain their
//! newest request carries, labelled with their username, worked out again
//! each time from the wallet's payments: Pay never sent, however often,
//! and a restart leave the same address for the next one. A newer request
//! with another chain starts that chain at its first address. The wallet's
//! view fills its Send tab, asking before it replaces a payment being
//! written (an amount alone included, an entry being removed not), and the
//! confirmation names the contact.
void PlatformTests::payingAContactUsesEachAddressOnce()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    CKey bob_key, dave_key;
    bob_key.MakeNewKey(/*fCompressed=*/true);
    dave_key.MakeNewKey(/*fCompressed=*/true);
    CExtKey root, rotated;
    const std::array<std::byte, 32> seed{};
    root.SetSeed(seed);
    QVERIFY(root.Derive(rotated, 1));
    const auto chain{PaymentChain(root.Neuter().pubkey)};
    const auto new_chain{PaymentChain(rotated.Neuter().pubkey)};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        // Bob (0x0b) and we asked each other, carol (0x0c) hasn't answered
        // and dave (0x0d) waits for us. Read again, bob's newest request
        // carries another chain.
        const auto bob{IncomingRequest(wallet, 0x0b, 5, bob_key, 2, chain.pubkey)};
        const auto dave{IncomingRequest(wallet, 0x0d, 5, dave_key, 2, chain.pubkey)};
        const auto sent{Requests({Request(5, 0x0b, 1), Request(5, 0x0c, 1)})};
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"}), Names({"bob2026"}), Names({}), Names({})};
        client.sent_requests = {sent, sent};
        client.received_requests = {Requests({bob, dave}),
                                    Requests({bob, IncomingRequest(wallet, 0x0b, 5, bob_key, 3, new_chain.pubkey), dave})};
        client.identities.assign(40, Peer(0x0b, bob_key, Purpose::ENCRYPTION));
    };
    const auto at = [](const wallet::FriendshipXpub& payee, uint32_t index) {
        return QString::fromStdString(EncodeDestination(PaymentAddress(payee, index)));
    };

    auto page{std::make_unique<PlatformPage>(sources.get())};
    f.attach(*page);
    sources.fake->deliverAll();
    QVERIFY(ContactButton(*page, 0x0b) && ContactButton(*page, 0x0d));
    QVERIFY(ContactButton(*page, 0x0d)->objectName() != "dashPayPayContact");
    QVERIFY(!ContactButton(*page, 0x0c));
    for (uint8_t id : {0x0c, 0x0d}) {
        platform::Identifier contact;
        contact.fill(id);
        page->contactFlow()->pay(contact);
        QVERIFY(page->contactFlow()->step() == ContactFlow::Step::IDLE);
    }
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{0});
    const auto first{PayRecipient(*page, sources, 0x0b)};
    QVERIFY(first && first->address == at(chain, 0) && first->label == "bob2026");
    for (int unsent = 0; unsent < 11; ++unsent) {
        QCOMPARE(PayContact(*page, sources, 0x0b), at(chain, 0));
    }

    page = std::make_unique<PlatformPage>(sources.get());
    f.attach(*page);
    sources.fake->deliverAll();
    QCOMPARE(PayContact(*page, sources, 0x0b), at(chain, 0));

    VisibleButton(*page, "dashPayRefreshContacts")->click();
    sources.fake->deliverAll();
    QCOMPARE(PayContact(*page, sources, 0x0b), at(new_chain, 0));
    QCOMPARE(PayContact(*page, sources, 0x0b), at(new_chain, 0));

    // A locked wallet needs its passphrase to decrypt the chain: without it
    // no address is chosen.
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    bool give_passphrase{false};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        if (give_passphrase) f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    QCOMPARE(PayContact(*page, sources, 0x0b), QString{});
    QVERIFY(!page->contactFlow()->error().isEmpty());
    give_passphrase = true;
    QCOMPARE(PayContact(*page, sources, 0x0b), at(new_chain, 0));
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);

    // The wallet's view fills its Send tab, after asking about a payment
    // being written there.
    WalletView view{&f.wallet_model, nullptr};
    QSignalSpy prepared{&view, &WalletView::contactPaymentPrepared};
    view.gotoSendCoinsPage();
    auto* send{qobject_cast<SendCoinsDialog*>(view.currentWidget())};
    QVERIFY(send);
    SendCoinsRecipient bob;
    bob.address = at(chain, 1);
    bob.label = "bob2026";
    const auto entries = [&] {
        // A cleared entry is deleted later.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return send->findChildren<SendCoinsEntry*>();
    };
    // An amount alone is a payment being written: Cancel keeps it.
    send->clear();
    entries().front()->findChild<BitcoinAmountField*>("payAmount")->setValue(2 * COIN);
    Answer(QMessageBox::Cancel);
    Q_EMIT view.findChild<PlatformPage*>()->payTo(bob);
    QCoreApplication::processEvents();
    QCOMPARE(prepared.size(), 0);
    QCOMPARE(entries().front()->findChild<BitcoinAmountField*>("payAmount")->value(), 2 * COIN);
    // An entry being removed is not one. The cleared ones are deleted
    // first, as the event loop does before the next click.
    send->clear();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    SendCoinsEntry* removed{send->addEntry()};
    removed->findChild<QValidatedLineEdit*>("payTo")->setText("removed-address");
    removed->findChild<QAbstractButton*>("deleteButton")->click();
    Answer(QMessageBox::Cancel);
    Q_EMIT view.findChild<PlatformPage*>()->payTo(bob);
    QCoreApplication::processEvents();
    QCOMPARE(prepared.size(), 1);
    prepared.clear();
    send->clear();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    send->setAddress("draft-address");
    Answer(QMessageBox::Cancel);
    Q_EMIT view.findChild<PlatformPage*>()->payTo(bob);
    QCOMPARE(prepared.size(), 0);
    QCOMPARE(entries().size(), 1);
    QCOMPARE(entries().front()->findChild<QValidatedLineEdit*>("payTo")->text(), QString{"draft-address"});
    Answer(QMessageBox::Yes);
    Q_EMIT view.findChild<PlatformPage*>()->payTo(bob);
    QCOMPARE(prepared.size(), 1);
    QCOMPARE(entries().size(), 1);
    SendCoinsEntry* entry{entries().front()};
    QCOMPARE(entry->findChild<QValidatedLineEdit*>("payTo")->text(), at(chain, 1));
    QCOMPARE(entry->findChild<QLineEdit*>("addAsLabel")->text(), QString{"bob2026"});
    entry->findChild<BitcoinAmountField*>("payAmount")->setValue(COIN);
    // The Send tab checks the balance the model last read.
    f.wallet_model.updateTransaction();
    f.wallet_model.pollBalanceChanged();
    QString confirmation;
    ReadConfirmation(confirmation);
    QVERIFY(QMetaObject::invokeMethod(send, "sendButtonClicked", Q_ARG(bool, false)));
    QVERIFY(confirmation.contains("bob2026"));
    QVERIFY(confirmation.contains(at(chain, 1)));
}

//! The address follows the wallet's payments to the chain: from one past
//! the last payment in a block (or InstantSend locked), the lowest one no
//! live payment takes, never further than a receiver watching six addresses
//! ahead looks. An abandoned payment and one a conflicting payment replaced
//! take nothing, and one a reorg took out of its block no longer counts.
//! After all that, and a dozen Pays never sent, the next real payment
//! reaches the receiver's wallet.
void PlatformTests::paymentsFollowTheWalletHistory()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    CKey bob_key;
    bob_key.MakeNewKey(/*fCompressed=*/true);
    CExtKey root;
    const std::array<std::byte, 32> seed{};
    root.SetSeed(seed);
    const auto chain{PaymentChain(root.Neuter().pubkey)};
    // Bob's wallet watches his chain six addresses past the last one paid.
    CExtKey bob_chain{root};
    std::fill(bob_chain.chaincode.begin(), bob_chain.chaincode.end(), 0x11);
    auto receiver{MakeTestWallet(f.node, f.context, "bob")};
    QVERIFY(receiver);
    f.guard.keep(receiver);
    {
        LOCK(receiver->cs_wallet);
        receiver->m_keypool_size = 6;
        FlatSigningProvider provider;
        std::string error;
        auto descriptor{Parse("pkh(" + EncodeExtKey(bob_chain) + "/*)", provider, error, /*require_checksum=*/false)};
        QVERIFY(descriptor);
        wallet::WalletDescriptor watched{std::move(descriptor), 0, 0, 6, 0};
        QVERIFY(receiver->AddWalletDescriptor(watched, provider, "", false));
        const CBlockIndex* tip{WITH_LOCK(::cs_main, return f.chain.m_node.chainman->ActiveChain().Tip())};
        receiver->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    auto bob_wallet{interfaces::MakeWallet(f.context, receiver)};
    const auto ours_notified{f.chain.m_node.chain->handleNotifications(f.wallet)};
    const auto bob_notified{f.chain.m_node.chain->handleNotifications(receiver)};
    const CScript miner{GetScriptForRawPubKey(f.chain.coinbaseKey.GetPubKey())};
    const auto mine = [&](std::vector<CMutableTransaction> txs) {
        const uint256 hash{f.chain.CreateAndProcessBlock(txs, miner).GetHash()};
        SyncWithValidationInterfaceQueue();
        return hash;
    };
    const auto tx = [&](const uint256& txid) { return CMutableTransaction{*wallet.getTx(txid)}; };
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"})};
        client.sent_requests = {Requests({Request(5, 0x0b, 1)})};
        client.received_requests = {Requests({IncomingRequest(wallet, 0x0b, 5, bob_key, 2, chain.pubkey)})};
        client.identities.assign(40, Peer(0x0b, bob_key, Purpose::ENCRYPTION));
    };
    const auto at = [&](uint32_t index) {
        return QString::fromStdString(EncodeDestination(PaymentAddress(chain, index)));
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    for (int unsent = 0; unsent < 12; ++unsent) {
        QCOMPARE(PayContact(page, sources, 0x0b), at(0));
    }

    mine({tx(SendTo(wallet, PaymentAddress(chain, 0)))});
    QCOMPARE(PayContact(page, sources, 0x0b), at(1));
    const uint256 live{SendTo(wallet, PaymentAddress(chain, 1))};
    QCOMPARE(PayContact(page, sources, 0x0b), at(2));
    QVERIFY(wallet.abandonTransaction(live));
    QCOMPARE(PayContact(page, sources, 0x0b), at(1));

    // Replaced: a payment spending the same coin is mined.
    const auto create = [&](const CScript& to, const wallet::CCoinControl& coin_control) {
        int change_pos{-1};
        CAmount fee;
        return wallet.createTransaction({{to, COIN, false}}, coin_control, /*sign=*/true, change_pos, fee);
    };
    const auto replaced{create(GetScriptForDestination(PaymentAddress(chain, 1)), {})};
    QVERIFY(replaced);
    wallet::CCoinControl same_coin;
    same_coin.Select((*replaced)->vin.at(0).prevout);
    same_coin.m_allow_other_inputs = false;
    const auto replacing{create(miner, same_coin)};
    QVERIFY(replacing);
    QVERIFY(wallet.commitTransaction(*replaced, {}, {}));
    mine({CMutableTransaction{**replacing}});
    QCOMPARE(PayContact(page, sources, 0x0b), at(1));

    // A reorg takes counted payments out of their block: six of them, so
    // every address the receiver watches is taken and the first is used.
    std::vector<CMutableTransaction> six;
    for (uint32_t index = 1; index <= 6; ++index)
        six.push_back(tx(SendTo(wallet, PaymentAddress(chain, index))));
    const uint256 block{mine(six)};
    QCOMPARE(PayContact(page, sources, 0x0b), at(7));
    {
        BlockValidationState state;
        CBlockIndex* index{WITH_LOCK(::cs_main, return f.chain.m_node.chainman->m_blockman.LookupBlockIndex(block))};
        QVERIFY(f.chain.m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        SyncWithValidationInterfaceQueue();
    }
    QCOMPARE(PayContact(page, sources, 0x0b), at(1));

    const CAmount before{bob_wallet->getBalance()};
    const QString address{PayContact(page, sources, 0x0b)};
    mine({tx(SendTo(wallet, DecodeDestination(address.toStdString())))});
    QCOMPARE(bob_wallet->getBalance(), before + COIN);
}

namespace {
//! The identity 0x05… with the keys Dash Core registers.
platform::Result<platform::Identity> OurIdentity()
{
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    return ours;
}

//! A valid payment chain key for contacts' requests.
CPubKey ChainKey()
{
    CExtKey key;
    const std::array<std::byte, 32> seed{};
    key.SetSeed(seed);
    return key.Neuter().pubkey;
}

QString RecoveryReport(const PlatformPage& page)
{
    const auto* label{page.findChild<QLabel*>("dashPayRecoveryStatus")};
    return label->isVisibleTo(&page) ? label->text() : QString{};
}
} // namespace

//! On a restored wallet Find my identity is the first DashPay click: under
//! one passphrase it starts DashPay, looks the identity up by its MASTER key
//! at index 0, reads its usernames and the contact requests we sent, and
//! imports our receiving chain towards each recipient, answered or not, born
//! at our earliest request to them; a chain the wallet holds is born earlier
//! when a request is older. The wallet is locked again once that is done,
//! and the tab says what was found. A client rebuilt during the lookup keeps
//! the unlock, no other write starts meanwhile, and Find starts none while
//! another flow is busy.
void PlatformTests::findMyIdentityRestoresTheWallet()
{
    Fixture f{m_node, /*consented=*/false};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    const auto ours{OurIdentity()};
    platform::Identifier bob, carol, dave;
    bob.fill(0x0b);
    carol.fill(0x0c);
    dave.fill(0x0d);
    CKey bob_key, carol_key;
    bob_key.MakeNewKey(/*fCompressed=*/true);
    carol_key.MakeNewKey(/*fCompressed=*/true);
    // Carol's chain was imported here when we answered her; bob's contact
    // was made on another app on this seed, and dave, asked there, has not
    // answered but can pay us.
    QVERIFY(wallet.ensureFriendshipReceivingKeychain({0, uint256{ours.value.id}, uint256{carol}, 1'700'000'000}));
    const CTxDestination bob_first{OurChain(wallet, ours.value.id, bob).second};
    const CTxDestination carol_first{OurChain(wallet, ours.value.id, carol).second};
    const CTxDestination dave_first{OurChain(wallet, ours.value.id, dave).second};
    const auto received{Requests({IncomingRequest(wallet, 0x0b, 5, bob_key, 2, ChainKey()),
                                  IncomingRequest(wallet, 0x0c, 5, carol_key, 2, ChainKey())})};
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    int unlocks{0};
    UsernameFlow* during_dialog{nullptr};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        if (during_dialog) during_dialog->registerName("erin2026");
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"})};
        client.sent_requests = {Requests({Request(5, 0x0b, 1'650'000'000'000), Request(5, 0x0b, 1'600'000'000'500),
                                          Request(5, 0x0c, 1'650'000'000'000), Request(5, 0x0d, 1'650'000'000'000)})};
        client.received_requests = {received};
        client.resolved_names = {Resolved(std::nullopt)};
    };
    // When the chain paying `address` was born, as listdescriptors shows it.
    const auto birth = [&](const CTxDestination& address) {
        const auto owners{f.wallet->GetScriptPubKeyMans(GetScriptForDestination(address))};
        return owners.size() == 1 ? (*owners.begin())->GetTimeFirstKey() : int64_t{-1};
    };
    const size_t spkms{f.wallet->GetAllScriptPubKeyMans().size()};

    PlatformPage page{sources.get()};
    f.attach(page);
    QVERIFY(sources.made.empty());
    QPushButton* find{VisibleButton(page, "dashPayFindIdentity")};
    QVERIFY(find != nullptr);
    find->click();
    QCOMPARE(sources.made.size(), size_t{1});
    // The evonodes change during the lookup: the new client looks the identity up again.
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{2});
    // A free username found while the contacts are read is not registered.
    page.usernameFlow()->registerName("dave2026");
    sources.fake->deliverAll();
    QTRY_VERIFY(!page.contactFlow()->rescanning());
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{0});
    QVERIFY(!page.usernameFlow()->error().isEmpty());

    QCOMPARE(unlocks, 1);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(page.service()->consented());
    QVERIFY(page.recoveryFlow()->step() == RecoveryFlow::Step::IDLE);
    QVERIFY(page.findChild<QLabel*>("dashPayIdentity")->isVisibleTo(&page));
    QCOMPARE(page.findChild<QLabel*>("dashPayUsername")->text(), QString{"me2026"});
    QVERIFY(VisibleButton(page, "dashPayFindIdentity") == nullptr);
    QVERIFY(VisibleButton(page, "dashPayRetryRecovery") == nullptr);
    std::vector<std::pair<uint8_t, ContactFlow::State>> listed;
    for (const auto& contact : page.contactFlow()->contacts()) {
        listed.emplace_back(contact.id[0], contact.state);
    }
    QVERIFY(listed == (std::vector<std::pair<uint8_t, ContactFlow::State>>{{0x0b, ContactFlow::State::ESTABLISHED},
                                                                           {0x0c, ContactFlow::State::ESTABLISHED},
                                                                           {0x0d, ContactFlow::State::OUTGOING}}));
    QVERIFY(RecoveryReport(page).contains("The addresses 3 contact"));
    QVERIFY(RecoveryReport(page).contains("pay you at are in the wallet."));

    // Bob's and dave's payments to us are now the wallet's, from our
    // earliest request to each; carol's chain is born at hers.
    QVERIFY(wallet.isSpendable(bob_first));
    QVERIFY(wallet.isSpendable(dave_first));
    QCOMPARE(f.wallet->GetAllScriptPubKeyMans().size(), spkms + 2);
    QCOMPARE(birth(bob_first), int64_t{1'600'000'000});
    QCOMPARE(birth(dave_first), int64_t{1'650'000'000});
    QCOMPARE(birth(carol_first), int64_t{1'650'000'000});

    // Another flow started while the passphrase dialog was open: Find looks nothing up.
    during_dialog = page.usernameFlow();
    page.recoveryFlow()->find();
    QCOMPARE(unlocks, 2);
    QVERIFY(page.recoveryFlow()->step() == RecoveryFlow::Step::IDLE);
    QVERIFY(RecoveryReport(page).contains("Another DashPay request is on its way"));
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    sources.fake->deliverAll();
}

//! Find says what it could not do, and Try again retries it under the
//! passphrase, at once whatever wait a failure set. The identity's names and
//! our requests that could not be read are read again, and their chains
//! imported. A read of the identity's pending username that fails leaves it
//! unknown, not "none", so registering one is not offered; read again it is
//! shown waiting for its vote. A rescan the wallet was too busy for is shown
//! as not finished, with no claim that blocks were checked, and runs on Try
//! again and finds the payment. A top-up whose fee steps are all used says
//! when it is sent again, not that it has not reached Platform yet.
void PlatformTests::findReportsWhatItCouldNotDo()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    const auto ours{OurIdentity()};
    platform::Identifier bob;
    bob.fill(0x0b);
    const CTxDestination address{OurChain(wallet, ours.value.id, bob).second};
    int64_t paid_at;
    {
        const auto notifications{f.chain.m_node.chain->handleNotifications(f.wallet)};
        const auto payment{f.chain.CreateValidMempoolTransaction(f.chain.m_coinbase_txns[1], 0, 2, f.chain.coinbaseKey,
                                                                 GetScriptForDestination(address), 5 * COIN,
                                                                 /*submit=*/false)};
        paid_at = f.chain.CreateAndProcessBlock({payment}, GetScriptForRawPubKey(f.chain.coinbaseKey.GetPubKey())).GetBlockTime();
        SyncWithValidationInterfaceQueue();
    }
    const CAmount before{wallet.getBalance()};
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    int unlocks{0};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {ours, ours, ours};
        client.names = {Unavailable<platform::Names>(), Names({})};
        client.contests = {Unavailable<std::vector<platform::Contest>>(), Contests("alice")};
        client.sent_requests = {Unavailable<platform::ContactRequestPage>(),
                                Requests({Request(5, 0x0b, uint64_t(paid_at - 24 * 60 * 60) * 1000)}), Requests({})};
        client.received_requests = {Requests({})};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    const auto* header{page.findChild<QLabel*>("dashPayUsername")};
    QVERIFY(VisibleButton(page, "dashPayRetryRecovery") == nullptr);
    VisibleButton(page, "dashPayFindIdentity")->click();
    sources.fake->deliverAll();
    QCOMPARE(unlocks, 1);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(page.usernameFlow()->names() == UsernameFlow::Names::FAILED);
    QVERIFY(VisibleButton(page, "dashPayRegisterUsername") == nullptr);
    QVERIFY(RecoveryReport(page).contains("could not read the contact requests you sent"));
    QVERIFY(!wallet.isSpendable(address));

    // Again, while another rescan holds the wallet's reservation: the names
    // are none, but whether one waits for its vote could not be read.
    std::optional<wallet::WalletRescanReserver> other{*f.wallet};
    QVERIFY(other->reserve());
    VisibleButton(page, "dashPayRetryRecovery")->click();
    sources.fake->deliverAll();
    QTRY_VERIFY(!page.contactFlow()->rescanning());
    QCOMPARE(unlocks, 2);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(page.usernameFlow()->names() == UsernameFlow::Names::NONE);
    QVERIFY(page.recoveryFlow()->pending() == RecoveryFlow::Pending::FAILED);
    QVERIFY(VisibleButton(page, "dashPayRegisterUsername") == nullptr);
    QVERIFY(!header->isVisibleTo(&page));
    QVERIFY(RecoveryReport(page).contains("waiting for its masternode vote"));
    QVERIFY(wallet.isSpendable(address));
    QCOMPARE(wallet.getBalance(), before);
    QVERIFY(page.contactFlow()->rescanFailed());
    QVERIFY(RecoveryReport(page).contains("did not finish"));
    QVERIFY(page.contactFlow()->rescanNote().isEmpty());
    QVERIFY(!VisibleTexts(page).join(' ').contains("were checked"));
    QVERIFY(!RecoveryReport(page).contains("could not read the contact requests"));

    other.reset();
    VisibleButton(page, "dashPayRetryRecovery")->click();
    sources.fake->deliverAll();
    QTRY_VERIFY(!page.contactFlow()->rescanning() && !page.contactFlow()->rescanFailed());
    QCOMPARE(unlocks, 3);
    QCOMPARE(header->text(), QString{"alice (waiting for the masternode vote)"});
    QVERIFY(VisibleButton(page, "dashPayRegisterUsername") == nullptr);
    QCOMPARE(wallet.getBalance(), before + 5 * COIN);
    QVERIFY(!wallet.owedRescanFrom());
    QVERIFY(!RecoveryReport(page).contains("did not finish"));
    QVERIFY(RecoveryReport(page).contains("The addresses 1 contact"));
    QVERIFY(VisibleButton(page, "dashPayRetryRecovery") == nullptr);

    {
        FakeSources behind;
        behind.script = [&](FakePlatformClient& client) {
            client.identities_by_pubkey_hash = {ours};
            for (const uint16_t last : {14, 29, 44, 59, 74, 89, 100}) {
                client.top_ups.push_back(Funded<uint64_t>(platform::FundingOutcome::PLATFORM_BEHIND_CORE, last));
            }
        };
        PlatformService service{f.wallet_model, f.models.client, behind.get()};
        Confirmations confirmations;
        IdentityFlow funding{service, confirmations.get(), InstantProof};
        funding.lookUp();
        behind.fake->deliverAll();
        funding.topUp(IdentityFlow::MIN_TOP_UP);
        behind.fake->deliverAll();
        for (int chain_lock{0}; chain_lock < 6; ++chain_lock) {
            Q_EMIT f.models.client.chainLockChanged();
            behind.fake->deliverAll();
        }
        QVERIFY(funding.error().contains("every fee step for it is used"));
    }
    PlatformPage later{sources.get()};
    f.attach(later);
    sources.fake->deliverAll();
    const QString texts{VisibleTexts(later).join(' ')};
    QVERIFY(texts.contains("every fee step for it is used"));
    QVERIFY(!texts.contains("has not reached Dash Platform yet"));
}

//! Find lists a restored wallet's funding payments under its passphrase and
//! checks each with Platform before offering it: the unused registration lock
//! is what Create sends, with no new payment, one Platform has used is counted
//! and never offered, and one whose check failed is being checked until Try
//! again checks it. Finding no identity says which one was looked for.
void PlatformTests::findRediscoversFundingPayments()
{
    using LockRecord = platform::AssetLockState::State;
    FundedFixture f{m_node, /*consented=*/false};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    const uint256 registration{CommitLock(wallet, wallet::RegistrationFundingKey{0})};
    const uint256 used{CommitLock(wallet, wallet::TopupFundingKey{0})};
    const uint256 unread{CommitLock(wallet, wallet::TopupFundingKey{1})};
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    int unlocks{0};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    const size_t txs{wallet.getWalletTxs().size()};
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {NotFound(), NotFound()};
        client.asset_lock_states = {LockState(LockRecord::NOT_PRESENT), LockState(LockRecord::FULLY_CONSUMED),
                                    Unavailable<platform::AssetLockState>(), LockState(LockRecord::NOT_PRESENT)};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    VisibleButton(page, "dashPayFindIdentity")->click();
    sources.fake->deliverAll();
    QCOMPARE(unlocks, 1);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(page.service()->consented());
    IdentityFlow& flow{*page.identityFlow()};
    QVERIFY(flow.lookedUp() && !flow.identity());
    QCOMPARE(sources.fake->countCalls("assetLockState"), size_t{3});
    QVERIFY(flow.used({used, 0}));
    for (const auto& found : flow.found()) {
        QVERIFY(found.funding.txid != used);
    }
    QString report{RecoveryReport(page)};
    QVERIFY(report.contains("identity index 0"));
    QVERIFY(report.contains("Funding payments found in this wallet: 1 not used on Dash Platform yet, 1 already used on "
                            "Dash Platform, 1 being checked with Dash Platform."));
    QVERIFY(report.contains("Create identity sends the identity funding payment found in this wallet"));
    QVERIFY(VisibleButton(page, "dashPayCreateIdentity") != nullptr);

    VisibleButton(page, "dashPayRetryRecovery")->click();
    sources.fake->deliverAll();
    QCOMPARE(unlocks, 2);
    QCOMPARE(sources.fake->countCalls("assetLockState"), size_t{4});
    QCOMPARE(sources.fake->calls.back().argument, COutPoint(unread, 0).ToStringShort());
    report = RecoveryReport(page);
    QVERIFY(report.contains("Funding payments found in this wallet: 2 not used on Dash Platform yet, 1 already used on "
                            "Dash Platform."));
    QVERIFY(VisibleButton(page, "dashPayRetryRecovery") == nullptr);

    // Create sends the registration lock found, without a new payment or its confirmation.
    Answer(QMessageBox::Cancel);
    VisibleButton(page, "dashPayCreateIdentity")->click();
    QCOMPARE(unlocks, 3);
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(flow.funding() && flow.funding()->txid == registration);
}

namespace {
//! The dialog the Search button opened.
UsernameSearchDialog* SearchDialog(const PlatformPage& page) { return page.findChild<UsernameSearchDialog*>(); }

//! The native macOS style paints through an NSView, which the minimal
//! platform lacks (QTBUG-49686), so a shown widget crashes it. While a test
//! shows one, Fusion is the style, as SharedMnWalkthroughTests does.
struct FusionStyle {
    QString saved;
    FusionStyle()
    {
#if defined(Q_OS_MACOS)
        if (QApplication::platformName() == "minimal") {
            saved = QApplication::style()->objectName();
            if (QStyle* const fusion = QStyleFactory::create("Fusion"); fusion != nullptr) {
                QApplication::setStyle(fusion);
            }
        }
#endif
    }
    ~FusionStyle()
    {
        if (!saved.isEmpty()) QApplication::setStyle(saved);
    }
};

//! Click Search and return the dialog it opened.
UsernameSearchDialog* OpenSearch(const PlatformPage& page)
{
    VisibleButton(page, "dashPaySearchContacts")->click();
    UsernameSearchDialog* dialog{SearchDialog(page)};
    Assert(dialog && dialog->isVisible());
    return dialog;
}

//! Type `prefix` in the dialog and click Search.
void SearchFor(const UsernameSearchDialog& dialog, const QString& prefix)
{
    dialog.findChild<QLineEdit*>("dashPaySearchInput")->setText(prefix);
    dialog.findChild<QPushButton*>("dashPaySearch")->click();
}

QStringList SearchResults(const UsernameSearchDialog& dialog)
{
    QStringList labels;
    const auto* list{dialog.findChild<QListWidget*>("dashPaySearchResults")};
    for (int row = 0; row < list->count(); ++row) {
        labels << list->item(row)->text();
    }
    return labels;
}
} // namespace

//! The search dialog looks a prefix up when asked, not before it is two
//! characters long and not twice at once, and shows the usernames the SDK
//! returns without claiming they are all. Add sends the chosen one a contact
//! request through the contact flow and closes the dialog; the Search
//! button is offered only while no DashPay flow is busy.
void PlatformTests::searchFindsUsernamesToAdd()
{
    const FusionStyle style;
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    auto bob{Found(9, 0)};
    bob.value.public_keys = {Key(1, Purpose::ENCRYPTION, Level::MEDIUM), Key(3, Purpose::DECRYPTION, Level::MEDIUM)};
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {OurIdentity()};
        client.names = {Names({"me2026"})};
        client.sent_requests = {Requests({}), Requests({Request(5, 9, 1)})};
        client.received_requests = {Requests({})};
        client.search_results = {Names({"alice2026", "bob2026"})};
        client.resolved_names = {Resolved(9)};
        client.identities = {bob};
        client.sent_contact_requests = {Resolved(1)};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    QVERIFY(SearchDialog(page) == nullptr);
    UsernameSearchDialog* dialog{OpenSearch(page)};
    auto* add{dialog->findChild<QPushButton*>("dashPayAddFromSearch")};
    auto* results{dialog->findChild<QListWidget*>("dashPaySearchResults")};
    QVERIFY(!add->isEnabled());

    // Too short to search.
    SearchFor(*dialog, "b");
    QCOMPARE(sources.fake->countCalls("searchNames"), size_t{0});
    // Searched once, with the prefix as typed and a limit; asking again
    // before the answer starts nothing.
    SearchFor(*dialog, " bo ");
    SearchFor(*dialog, "bob");
    QCOMPARE(sources.fake->countCalls("searchNames"), size_t{1});
    QCOMPARE(sources.fake->calls.back().argument, std::string{"bo"});
    QVERIFY(sources.fake->search_limits.back() > 0);
    sources.fake->deliverAll();
    QCOMPARE(SearchResults(*dialog), (QStringList{"alice2026", "bob2026"}));
    QVERIFY(!add->isEnabled());

    // Add sends the chosen username's owner a request, and the Search button
    // is gone while it is on its way.
    results->setCurrentRow(1);
    QVERIFY(add->isEnabled());
    add->click();
    QVERIFY(!dialog->isVisible());
    QVERIFY(VisibleButton(page, "dashPaySearchContacts") == nullptr);
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{1});
    QCOMPARE(sources.fake->calls.back().argument, std::string{"bob2026"});
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->contact_inputs.size(), size_t{1});
    QVERIFY(sources.fake->contact_inputs.back().recipient == bob.value.id);
    QVERIFY(page.contactFlow()->error().isEmpty());
    QCOMPARE(page.contactFlow()->contacts().size(), size_t{1});
    QVERIFY(VisibleButton(page, "dashPaySearchContacts") != nullptr);
    QTRY_VERIFY(!page.contactFlow()->rescanning());
}

//! A search the client drops is asked again of the client that replaces it,
//! and a search that fails shows no usernames. Add waits while a flow is
//! busy, and needs only our requests read, as adding by username does.
void PlatformTests::searchSurvivesAClientRebuild()
{
    const FusionStyle style;
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    int clients{0};
    platform::Result<platform::Names> failed;
    failed.status.kind = platform::StatusKind::UNAVAILABLE;
    sources.script = [&](FakePlatformClient& client) {
        // The network is turned off and on again, which forgets the identity.
        if (clients++ != 1) {
            client.identities_by_pubkey_hash = {OurIdentity()};
            client.names = {Names({"me2026"})};
            client.sent_requests = {Requests({})};
            client.received_requests = {Unavailable<platform::ContactRequestPage>()};
        }
        if (clients == 2) client.search_results = {Names({"carol2026"})};
        if (clients == 3) {
            client.search_results = {Names({"carol2026", "carolyn"}), failed, Names({"carol2026"}), Names({"carol2026"})};
        }
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    QVERIFY(page.contactFlow()->sentRead() && !page.contactFlow()->swept());
    UsernameSearchDialog* dialog{OpenSearch(page)};
    auto* status{dialog->findChild<QLabel*>("dashPaySearchStatus")};
    auto* add{dialog->findChild<QPushButton*>("dashPayAddFromSearch")};

    SearchFor(*dialog, "car");
    QCOMPARE(sources.fake->countCalls("searchNames"), size_t{1});
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{2});
    QCOMPARE(sources.fake->countCalls("searchNames"), size_t{1});
    QCOMPARE(sources.fake->calls.back().argument, std::string{"car"});
    sources.fake->deliverAll();
    QCOMPARE(SearchResults(*dialog), (QStringList{"carol2026"}));

    // Turned off meanwhile, the search waits for the network.
    SearchFor(*dialog, "caro");
    f.setNetworkActive(false);
    f.setNetworkActive(true);
    QCOMPARE(sources.made.size(), size_t{3});
    QCOMPARE(sources.fake->countCalls("searchNames"), size_t{1});
    sources.fake->deliverAll();
    QCOMPARE(SearchResults(*dialog), (QStringList{"carol2026", "carolyn"}));

    // A failed search leaves no usernames.
    SearchFor(*dialog, "caroly");
    sources.fake->deliverAll();
    QVERIFY(SearchResults(*dialog).isEmpty());
    QVERIFY(status->isVisibleTo(dialog));

    // Nothing can be added while the contact flow is busy.
    SearchFor(*dialog, "caro");
    sources.fake->deliverAll();
    dialog->findChild<QListWidget*>("dashPaySearchResults")->setCurrentRow(0);
    QVERIFY(add->isEnabled());
    page.contactFlow()->add("dave2026");
    QVERIFY(page.contactFlow()->step() == ContactFlow::Step::RESOLVING);
    QVERIFY(!add->isEnabled());
    sources.fake->deliverAll();
    QVERIFY(add->isEnabled());

    // Closing the dialog does not end a search: the one dialog the page keeps
    // is the one the answer goes to, whenever it is opened again.
    SearchFor(*dialog, "caro");
    dialog->reject();
    VisibleButton(page, "dashPaySearchContacts")->click();
    QVERIFY(SearchDialog(page) == dialog);
    sources.fake->deliverAll();
    QCOMPARE(SearchResults(*dialog), (QStringList{"carol2026"}));
}
