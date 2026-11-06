// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/identityflow.h>

#include <crypto/common.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/overloaded.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>

#include <algorithm>
#include <cstring>
#include <ios>
#include <limits>
#include <utility>
#include <variant>

namespace {
using platform::IdentityPublicKey;
using Funding = IdentityFlow::Funding;
using FundingState = IdentityFlow::FundingState;

//! Every identity is index 0 until the UI offers more than one.
constexpr uint32_t IDENTITY_INDEX{0};
//! The one credit output of the asset locks this flow builds.
constexpr uint32_t CREDIT_OUTPUT{0};
//! The funding key index of the next top-up.
constexpr const char* NEXT_TOP_UP_RECORD{"dashpay/topup-index"};

//! The keys an identity registers, by id as the mobile wallets register
//! them: the SDK's four base keys, then the ENCRYPTION and DECRYPTION pair of
//! contact requests.
struct KeySpec {
    uint32_t id;
    IdentityPublicKey::Purpose purpose;
    IdentityPublicKey::SecurityLevel security_level;
};
constexpr KeySpec REGISTRATION_KEYS[]{
    {0, IdentityPublicKey::Purpose::AUTHENTICATION, IdentityPublicKey::SecurityLevel::MASTER},
    {1, IdentityPublicKey::Purpose::AUTHENTICATION, IdentityPublicKey::SecurityLevel::CRITICAL},
    {2, IdentityPublicKey::Purpose::AUTHENTICATION, IdentityPublicKey::SecurityLevel::HIGH},
    {3, IdentityPublicKey::Purpose::TRANSFER, IdentityPublicKey::SecurityLevel::CRITICAL},
    {4, IdentityPublicKey::Purpose::ENCRYPTION, IdentityPublicKey::SecurityLevel::MEDIUM},
    {5, IdentityPublicKey::Purpose::DECRYPTION, IdentityPublicKey::SecurityLevel::MEDIUM},
};

std::string RegistrationName() { return strprintf("identity/%u/", IDENTITY_INDEX); }
//! Each credit output has its own record, named after its key and outpoint:
//! a lock set aside, or kept beside an identity another lock registered,
//! leaves room for the next one, and two outputs paying one key keep apart.
std::string RecordName(const wallet::AssetLockFundingKey& key, const COutPoint& outpoint)
{
    const auto* top_up{std::get_if<wallet::TopupFundingKey>(&key)};
    const std::string name{strprintf("%s:%u", outpoint.hash.ToString(), outpoint.n)};
    return top_up ? strprintf("%stopup/%u/%s", IdentityFlow::FUNDING_PREFIX, top_up->funding_index, name)
                  : strprintf("%s%s%s", IdentityFlow::FUNDING_PREFIX, RegistrationName(), name);
}

std::string UsedRecord(const COutPoint& outpoint)
{
    return strprintf("%s%s:%u", IdentityFlow::USED_PREFIX, outpoint.hash.ToString(), outpoint.n);
}

std::vector<unsigned char> Encode(const Funding& funding)
{
    DataStream stream;
    stream << funding.txid << funding.amount << funding.top_up.has_value()
           << funding.top_up.value_or(platform::Identifier{}) << funding.credit_index
           << static_cast<uint8_t>(funding.state) << funding.last_fee_increase.has_value()
           << funding.last_fee_increase.value_or(0) << funding.last_attempt;
    return {UCharCast(stream.data()), UCharCast(stream.data() + stream.size())};
}

std::optional<Funding> Decode(const std::string& record, const std::vector<unsigned char>& value)
{
    Funding funding;
    funding.record = record;
    const std::string name{record.substr(std::strlen(IdentityFlow::FUNDING_PREFIX))};
    if (name.rfind("topup/", 0) == 0) {
        const std::string rest{name.substr(std::strlen("topup/"))};
        const auto index{ToIntegral<uint32_t>(rest.substr(0, rest.find('/')))};
        if (!index) return std::nullopt;
        funding.key = wallet::TopupFundingKey{*index};
    } else if (name.rfind(RegistrationName(), 0) == 0) {
        funding.key = wallet::RegistrationFundingKey{IDENTITY_INDEX};
    } else {
        return std::nullopt;
    }
    try {
        DataStream stream{MakeUCharSpan(value)};
        bool tops_up;
        platform::Identifier identity;
        uint8_t state;
        bool has_last;
        uint16_t last;
        stream >> funding.txid >> funding.amount >> tops_up >> identity >> funding.credit_index >> state >> has_last >>
            last >> funding.last_attempt;
        if (state > static_cast<uint8_t>(FundingState::SET_ASIDE)) return std::nullopt;
        funding.state = static_cast<FundingState>(state);
        if (tops_up) funding.top_up = identity;
        if (has_last) funding.last_fee_increase = last;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    return funding;
}

uint32_t NextTopUpIndex(interfaces::Wallet& wallet)
{
    const auto records{wallet.getPlatformData(NEXT_TOP_UP_RECORD)};
    const auto it{records.find(NEXT_TOP_UP_RECORD)};
    return it == records.end() || it->second.size() != 4 ? 0 : ReadLE32(it->second.data());
}

bool SaveNextTopUpIndex(interfaces::Wallet& wallet, uint32_t index)
{
    std::vector<unsigned char> next(4);
    WriteLE32(next.data(), index);
    return wallet.writePlatformData(NEXT_TOP_UP_RECORD, next);
}

bool SaveFunding(interfaces::Wallet& wallet, const Funding& funding)
{
    if (const auto* key{std::get_if<wallet::TopupFundingKey>(&funding.key)}) {
        if (!SaveNextTopUpIndex(wallet, key->funding_index + 1)) return false;
    }
    return wallet.writePlatformData(funding.record, Encode(funding));
}

//! The user_fee_increase a submission starts from: one above the highest
//! one a submission may have used, unless Platform has forgotten it.
uint16_t FeeFloor(const Funding& funding, int64_t now)
{
    if (!funding.last_fee_increase) return 0;
    if (now - funding.last_attempt >= std::chrono::seconds{IdentityFlow::FEE_INCREASE_MEMORY}.count()) return 0;
    return *funding.last_fee_increase == std::numeric_limits<uint16_t>::max() ? *funding.last_fee_increase
                                                                              : *funding.last_fee_increase + 1;
}

//! Every fee step a submission could take is used until Platform forgets
//! the last attempt.
bool Capped(const Funding& funding, int64_t now) { return FeeFloor(funding, now) > platform::MAX_FUNDING_FEE_INCREASE; }

std::pair<CTransactionRef, interfaces::WalletTxStatus> WalletTxOf(interfaces::Wallet& wallet, const uint256& txid)
{
    interfaces::WalletTxStatus status{};
    interfaces::WalletOrderForm order_form;
    bool in_mempool{false};
    int num_blocks{0};
    const CTransactionRef tx{wallet.getWalletTxDetails(txid, status, order_form, in_mempool, num_blocks).tx};
    return {tx, status};
}

std::optional<std::array<uint8_t, 20>> MasterKeyHash(interfaces::Wallet& wallet)
{
    const auto key{wallet.getPlatformPubKey(wallet::IdentityAuthKey{IDENTITY_INDEX, REGISTRATION_KEYS[0].id})};
    if (!key) return std::nullopt;
    const CKeyID hash{key.value.GetID()};
    std::array<uint8_t, 20> out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

std::optional<std::vector<IdentityPublicKey>> RegistrationKeys(interfaces::Wallet& wallet)
{
    std::vector<IdentityPublicKey> keys;
    for (const KeySpec& spec : REGISTRATION_KEYS) {
        const auto pubkey{wallet.getPlatformPubKey(wallet::IdentityAuthKey{IDENTITY_INDEX, spec.id})};
        if (!pubkey) return std::nullopt;
        IdentityPublicKey key;
        key.id = spec.id;
        key.purpose = spec.purpose;
        key.security_level = spec.security_level;
        key.data.assign(pubkey.value.begin(), pubkey.value.end());
        keys.push_back(std::move(key));
    }
    return keys;
}

std::optional<platform::ChainAssetLockProof> NodeChainProof(interfaces::Wallet& wallet, const CTransactionRef& tx)
{
    const auto status{WalletTxOf(wallet, tx->GetHash()).second};
    if (!status.is_chainlocked) return std::nullopt;
    return platform::ChainAssetLockProof{static_cast<uint32_t>(status.block_height),
                                         COutPoint{tx->GetHash(), CREDIT_OUTPUT}};
}

std::optional<platform::AssetLockProof> NodeProof(interfaces::Node& node, interfaces::Wallet& wallet,
                                                  const CTransactionRef& tx)
{
    auto islock{node.llmq().getInstantSendLock(tx->GetHash())};
    if (!islock.empty()) return platform::InstantAssetLockProof{tx, std::move(islock), CREDIT_OUTPUT};
    // The node drops the InstantSend lock once the block is ChainLocked.
    if (auto proof{NodeChainProof(wallet, tx)}) return *proof;
    return std::nullopt;
}

//! A proof names the credit output of the funding it is for.
void SetCreditOutput(platform::AssetLockProof& proof, uint32_t credit_index)
{
    std::visit(util::Overloaded{[&](platform::InstantAssetLockProof& p) { p.output_index = credit_index; },
                                [&](platform::ChainAssetLockProof& p) { p.out_point.n = credit_index; }},
               proof);
}

int64_t Now() { return GetTime<std::chrono::seconds>().count(); }

bool CanSignUnasked(const WalletModel& wallet_model)
{
    const auto status{wallet_model.getEncryptionStatus()};
    return status != WalletModel::Locked && status != WalletModel::UnlockedForMixingOnly;
}

//! Registration first, then top-ups by index: the order they are checked in.
bool FoundBefore(const IdentityFlow::Found& a, const IdentityFlow::Found& b)
{
    const auto index{[](const IdentityFlow::Found& found) -> int64_t {
        const auto* top_up{std::get_if<wallet::TopupFundingKey>(&found.funding.key)};
        return top_up ? int64_t{top_up->funding_index} : int64_t{-1};
    }};
    return std::make_pair(index(a), a.funding.outpoint()) < std::make_pair(index(b), b.funding.outpoint());
}
} // namespace

IdentityFlow::IdentityFlow(PlatformService& service, Confirm confirm, Proof proof, ChainProof chain_proof) :
    m_service(service),
    m_confirm(std::move(confirm)),
    m_proof(std::move(proof)),
    m_chain_proof(std::move(chain_proof))
{
    if (!m_proof) {
        m_proof = [&service](const CTransactionRef& tx) {
            return NodeProof(service.clientModel().node(), service.walletModel().wallet(), tx);
        };
    }
    if (!m_chain_proof) {
        m_chain_proof = [&service](const CTransactionRef& tx) {
            return NodeChainProof(service.walletModel().wallet(), tx);
        };
    }
    connect(&m_service, &PlatformService::changed, this, &IdentityFlow::serviceChanged);
    connect(&m_service.clientModel(), &ClientModel::instantSendChanged, this, [this] { send(); });
    connect(&m_service.clientModel(), &ClientModel::chainLockChanged, this, &IdentityFlow::chainLocked);
    serviceChanged();
}

std::vector<Funding> IdentityFlow::fundings() const
{
    std::vector<Funding> out;
    for (const auto& [record, value] : m_service.walletModel().wallet().getPlatformData(FUNDING_PREFIX)) {
        if (auto funding{Decode(record, value)}) out.push_back(std::move(*funding));
    }
    return out;
}

bool IdentityFlow::inactive(const Funding& funding) const
{
    const auto [tx, status]{WalletTxOf(m_service.walletModel().wallet(), funding.txid)};
    return tx && status.is_abandoned && status.depth_in_main_chain == 0;
}

std::vector<Funding> IdentityFlow::earlier() const
{
    if (!m_identity) return {};
    std::vector<Funding> out;
    for (Funding& funding : fundings()) {
        // A registration lock beside a different identity, one refused as a
        // registration included: it was never tried as a top-up.
        if (!funding.top_up && !inactive(funding)) out.push_back(std::move(funding));
    }
    for (const Found& found : m_found) {
        if (!found.inactive && found.state) out.push_back(found.funding);
    }
    return out;
}

std::optional<Funding> IdentityFlow::funding() const
{
    for (Funding& funding : fundings()) {
        if (funding.state == FundingState::SET_ASIDE) continue;
        // Kept beside an identity another lock registered.
        if (!funding.top_up && m_identity) continue;
        if (inactive(funding)) continue;
        return std::move(funding);
    }
    return std::nullopt;
}

template <typename T>
std::function<void(T)> IdentityFlow::tracked(std::function<void(T)> fn)
{
    auto call{std::make_shared<int>(0)};
    m_call = call;
    return [call, fn = std::move(fn)](T result) { fn(std::move(result)); };
}

void IdentityFlow::serviceChanged()
{
    // A call the client dropped never calls back; a funding record stays for Continue.
    if ((m_step == Step::LOOKING_UP || m_step == Step::SENDING || m_step == Step::CHECKING) && m_call.expired()) {
        finish();
    }
    if (m_service.gate() != PlatformService::Gate::NONE) {
        m_looked_up = false;
        m_identity.reset();
        if (m_step == Step::WAITING_FOR_LOCK) finish();
    }
    if (m_step == Step::WAITING_FOR_LOCK) {
        send();
    } else if (!m_looked_up) {
        startLookUp(/*unlock=*/false);
    } else {
        checkPending();
    }
    Q_EMIT changed();
}

bool IdentityFlow::discover()
{
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const uint32_t recorded{NextTopUpIndex(wallet)};
    std::vector<wallet::AssetLockFundingKey> keys{wallet::RegistrationFundingKey{IDENTITY_INDEX}};
    std::vector<wallet::AssetLockFunding> listed;
    int64_t highest{-1};
    for (uint32_t scanned{0};;) {
        const uint32_t end{static_cast<uint32_t>(std::max<int64_t>(recorded, highest + 1)) + TOP_UP_GAP};
        if (end <= scanned) break;
        for (; scanned < end; ++scanned)
            keys.emplace_back(wallet::TopupFundingKey{scanned});
        auto batch{wallet.listAssetLockFundings(keys)};
        if (!batch) return false;
        for (auto& lock : batch.value) {
            if (const auto* key{std::get_if<wallet::TopupFundingKey>(&lock.key)}) {
                highest = std::max<int64_t>(highest, key->funding_index);
            }
            listed.push_back(std::move(lock));
        }
        keys.clear();
    }
    if (highest >= recorded) SaveNextTopUpIndex(wallet, static_cast<uint32_t>(highest + 1));

    const auto records{fundings()};
    const auto used{wallet.getPlatformData(USED_PREFIX)};
    std::vector<Found> found;
    for (const auto& lock : listed) {
        // One a conflicting payment replaced can never reach Platform.
        if (lock.status == wallet::AssetLockFundingStatus::CONFLICTED) continue;
        const COutPoint outpoint{lock.txid, lock.credit_index};
        if (used.count(UsedRecord(outpoint))) continue;
        if (std::any_of(records.begin(), records.end(), [&](const Funding& r) { return r.outpoint() == outpoint; })) {
            continue;
        }
        Found entry;
        entry.funding.key = lock.key;
        entry.funding.txid = lock.txid;
        entry.funding.credit_index = lock.credit_index;
        entry.funding.amount = lock.amount;
        entry.inactive = lock.status == wallet::AssetLockFundingStatus::INACTIVE;
        // A read already made stands.
        for (const Found& known : m_found) {
            if (known.funding.outpoint() != outpoint) continue;
            entry.state = known.state;
            entry.read_failed = known.read_failed;
        }
        found.push_back(std::move(entry));
    }
    std::sort(found.begin(), found.end(), FoundBefore);
    m_found = std::move(found);
    return true;
}

void IdentityFlow::markUsed(const COutPoint& outpoint)
{
    if (!m_service.walletModel().wallet().writePlatformData(UsedRecord(outpoint), {1})) {
        LogPrintf("DashPay: could not mark the funding payment %s used\n", outpoint.ToStringShort());
    }
    m_found.erase(std::remove_if(m_found.begin(), m_found.end(),
                                 [&](const Found& found) { return found.funding.outpoint() == outpoint; }),
                  m_found.end());
}

void IdentityFlow::readFound(const COutPoint& outpoint)
{
    platform::PlatformClient* client{m_service.client()};
    if (m_step != Step::IDLE || !client) return;
    m_step = Step::CHECKING;
    Q_EMIT changed();
    client->assetLockState(outpoint, tracked<platform::Result<platform::AssetLockState>>(
                                         [this, outpoint](platform::Result<platform::AssetLockState> result) {
                                             m_step = Step::IDLE;
                                             for (Found& found : m_found) {
                                                 if (found.funding.outpoint() != outpoint) continue;
                                                 found.read_failed = !result.ok();
                                                 if (result.ok()) found.state = result.value.state;
                                             }
                                             if (result.ok() && result.value.state ==
                                                                    platform::AssetLockState::State::FULLY_CONSUMED) {
                                                 markUsed(outpoint);
                                             }
                                             Q_EMIT changed();
                                             checkPending(/*retry=*/false);
                                         }));
}

void IdentityFlow::chainLocked()
{
    if (m_step == Step::WAITING_FOR_LOCK) return send(/*chain_locked=*/true);
    checkPending();
}

void IdentityFlow::checkPending(bool retry)
{
    if (m_step != Step::IDLE || !m_service.client()) return;
    const auto pending{funding()};
    // One with every fee step used is read again on Continue only.
    if (pending && (pending->state == FundingState::CONFIRMING || pending->state == FundingState::CHECK_AGAIN) &&
        !Capped(*pending, Now())) {
        return check(*pending);
    }
    for (const Found& found : m_found) {
        if (found.inactive || found.state || (found.read_failed && !retry)) continue;
        return readFound(found.funding.outpoint());
    }
}

void IdentityFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_funding.reset();
    m_tx.reset();
    m_unlock.reset();
    m_applying = false;
    m_error = error;
    Q_EMIT changed();
}

void IdentityFlow::lookUp()
{
    m_note.clear();
    startLookUp(/*unlock=*/true);
}

void IdentityFlow::startLookUp(bool unlock)
{
    if (m_step != Step::IDLE || !m_service.client()) return;
    {
        std::optional<WalletModel::UnlockContext> context;
        if (!m_key_hash && unlock) context.emplace(m_service.walletModel().requestUnlock());
        if (!m_key_hash) m_key_hash = MasterKeyHash(m_service.walletModel().wallet());
        if (CanSignUnasked(m_service.walletModel())) discover();
    }
    if (!m_key_hash) {
        if (unlock) finish(tr("The wallet stayed locked, so DashPay could not look up your identity."));
        return;
    }
    // The passphrase dialog runs an event loop, which may have started a call.
    platform::PlatformClient* client{m_service.client()};
    if (m_step != Step::IDLE || !client) return;
    m_step = Step::LOOKING_UP;
    m_error.clear();
    client->getIdentityByPublicKeyHash(*m_key_hash, tracked<platform::Result<platform::Identity>>(
                                                        [this](auto result) { onLookedUp(std::move(result)); }));
    Q_EMIT changed();
}

void IdentityFlow::onLookedUp(platform::Result<platform::Identity> result)
{
    if (!result.ok() && result.status.kind != platform::StatusKind::NOT_FOUND) {
        return finish(PlatformUi::Describe(result.status));
    }
    m_looked_up = true;
    m_identity.reset();
    if (result.ok()) {
        m_identity = std::move(result.value);
        // A registration lock is used by the identity it registers, and by no
        // other: one beside a different identity is kept.
        for (const Funding& funding : fundings()) {
            if (!funding.top_up && platform::IdentityIdFromAssetLock(funding.outpoint()) == m_identity->id) {
                retire(funding);
            }
        }
        std::vector<COutPoint> registered;
        for (const Found& found : m_found) {
            if (std::holds_alternative<wallet::RegistrationFundingKey>(found.funding.key) &&
                platform::IdentityIdFromAssetLock(found.funding.outpoint()) == m_identity->id) {
                registered.push_back(found.funding.outpoint());
            }
        }
        for (const COutPoint& outpoint : registered)
            markUsed(outpoint);
    }
    finish();
    checkPending();
}

void IdentityFlow::create()
{
    if (!m_looked_up || m_identity) return;
    Funding pending;
    pending.key = wallet::RegistrationFundingKey{IDENTITY_INDEX};
    fund(REGISTRATION_AMOUNT, std::move(pending));
}

void IdentityFlow::topUp(CAmount amount)
{
    if (!m_identity) return;
    const uint32_t index{NextTopUpIndex(m_service.walletModel().wallet())};
    Funding pending;
    pending.key = wallet::TopupFundingKey{index};
    pending.top_up = m_identity->id;
    fund(amount, std::move(pending));
}

std::optional<WalletModel::UnlockContext> IdentityFlow::beginFunding()
{
    // Keeps other calls out while the passphrase and confirmation dialogs run their event loops.
    m_step = Step::FUNDING;
    m_error.clear();
    m_note.clear();
    Q_EMIT changed();
    std::optional<WalletModel::UnlockContext> unlock{m_service.walletModel().requestUnlock()};
    if (!unlock->isValid()) {
        finish(tr("The wallet stayed locked, so nothing was sent."));
        return std::nullopt;
    }
    if (!m_key_hash) m_key_hash = MasterKeyHash(m_service.walletModel().wallet());
    return unlock;
}

bool IdentityFlow::stillCurrent(const Funding& pending) const
{
    if (m_step != Step::FUNDING || m_service.gate() != PlatformService::Gate::NONE || !m_service.client()) {
        return false;
    }
    if (pending.top_up) return m_identity && m_identity->id == *pending.top_up;
    return !m_identity;
}

void IdentityFlow::fund(CAmount amount, Funding pending)
{
    if (m_step != Step::IDLE || !m_service.client() || funding()) return;
    const bool registration{!pending.top_up};
    auto unlock{beginFunding()};
    if (!unlock) return;
    // A dialog's event loop may have closed a gate or found the identity.
    if (!stillCurrent(pending) || (registration && !m_looked_up)) {
        return finish(tr("DashPay stopped while the wallet was being unlocked, so nothing was sent."));
    }
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    discover();
    if (registration) {
        // A registration lock already paid is sent again, never a second one.
        const auto found{std::find_if(m_found.begin(), m_found.end(), [](const Found& found) {
            return std::holds_alternative<wallet::RegistrationFundingKey>(found.funding.key) && !found.inactive;
        })};
        if (found != m_found.end() && found->state) return adopt(found->funding, std::move(*unlock));
        if (found != m_found.end()) {
            // Checked first: one Platform has used is never sent.
            const COutPoint outpoint{found->funding.outpoint()};
            platform::PlatformClient* client{m_service.client()};
            m_unlock.emplace(std::move(*unlock));
            m_step = Step::CHECKING;
            Q_EMIT changed();
            return client->assetLockState(
                outpoint, tracked<platform::Result<platform::AssetLockState>>(
                              [this, outpoint](platform::Result<platform::AssetLockState> result) {
                                  const auto it{std::find_if(m_found.begin(), m_found.end(), [&](const Found& found) {
                                      return found.funding.outpoint() == outpoint;
                                  })};
                                  if (it == m_found.end()) return finish();
                                  if (!result.ok()) {
                                      it->read_failed = true;
                                      return finish(tr("DashPay could not check the identity funding payment "
                                                       "already in this wallet with Dash Platform, so nothing was "
                                                       "sent. Try again."));
                                  }
                                  if (result.value.state == platform::AssetLockState::State::FULLY_CONSUMED) {
                                      markUsed(outpoint);
                                      return finish(tr("The identity funding payment already in this wallet was "
                                                       "used on Dash Platform. Create again to fund a new one."));
                                  }
                                  it->state = result.value.state;
                                  WalletModel::UnlockContext held{std::move(*m_unlock)};
                                  m_unlock.reset();
                                  adopt(it->funding, std::move(held));
                              }));
        }
    } else {
        // No new lock while one this wallet paid can still be used: it is
        // applied first. One whose read failed, or that was abandoned, does
        // not hold a top-up back.
        const bool unread{std::any_of(m_found.begin(), m_found.end(), [](const Found& found) {
            return !found.inactive && !found.state && !found.read_failed;
        })};
        if (unread || !earlier().empty()) {
            finish(tr("This wallet holds an earlier funding payment for Dash Platform. Apply it before topping up."));
            // One found just now is read, so it can be applied.
            return checkPending(/*retry=*/false);
        }
        pending.key = wallet::TopupFundingKey{NextTopUpIndex(wallet)};
    }
    CAmount fee{0};
    const auto tx{wallet.createAssetLockTransaction(amount, pending.key, wallet::CCoinControl{}, fee)};
    if (!tx) return finish(QString::fromStdString(util::ErrorString(tx).translated));
    if (!m_confirm(amount, fee)) return finish();
    if (!stillCurrent(pending) || (registration && (!m_looked_up || funding()))) {
        return finish(tr("DashPay stopped while the payment was being confirmed, so nothing was sent."));
    }

    pending.txid = (*tx)->GetHash();
    pending.credit_index = CREDIT_OUTPUT;
    pending.amount = amount;
    pending.record = RecordName(pending.key, pending.outpoint());
    // Before the asset lock is sent: the record is all that resumes it.
    if (!SaveFunding(wallet, pending)) {
        return finish(tr("The wallet could not save the funding payment, so nothing was sent."));
    }
    if (const auto res{wallet.commitTransaction(*tx, {}, {})}; !res) {
        // Abandoning releases its coins; one it can't abandon may still confirm.
        if (wallet.abandonTransaction(pending.txid)) wallet.writePlatformData(pending.record, {});
        return finish(tr("The network did not accept the funding payment: %1")
                          .arg(QString::fromStdString(util::ErrorString(res).translated)));
    }
    awaitLock(std::move(pending), *tx, std::move(*unlock));
}

void IdentityFlow::resume()
{
    auto pending{funding()};
    if (m_step != Step::IDLE || !m_service.client() || !pending) return;
    m_note.clear();
    if (pending->state == FundingState::CONFIRMING || pending->state == FundingState::CHECK_AGAIN) {
        return check(*pending);
    }
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const auto [tx, status]{WalletTxOf(wallet, pending->txid)};
    if (!tx || status.depth_in_main_chain < 0) {
        // Replaced by a payment spending the same coins, or never sent:
        // Platform can never get it.
        wallet.writePlatformData(pending->record, {});
        return finish(tr("The funding payment was replaced by another payment or never sent, so it bought no "
                         "credits."));
    }
    // Only Platform's record can settle it before a fee step is free again.
    if (Capped(*pending, Now())) return check(*pending);
    auto unlock{beginFunding()};
    if (!unlock) return;
    if (!stillCurrent(*pending)) {
        return finish(tr("DashPay stopped while the wallet was being unlocked, so nothing was sent."));
    }
    awaitLock(std::move(*pending), tx, std::move(*unlock));
}

void IdentityFlow::adopt(Funding funding, WalletModel::UnlockContext unlock)
{
    funding.record = RecordName(funding.key, funding.outpoint());
    funding.state = FundingState::PENDING;
    const COutPoint outpoint{funding.outpoint()};
    m_found.erase(std::remove_if(m_found.begin(), m_found.end(),
                                 [&](const Found& found) { return found.funding.outpoint() == outpoint; }),
                  m_found.end());
    if (!save(funding)) return finish(tr("The wallet could not save the funding payment, so nothing was sent."));
    const auto [tx, status]{WalletTxOf(m_service.walletModel().wallet(), funding.txid)};
    if (!tx) return finish();
    awaitLock(std::move(funding), tx, std::move(unlock));
}

void IdentityFlow::applyEarlier()
{
    if (m_step != Step::IDLE || !m_service.client() || funding() || !m_identity || earlier().empty()) return;
    const platform::Identifier identity{m_identity->id};
    auto unlock{beginFunding()};
    if (!unlock) return;
    if (m_service.gate() != PlatformService::Gate::NONE || !m_service.client() || !m_identity ||
        m_identity->id != identity) {
        return finish(tr("DashPay stopped while the wallet was being unlocked, so nothing was sent."));
    }
    // Every one is recorded first, so each is kept whatever happens to the others.
    for (Funding funding : earlier()) {
        funding.top_up = identity;
        funding.state = FundingState::PENDING;
        if (funding.record.empty()) funding.record = RecordName(funding.key, funding.outpoint());
        if (!save(funding)) return finish(tr("The wallet could not save the funding payment, so nothing was sent."));
        const COutPoint outpoint{funding.outpoint()};
        m_found.erase(std::remove_if(m_found.begin(), m_found.end(),
                                     [&](const Found& found) { return found.funding.outpoint() == outpoint; }),
                      m_found.end());
    }
    m_unlock.emplace(std::move(*unlock));
    m_applying = true;
    if (!applyNext()) finish();
}

bool IdentityFlow::applyNext()
{
    auto next{funding()};
    if (!next || !next->top_up || next->state != FundingState::PENDING) return false;
    const auto [tx, status]{WalletTxOf(m_service.walletModel().wallet(), next->txid)};
    if (!tx || status.depth_in_main_chain < 0) return false;
    if (Capped(*next, Now())) {
        finish(cappedNote(*next));
        return true;
    }
    m_funding = std::move(*next);
    m_tx = tx;
    m_step = Step::WAITING_FOR_LOCK;
    Q_EMIT changed();
    send();
    return true;
}

void IdentityFlow::awaitLock(Funding pending, CTransactionRef tx, WalletModel::UnlockContext unlock)
{
    m_funding = std::move(pending);
    m_tx = std::move(tx);
    m_unlock.emplace(std::move(unlock));
    m_step = Step::WAITING_FOR_LOCK;
    // The record stays; the wallet is not kept unlocked for a closed gate.
    if (m_service.gate() != PlatformService::Gate::NONE) return finish();
    Q_EMIT changed();
    send();
}

bool IdentityFlow::save(const Funding& funding)
{
    return m_service.walletModel().wallet().writePlatformData(funding.record, Encode(funding));
}

void IdentityFlow::retire(const Funding& funding)
{
    markUsed(funding.outpoint());
    if (!m_service.walletModel().wallet().writePlatformData(funding.record, {})) {
        LogPrintf("DashPay: could not erase the funding record %s\n", funding.record);
    }
}

void IdentityFlow::send(bool chain_locked)
{
    if (m_step != Step::WAITING_FOR_LOCK) return;
    // Platform behind Core is tried again once a newer block is ChainLocked.
    if (m_funding->state == FundingState::PLATFORM_BEHIND && !chain_locked) return;
    std::optional<platform::AssetLockProof> proof;
    if (m_funding->state == FundingState::NEEDS_CHAINLOCK) {
        if (auto chain_proof{m_chain_proof(m_tx)}) proof = *chain_proof;
    } else {
        proof = m_proof(m_tx);
    }
    platform::PlatformClient* client{m_service.client()};
    if (!proof || !client) return;
    SetCreditOutput(*proof, m_funding->credit_index);

    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    std::optional<std::vector<IdentityPublicKey>> keys;
    if (!m_funding->top_up) {
        keys = RegistrationKeys(wallet);
        if (!keys) return finish(tr("Dash Core could not derive your identity's keys from this wallet."));
    }
    const int64_t now{Now()};
    const uint16_t floor{FeeFloor(*m_funding, now)};
    if (floor > platform::MAX_FUNDING_FEE_INCREASE) return finish(cappedNote(*m_funding));
    // Any attempt the call builds may reach Platform, even if its answer
    // never comes: the next call starts above every one it could build.
    const int64_t previous_attempt{m_funding->last_attempt};
    m_funding->last_fee_increase = static_cast<uint16_t>(
        std::min<int>(floor + platform::FUNDING_ATTEMPTS - 1, platform::MAX_FUNDING_FEE_INCREASE));
    m_funding->last_attempt = now;
    if (!save(*m_funding)) return finish(tr("The wallet could not save the funding payment, so nothing was sent."));

    // The flow keeps the wallet unlocked until Platform has answered.
    platform::Signing signing{wallet, IDENTITY_INDEX, nullptr};
    m_step = Step::SENDING;
    if (m_funding->top_up) {
        client->topUpIdentity(*m_funding->top_up, *proof, m_funding->key, floor, std::move(signing),
                              m_service.trackFunding<uint64_t>(tracked<platform::FundingResult<uint64_t>>(
                                  [this, funding = *m_funding, floor,
                                   previous_attempt](platform::FundingResult<uint64_t> result) {
                                      if (result.funding.outcome == platform::FundingOutcome::OK && m_identity &&
                                          m_identity->id == *funding.top_up) {
                                          m_identity->balance = result.value;
                                      }
                                      sent(funding, floor, previous_attempt, result.status, result.funding);
                                  })));
    } else {
        client->registerIdentity(
            std::move(*keys), *proof, floor, std::move(signing),
            m_service.trackFunding<platform::Identity>(tracked<platform::FundingResult<platform::Identity>>(
                [this, funding = *m_funding, floor, previous_attempt](platform::FundingResult<platform::Identity> result) {
                    if (result.funding.outcome == platform::FundingOutcome::OK) {
                        m_identity = std::move(result.value);
                        m_looked_up = true;
                    }
                    sent(funding, floor, previous_attempt, result.status, result.funding);
                })));
    }
    Q_EMIT changed();
}

void IdentityFlow::sent(Funding funding, uint16_t floor, int64_t previous_attempt, const platform::Status& status,
                        const platform::FundingReport& report)
{
    using Outcome = platform::FundingOutcome;
    // The report replaces the reservation, never below the floor. A call
    // that built nothing was no attempt.
    funding.last_fee_increase = std::max(floor, report.last_user_fee_increase);
    if (report.last_user_fee_increase < floor) funding.last_attempt = previous_attempt;
    const char* what{funding.top_up ? "identity top-up" : "identity registration"};
    switch (report.outcome) {
    case Outcome::OK:
        retire(funding);
        if (m_applying && applyNext()) return;
        return finish();
    case Outcome::NEEDS_CHAINLOCK_PROOF:
    case Outcome::PLATFORM_BEHIND_CORE:
        // Wait as for the first lock, still unlocked, and send it again.
        funding.state = report.outcome == Outcome::NEEDS_CHAINLOCK_PROOF ? FundingState::NEEDS_CHAINLOCK
                                                                         : FundingState::PLATFORM_BEHIND;
        if (!save(funding)) return finish(tr("The wallet could not save the funding payment's state."));
        LogPrintf("DashPay: %s waits: %s\n", what, status.message);
        // No submission is left before Platform forgets the last attempt.
        if (Capped(funding, Now())) return finish(cappedNote(funding));
        m_funding = std::move(funding);
        m_step = Step::WAITING_FOR_LOCK;
        m_error.clear();
        Q_EMIT changed();
        return send();
    case Outcome::SUBMITTED:
        funding.state = FundingState::CONFIRMING;
        save(funding);
        return finish();
    case Outcome::REPORTED_CONSUMED:
        funding.state = FundingState::CHECK_AGAIN;
        save(funding);
        return finish();
    case Outcome::REJECTED:
        funding.state = FundingState::SET_ASIDE;
        save(funding);
        LogPrintf("DashPay: %s refused: %s\n", what, status.message);
        return finish(
            tr("Dash Platform refused the funding payment, so it is set aside: %1").arg(PlatformUi::Describe(status)));
    case Outcome::UNAVAILABLE:
        break;
    }
    // Nothing settled: a transition that was sent may still execute. One
    // Platform needs a ChainLock proof for still does.
    if (funding.state != FundingState::NEEDS_CHAINLOCK) funding.state = FundingState::PENDING;
    save(funding);
    LogPrintf("DashPay: %s failed: %s\n", what, status.message);
    finish(PlatformUi::Describe(status));
}

void IdentityFlow::check(Funding funding)
{
    platform::PlatformClient* client{m_service.client()};
    if (m_step != Step::IDLE || !client) return;
    m_step = Step::CHECKING;
    m_error.clear();
    Q_EMIT changed();
    if (funding.top_up) return readLockState(std::move(funding));
    // A registration is proved used by the identity its lock registers.
    const auto derived{platform::IdentityIdFromAssetLock(funding.outpoint())};
    if (!derived) return unsettled(funding);
    client->getIdentity(*derived, tracked<platform::Result<platform::Identity>>(
                                      [this, funding](platform::Result<platform::Identity> result) {
                                          if (result.ok()) {
                                              m_identity = std::move(result.value);
                                              m_looked_up = true;
                                              retire(funding);
                                              return finish();
                                          }
                                          if (result.status.kind != platform::StatusKind::NOT_FOUND) {
                                              return unsettled(funding);
                                          }
                                          readLockState(funding);
                                      }));
}

void IdentityFlow::readLockState(Funding funding)
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return unsettled(funding);
    client->assetLockState(
        funding.outpoint(),
        tracked<platform::Result<platform::AssetLockState>>([this, funding](platform::Result<platform::AssetLockState> result) {
            // A failed read checks again later.
            if (!result.ok()) return unsettled(funding);
            if (result.value.state != platform::AssetLockState::State::FULLY_CONSUMED) return sendAgain(funding);
            // Used up, by a transition the record does not name.
            retire(funding);
            const BitcoinUnit unit{m_service.walletModel().getOptionsModel()->getDisplayUnit()};
            const QString amount{BitcoinUnits::formatWithUnit(unit, funding.amount)};
            m_note = funding.top_up ? tr("The funding payment of %1 was used on Dash Platform.").arg(amount)
                                    : tr("Funding payment used on Platform; no derived identity found (%1).").arg(amount);
            finish();
            if (funding.top_up) {
                // Its balance is read again.
                m_looked_up = false;
                startLookUp(/*unlock=*/false);
            }
        }));
}

