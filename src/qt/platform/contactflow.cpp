// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactflow.h>

#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <pubkey.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <support/cleanse.h>
#include <uint256.h>
#include <util/time.h>
#include <wallet/platformtypes.h>

#include <QMetaObject>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <tuple>
#include <utility>

namespace {
using platform::IdentityPublicKey;
using Purpose = IdentityPublicKey::Purpose;

//! The DIP-15 account of every receiving chain, as the mobile wallets send it.
constexpr uint32_t ACCOUNT{0};

const IdentityPublicKey* FindKey(const platform::Identity& identity, uint32_t id)
{
    const auto it{std::find_if(identity.public_keys.begin(), identity.public_keys.end(),
                               [id](const IdentityPublicKey& key) { return key.id == id; })};
    return it == identity.public_keys.end() ? nullptr : &*it;
}

bool Enabled(const IdentityPublicKey& key, Purpose purpose)
{
    return key.purpose == purpose && key.type == IdentityPublicKey::Type::ECDSA_SECP256K1 && key.disabled_at == 0;
}

std::optional<uint32_t> FirstEnabledKey(const platform::Identity& identity, Purpose purpose)
{
    for (const IdentityPublicKey& key : identity.public_keys) {
        if (Enabled(key, purpose)) return key.id;
    }
    return std::nullopt;
}

//! The accountReference over our receiving chain towards `theirs`, which is
//! imported first so their payments are seen. No payment can predate it, so
//! it is born now.
std::optional<uint32_t> ImportedAccountReference(interfaces::Wallet& wallet, const platform::Identifier& ours,
                                                 const platform::Identifier& theirs, uint32_t encryption_key)
{
    const auto chain{wallet.ensureFriendshipReceivingKeychain(
        {ACCOUNT, uint256{ours}, uint256{theirs}, TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())})};
    wallet::CompactXpub compact;
    if (!chain || !wallet::CompactXpubBytes(chain.value, compact)) return std::nullopt;
    const auto reference{wallet.platformAccountReference({IdentityFlow::IDENTITY_INDEX, encryption_key}, compact)};
    if (!reference) return std::nullopt;
    return reference.value;
}

//! The newest request per counterparty: the latest, then the higher
//! accountReference, as the mobile wallets choose it.
std::map<platform::Identifier, platform::ContactRequest> Newest(std::vector<platform::ContactRequest> requests,
                                                                platform::Identifier platform::ContactRequest::* key)
{
    const auto rank = [](const platform::ContactRequest& request) {
        return std::tie(request.created_at_ms, request.account_reference);
    };
    std::map<platform::Identifier, platform::ContactRequest> newest;
    for (auto& request : requests) {
        const auto [it, added]{newest.try_emplace(request.*key, std::move(request))};
        if (!added && rank(request) > rank(it->second)) it->second = std::move(request);
    }
    return newest;
}

QString NoSuchUser() { return ContactFlow::tr("No DashPay user has that username."); }

//! The payment chain an incoming request carries, decrypted with our key it
//! names. nullopt unless their key and ours are keys DIP-15 accepts for its
//! ECDH and it decrypts to a valid point. The wallet must be unlocked.
std::optional<wallet::FriendshipXpub> PeerXpub(interfaces::Wallet& wallet, const platform::Identity& ours,
                                               const platform::Identity& theirs, const platform::ContactRequest& request)
{
    // The keys DIP-15 accepts on receive, as rs-platform-wallet validates them.
    const IdentityPublicKey* their_key{FindKey(theirs, request.sender_key_index)};
    if (!their_key || !(Enabled(*their_key, Purpose::ENCRYPTION) || Enabled(*their_key, Purpose::AUTHENTICATION))) {
        return std::nullopt;
    }
    const IdentityPublicKey* our_key{FindKey(ours, request.recipient_key_index)};
    if (!our_key || our_key->disabled_at != 0 ||
        (our_key->purpose != Purpose::DECRYPTION && our_key->purpose != Purpose::ENCRYPTION &&
         our_key->purpose != Purpose::AUTHENTICATION && our_key->purpose != Purpose::TRANSFER) ||
        (our_key->type != IdentityPublicKey::Type::ECDSA_SECP256K1 &&
         our_key->type != IdentityPublicKey::Type::ECDSA_HASH160)) {
        return std::nullopt;
    }
    const auto secret{wallet.platformECDHSecret({IdentityFlow::IDENTITY_INDEX, request.recipient_key_index},
                                                CPubKey{their_key->data})};
    std::array<uint8_t, 32> shared;
    if (!secret || secret.value.size() != shared.size()) return std::nullopt;
    std::copy(secret.value.begin(), secret.value.end(), shared.begin());
    auto xpub{platform::DecryptContactXpub(shared, request.encrypted_public_key)};
    memory_cleanse(shared.data(), shared.size());
    if (!xpub || !xpub->pubkey.IsFullyValid() || !xpub->pubkey.IsCompressed()) return std::nullopt;
    return xpub;
}
} // namespace

ContactFlow::ContactFlow(PlatformService& service, IdentityFlow& identity_flow) :
    m_service(service),
    m_identity_flow(identity_flow)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &ContactFlow::update);
    connect(&m_identity_flow, &IdentityFlow::changed, this, &ContactFlow::update);
    update();
}

