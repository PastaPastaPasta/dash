// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformservice.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <platform/statetransitions.h>
#include <qt/clientmodel.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/walletmodel.h>
#include <util/strencodings.h>
#include <util/strencodings.h>

#include <QMetaObject>
#include <QPointer>
#include <QVector>

#include <utility>

namespace {
//! Flow advance / retry cadence.
constexpr int TICK_INTERVAL_MS{5'000};
//! Endpoint + quorum key refresh cadence.
constexpr int CONTEXT_INTERVAL_MS{60'000};
} // namespace

PlatformService::PlatformService(WalletModel& wallet_model, ClientModel& client_model,
                                 std::shared_ptr<platform::PlatformClient> client, QObject* parent) :
    QObject(parent),
    m_wallet_model(wallet_model),
    m_client_model(client_model),
    m_client(std::move(client))
{
    if (auto params{platform::GetParams(Params().NetworkIDString())}) {
        m_params = *params;
    }

    m_identity_flow = std::make_unique<IdentityFlow>(*this, this);
    connect(m_identity_flow.get(), &IdentityFlow::stateChanged, this, &PlatformService::identityStateChanged);
    connect(m_identity_flow.get(), &IdentityFlow::failed, this, &PlatformService::flowFailed);

    m_contact_flow = std::make_unique<ContactFlow>(*this, this);

    m_tick_timer = new QTimer(this);
    m_tick_timer->setInterval(TICK_INTERVAL_MS);
    connect(m_tick_timer, &QTimer::timeout, this, [this] { m_identity_flow->advance(); });
    m_tick_timer->start();

    m_context_timer = new QTimer(this);
    m_context_timer->setInterval(CONTEXT_INTERVAL_MS);
    connect(m_context_timer, &QTimer::timeout, this, &PlatformService::updateNodeContext);
    m_context_timer->start();
    updateNodeContext();

    m_identity_flow->resume();
}

PlatformService::~PlatformService()
{
    if (m_client) m_client->shutdown();
}

QString PlatformService::myUsername() const
{
    const auto& rec{m_identity_flow->record()};
    if (rec.state == IdentityFlow::State::REGISTERED) {
        return QString::fromStdString(rec.label);
    }
    return {};
}

std::optional<platform::Identifier> PlatformService::myIdentityId() const
{
    const auto& rec{m_identity_flow->record()};
    if (rec.state >= IdentityFlow::State::IDENTITY_CONFIRMED &&
        rec.state != IdentityFlow::State::FAILED) {
        return rec.identity_id;
    }
    return std::nullopt;
}

void PlatformService::checkNameAvailability(const QString& name)
{
    const std::string normalized{platform::st::NormalizeLabel(name.toStdString())};
    const bool contested{platform::st::IsContestedLabel(normalized)};
    QPointer<PlatformService> self{this};
    m_client->resolveName(normalized, [self, normalized, contested](platform::Result<std::optional<platform::DpnsName>> res) {
        if (!self) return;
        self->post([self, normalized, contested, res = std::move(res)] {
            if (!self || !res.ok()) return;
            const bool available{!res.value->has_value()};
            Q_EMIT self->nameAvailability(QString::fromStdString(normalized), available, contested);
        });
    });
}

void PlatformService::searchNames(const QString& prefix)
{
    const std::string normalized{platform::st::NormalizeLabel(prefix.toStdString())};
    QPointer<PlatformService> self{this};
    m_client->searchNames(normalized, /*limit=*/25, [self, prefix](platform::Result<std::vector<platform::DpnsName>> res) {
        if (!self) return;
        self->post([self, prefix, res = std::move(res)] {
            if (!self || !res.ok()) return;
            QVector<QPair<QString, QString>> out;
            out.reserve(res.value->size());
            for (const auto& name : *res.value) {
                out.append({QString::fromStdString(name.label),
                            QString::fromStdString(HexStr(name.identity))});
            }
            Q_EMIT self->searchResults(prefix, out);
        });
    });
}

void PlatformService::loadProfile(const platform::Identifier& identity)
{
    QPointer<PlatformService> self{this};
    m_client->getProfile(identity, [self, identity](platform::Result<std::optional<platform::Profile>> res) {
        if (!self) return;
        self->post([self, identity, res = std::move(res)] {
            if (!self || !res.ok() || !res.value->has_value()) return;
            const auto& profile{**res.value};
            Q_EMIT self->profileLoaded(QString::fromStdString(HexStr(identity)),
                                       QString::fromStdString(profile.display_name),
                                       QString::fromStdString(profile.public_message),
                                       QString::fromStdString(profile.avatar_url));
        });
    });
}

bool PlatformService::writeRecord(const std::string& key, const std::vector<unsigned char>& value)
{
    return m_wallet_model.wallet().writePlatformData(key, value);
}

std::vector<unsigned char> PlatformService::readRecord(const std::string& key) const
{
    auto records{m_wallet_model.wallet().getPlatformData(key)};
    const auto it{records.find(key)};
    return it != records.end() ? it->second : std::vector<unsigned char>{};
}

void PlatformService::post(std::function<void()> fn)
{
    QMetaObject::invokeMethod(this, [fn = std::move(fn)] { fn(); }, Qt::QueuedConnection);
}

void PlatformService::refreshContacts()
{
    const auto my_id{myIdentityId()};
    if (!my_id) return;
    QPointer<PlatformService> self{this};
    auto collect = [self](bool to_me) {
        if (!self) return;
        self->m_client->getContactRequests(*self->myIdentityId(), to_me, 0,
            [self, to_me](platform::Result<std::vector<platform::ContactRequest>> res) {
            if (!self || !res.ok()) return;
            QVector<QPair<QString, QString>> list;
            for (const auto& cr : *res.value) {
                const auto& other = to_me ? cr.owner_id : cr.to_user_id;
                list.append({QString::fromStdString(HexStr(other)), QString{}});
            }
            self->post([self, to_me, list] {
                if (!self) return;
                if (to_me) Q_EMIT self->contactsUpdated(list, {});
                else Q_EMIT self->contactsUpdated({}, list);
            });
        });
    };
    collect(true);
    collect(false);
}

void PlatformService::updateProfile(const QString& display_name, const QString& public_message,
                                    const QString& avatar_url)
{
    const auto my_id{myIdentityId()};
    if (!my_id) { Q_EMIT profileUpdated(false, tr("register a username first")); return; }

    platform::Profile profile;
    profile.owner_id = *my_id;
    profile.display_name = display_name.toStdString();
    profile.public_message = public_message.toStdString();
    profile.avatar_url = avatar_url.toStdString();

    QPointer<PlatformService> self{this};
    m_client->getIdentityContractNonce(*my_id, platform::DASHPAY_CONTRACT_ID,
        [self, profile](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->post([self, profile, nonce_res] {
            if (!self) return;
            if (!nonce_res.ok()) { Q_EMIT self->profileUpdated(false, tr("could not fetch identity nonce")); return; }
            interfaces::Wallet& wallet{self->m_wallet_model.wallet()};
            const auto signer = [&wallet](const uint256& digest, std::vector<uint8_t>& sig) {
                return wallet.signPlatformDigest(interfaces::Wallet::PlatformKeyType::IdentityAuth, 0, 1, digest, sig);
            };
            auto built{platform::st::BuildProfile(profile.owner_id, *nonce_res.value + 1, profile,
                                                  /*revision=*/1, std::nullopt, /*signature_public_key_id=*/1, signer)};
            if (!built.ok()) { Q_EMIT self->profileUpdated(false, QString::fromStdString(built.error)); return; }
            QPointer<PlatformService> inner{self};
            self->m_client->broadcastStateTransition(built.value->bytes,
                [inner](platform::Result<platform::BroadcastResult> res) {
                if (!inner) return;
                inner->post([inner, res] {
                    if (!inner) return;
                    const bool ok = res.ok() && res.value->accepted;
                    Q_EMIT inner->profileUpdated(ok, ok ? QString{} : (res.ok() ? QString::fromStdString(res.value->error) : tr("broadcast failed")));
                });
            });
        });
    });
}

void PlatformService::updateNodeContext()
{
    interfaces::Node& node{m_client_model.node()};

    // Evonode DAPI endpoints from the deterministic masternode list.
    std::vector<platform::Endpoint> endpoints;
    const auto [mn_list, tip] = node.evo().getListAtChainTip();
    if (mn_list) {
        mn_list->forEachMN(/*only_valid=*/true, [&endpoints](const auto& dmn) {
            for (const auto& service : dmn->getPlatformHTTPSAddrs()) {
                endpoints.push_back(platform::Endpoint{service, dmn->getProTxHash()});
            }
        });
    }
    if (!endpoints.empty()) {
        m_client->updateEndpoints(std::move(endpoints));
    }

    // Platform-signing quorum public keys for proof verification.
    const auto llmq_type{static_cast<uint8_t>(Params().GetConsensus().llmqTypePlatform)};
    auto quorums{node.llmq().getPlatformQuorums(llmq_type)};
    if (!quorums.empty()) {
        std::vector<platform::QuorumKey> keys;
        keys.reserve(quorums.size());
        for (auto& q : quorums) {
            keys.push_back(platform::QuorumKey{q.m_quorum_hash, std::move(q.m_pubkey), q.m_height});
        }
        m_client->updateQuorumKeys(llmq_type, std::move(keys));
    }
}