void IdentityFlow::sendAgain(Funding funding)
{
    if (Capped(funding, Now())) return finish(cappedNote(funding));
    funding.state = FundingState::PENDING;
    save(funding);
    const auto [tx, status]{WalletTxOf(m_service.walletModel().wallet(), funding.txid)};
    const auto encryption{m_service.walletModel().getEncryptionStatus()};
    // Only where no passphrase is asked for: Continue asks for one.
    if (!tx || encryption == WalletModel::Locked || encryption == WalletModel::UnlockedForMixingOnly) {
        return finish(tr("Dash Platform has not used the funding payment. Continue sends it again."));
    }
    awaitLock(std::move(funding), tx, m_service.walletModel().requestUnlock());
}

void IdentityFlow::unsettled(const Funding& funding)
{
    finish(Capped(funding, Now()) ? cappedNote(funding) : QString{});
}

QString IdentityFlow::cappedNote(const Funding& funding) const
{
    const BitcoinUnit unit{m_service.walletModel().getOptionsModel()->getDisplayUnit()};
    return tr("Dash Platform has not reached the block of the funding payment of %1, and every fee step for it is "
              "used. Continue sends it again after %2.")
        .arg(BitcoinUnits::formatWithUnit(unit, funding.amount),
             GUIUtil::dateTimeStr(funding.last_attempt + std::chrono::seconds{FEE_INCREASE_MEMORY}.count()));
}