ContactFlow::Requests ContactFlow::sent() const { return Newest(m_sent.read, &platform::ContactRequest::to_user_id); }

ContactFlow::Requests ContactFlow::received() const
{
    return Newest(m_received.read, &platform::ContactRequest::owner);
}

QString ContactFlow::error() const
{
    if (!m_error.isEmpty()) return m_error;
    return m_sent.error.isEmpty() ? m_received.error : m_sent.error;
}

std::vector<ContactFlow::Contact> ContactFlow::contacts() const
{
    const Requests sent{this->sent()};
    const Requests received{this->received()};
    std::vector<std::pair<const platform::ContactRequest*, State>> rows;
    for (const auto& [id, request] : received) {
        rows.emplace_back(&request, sent.count(id) ? State::ESTABLISHED : State::INCOMING);
    }
    for (const auto& [id, request] : sent) {
        if (!received.count(id)) rows.emplace_back(&request, State::OUTGOING);
    }
    // By state; the newest incoming first, as only they are listed.
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second < b.second;
        return a.second == State::INCOMING && a.first->created_at_ms > b.first->created_at_ms;
    });
    std::vector<Contact> contacts;
    size_t incoming{0};
    for (const auto& [request, state] : rows) {
        if (state == State::INCOMING && ++incoming > INCOMING_SHOWN) continue;
        const platform::Identifier& id{state == State::OUTGOING ? request->to_user_id : request->owner};
        const auto name{m_names.find(id)};
        contacts.push_back({id, state, name == m_names.end() ? std::nullopt : name->second});
    }
    return contacts;
}

size_t ContactFlow::moreIncoming() const
{
    const Requests sent{this->sent()};
    size_t incoming{0};
    for (const auto& [id, request] : received())
        incoming += !sent.count(id);
    return incoming > INCOMING_SHOWN ? incoming - INCOMING_SHOWN : 0;
}

void ContactFlow::update()
{
    // The identity is forgotten when a gate closes; its contacts go with it.
    if (!m_identity_flow.identity()) {
        m_sent = {};
        m_received = {};
        m_names.clear();
        m_name_calls.clear();
    }
    // A call the client dropped never calls back.
    if (m_call.expired()) {
        if (m_step == Step::RESOLVING) {
            resolve();
        } else if (m_step == Step::FETCHING) {
            fetch();
        } else if (m_step == Step::SENDING) {
            // Only a closed gate drops a write, and the contacts went with
            // the identity: our requests, read again, say whether it landed.
            m_step = Step::IDLE;
        }
    }
    sweep();
    readNames();
    Q_EMIT changed();
}

void ContactFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_error = error;
    sweep();
    Q_EMIT changed();
}

void ContactFlow::refresh()
{
    if (m_step != Step::IDLE || !m_sent.call.expired() || !m_received.call.expired()) return;
    // From where each direction stopped: newer requests come after it.
    for (Direction* direction : {&m_sent, &m_received}) {
        direction->complete = false;
        direction->error.clear();
    }
    std::erase_if(m_names, [](const auto& name) { return !name.second; });
    m_error.clear();
    sweep();
    Q_EMIT changed();
}

void ContactFlow::sweep()
{
    readRequests(platform::ContactDirection::SENT);
    readRequests(platform::ContactDirection::RECEIVED);
}

void ContactFlow::readRequests(platform::ContactDirection direction, bool next)
{
    Direction& read{direction == platform::ContactDirection::SENT ? m_sent : m_received};
    const auto& identity{m_identity_flow.identity()};
    platform::PlatformClient* client{m_service.client()};
    if (!identity || !client || read.complete || !read.error.isEmpty() || (!next && !read.call.expired())) return;
    client->contactRequests(
        identity->id, direction, read.next_since_ms,
        PlatformUi::Tracked<platform::ContactRequestPage>(read.call, [this, direction, &read](auto result) {
            if (result.ok()) {
                read.read.insert(read.read.end(), std::make_move_iterator(result.value.requests.begin()),
                                 std::make_move_iterator(result.value.requests.end()));
                read.next_since_ms = result.value.next_since_ms;
                read.complete = result.value.complete;
                if (!read.complete) readRequests(direction, /*next=*/true);
            } else {
                // What was read stays; the binding's message says why the rest is not.
                read.error = result.status.message.empty() ? PlatformUi::Describe(result.status)
                                                           : QString::fromStdString(result.status.message);
            }
            readNames();
            Q_EMIT changed();
        }));
}

void ContactFlow::readNames()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return;
    for (const Contact& contact : contacts()) {
        auto& call{m_name_calls[contact.id]};
        if (m_names.count(contact.id) || !call.expired()) continue;
        client->namesOfIdentity(contact.id, /*page_limit=*/1, /*max_pages=*/1,
                                PlatformUi::Tracked<platform::Names>(call, [this, id = contact.id](auto result) {
                                    auto& name{m_names[id]};
                                    if (result.ok() && !result.value.names.empty()) {
                                        name = result.value.names.front().label;
                                    }
                                    namesChanged();
                                }));
    }
}

void ContactFlow::namesChanged()
{
    if (std::exchange(m_names_changed, true)) return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_names_changed = false;
            Q_EMIT changed();
        },
        Qt::QueuedConnection);
}

void ContactFlow::add(const std::string& username)
{
    if (m_step != Step::IDLE || !sentRead() || !m_service.client()) return;
    m_label = username;
    if (!platform::IsValidUsername(m_label)) return finish(NoSuchUser());
    m_step = Step::RESOLVING;
    m_error.clear();
    resolve();
    Q_EMIT changed();
}

void ContactFlow::resolve()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return finish();
    client->resolveName(m_label, PlatformUi::Tracked<platform::Identifier>(m_call, [this](auto result) {
                            onResolved(std::move(result));
                        }));
}

void ContactFlow::onResolved(platform::Result<platform::Identifier> result)
{
    const QString label{QString::fromStdString(m_label)};
    if (result.status.kind == platform::StatusKind::NOT_FOUND) return finish(NoSuchUser());
    if (!result.ok()) return finish(PlatformUi::Describe(result.status));
    const auto& identity{m_identity_flow.identity()};
    if (identity && result.value == identity->id) return finish(tr("“%1” is your own username.").arg(label));
    if (sent().count(result.value)) {
        return finish(tr("You have already sent “%1” a contact request.").arg(label));
    }
    m_contact = result.value;
    m_step = Step::FETCHING;
    fetch();
    Q_EMIT changed();
}

void ContactFlow::accept(const platform::Identifier& contact)
{
    // Their request read, and ours known not to be sent.
    if (m_step != Step::IDLE || !sentRead() || !received().count(contact) || sent().count(contact)) return;
    m_contact = contact;
    m_step = Step::FETCHING;
    m_error.clear();
    fetch();
    Q_EMIT changed();
}

void ContactFlow::fetch()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return finish();
    client->getIdentity(m_contact, PlatformUi::Tracked<platform::Identity>(m_call, [this](auto result) {
                            onFetched(std::move(result));
                        }));
}

void ContactFlow::onFetched(platform::Result<platform::Identity> result)
{
    if (result.status.kind == platform::StatusKind::NOT_FOUND) {
        return finish(tr("This DashPay user no longer exists."));
    }
    if (!result.ok()) return finish(PlatformUi::Describe(result.status));
    if (!m_identity_flow.identity()) return finish();
    const platform::Identity ours{*m_identity_flow.identity()};
    const platform::Identity& theirs{result.value};
    // Theirs: an enabled DECRYPTION key, else ENCRYPTION. Ours: ENCRYPTION.
    auto recipient_key{FirstEnabledKey(theirs, Purpose::DECRYPTION)};
    if (!recipient_key) recipient_key = FirstEnabledKey(theirs, Purpose::ENCRYPTION);
    if (!recipient_key) return finish(tr("This DashPay user can't receive contact requests."));
    const auto encryption_key{FirstEnabledKey(ours, Purpose::ENCRYPTION)};
    const auto signing_key{PlatformUi::DocumentSigningKey(ours)};
    if (!encryption_key || !signing_key) {
        return finish(tr("Your identity has no keys DashPay can send a contact request with."));
    }

    m_step = Step::SENDING;
    Q_EMIT changed();
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) return finish(tr("The wallet stayed locked, so no contact request was sent."));
    // The passphrase dialog may have let another flow start, or closed a gate.
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay request is on its way. Try again once it is done."));
    }
    platform::PlatformClient* client{m_service.client()};
    if (!client || !m_identity_flow.identity() || !sentRead()) return finish();

    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    // Answering their request: only one carrying a chain we can pay.
    const Requests received{this->received()};
    if (const auto incoming{received.find(theirs.id)};
        incoming != received.end() && !PeerXpub(wallet, ours, theirs, incoming->second)) {
        return finish(tr("This person's contact request to you carries no valid payment key, so DashPay can't add "
                         "them."));
    }
    const auto reference{ImportedAccountReference(wallet, ours.id, theirs.id, *encryption_key)};
    if (!reference) return finish(tr("Dash Core could not derive the contact request's keys from this wallet."));

    platform::ContactRequestInput input;
    input.sender = ours.id;
    input.recipient = theirs.id;
    input.sender_key_index = *encryption_key;
    input.recipient_key_index = *recipient_key;
    input.signing_key_id = *signing_key;
    input.account_reference = *reference;
    client->sendContactRequest(input, {wallet, IdentityFlow::IDENTITY_INDEX, PlatformUi::KeepUnlocked(std::move(unlock))},
                               m_service.trackWrite<platform::Identifier>(PlatformUi::Tracked<platform::Identifier>(
                                   m_call, [this](auto result) { onSent(result.status); })));
}

void ContactFlow::onSent(const platform::Status& status)
{
    if (status.kind != platform::StatusKind::OK) LogPrintf("DashPay: contact request failed: %s\n", status.message);
    // Sent or not, our requests read on from where they stopped say.
    m_sent.complete = false;
    m_sent.error.clear();
    finish(PlatformUi::Describe(status));
}
