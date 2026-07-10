// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/identityflow.h>

#include <clientversion.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <platform/statetransitions.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <random.h>
#include <script/script.h>
#include <streams.h>
#include <util/time.h>
#include <wallet/coincontrol.h>

#include <QPointer>

using interfaces::Wallet;
using PlatformKeyType = interfaces::Wallet::PlatformKeyType;

namespace {

constexpr char RECORD_KEY[]{"identity/0"};
//! Confirmation polls before assuming a broadcast was lost and stepping back
//! to rebuild + rebroadcast.
constexpr int MAX_CONFIRM_POLLS{6};
//! Index of the identity-funding credit output within the asset lock
//! payload's creditOutputs. We always create a single credit output, so
//! this is 0 — matching js-dash-sdk ASSET_LOCK_OUTPUT_INDEX. Note this is
//! NOT the OP_RETURN vout in the transaction's output list.
constexpr uint32_t ASSET_LOCK_OUTPUT_INDEX{0};

std::vector<unsigned char> SerializeRecord(const IdentityFlow::Record& r)
{
    CDataStream s(SER_DISK, CLIENT_VERSION);
    s << r.version << static_cast<uint8_t>(r.state) << r.funding_txid << r.funding_vout
      << r.funding_key_index << r.funding_amount
      << std::vector<uint8_t>(r.identity_id.begin(), r.identity_id.end())
      << r.label << r.normalized_label
      << std::vector<uint8_t>(r.preorder_salt.begin(), r.preorder_salt.end())
      << r.contested << r.last_error << r.started_at;
    const auto span = MakeUCharSpan(s);
    return {span.begin(), span.end()};
}

bool DeserializeRecord(const std::vector<unsigned char>& data, IdentityFlow::Record& r)
{
    try {
        CDataStream s(data, SER_DISK, CLIENT_VERSION);
        uint8_t state{0};
        std::vector<uint8_t> identity_id, salt;
        s >> r.version;
        if (r.version == 0 || r.version > IdentityFlow::Record::CURRENT_VERSION) return false;
        s >> state >> r.funding_txid >> r.funding_vout >> r.funding_key_index >> r.funding_amount >>
            identity_id >> r.label >> r.normalized_label >> salt >> r.contested >> r.last_error >>
            r.started_at;
        if (identity_id.size() != r.identity_id.size() || salt.size() != r.preorder_salt.size()) return false;
        std::copy(identity_id.begin(), identity_id.end(), r.identity_id.begin());
        std::copy(salt.begin(), salt.end(), r.preorder_salt.begin());
        r.state = static_cast<IdentityFlow::State>(state);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

//! 36-byte outpoint (txid || LE index) referencing the asset lock burn
//! output, the preimage of the identity id.
std::array<uint8_t, 36> BurnOutpoint(const uint256& txid, uint32_t vout)
{
    std::array<uint8_t, 36> out{};
    std::copy(txid.begin(), txid.end(), out.begin());
    out[32] = static_cast<uint8_t>(vout);
    out[33] = static_cast<uint8_t>(vout >> 8);
    out[34] = static_cast<uint8_t>(vout >> 16);
    out[35] = static_cast<uint8_t>(vout >> 24);
    return out;
}

} // namespace

IdentityFlow::IdentityFlow(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
    load();
}

bool IdentityFlow::load()
{
    const auto data{m_service.readRecord(RECORD_KEY)};
    if (data.empty()) return false;
    Record record;
    if (!DeserializeRecord(data, record)) return false;
    m_record = std::move(record);
    return true;
}

void IdentityFlow::save()
{
    m_service.writeRecord(RECORD_KEY, SerializeRecord(m_record));
}

void IdentityFlow::setState(State state)
{
    if (m_record.state == state) return;
    m_record.state = state;
    m_record.last_error.clear();
    m_retries = 0;
    save();
    LogPrintf("Platform identity flow: state=%d name=%s\n", static_cast<int>(state),
              m_record.normalized_label);
    Q_EMIT stateChanged();
}

void IdentityFlow::fail(const QString& step, const QString& error, bool retryable)
{
    m_record.last_error = error.toStdString();
    if (!retryable || ++m_retries > 10) {
        m_record.state = State::FAILED;
        m_retries = 0;
    }
    save();
    Q_EMIT failed(step, error);
    if (m_record.state == State::FAILED) Q_EMIT stateChanged();
}

void IdentityFlow::reset()
{
    if (m_record.state != State::FAILED) return;
    m_record = Record{};
    m_service.writeRecord(RECORD_KEY, {});
    Q_EMIT stateChanged();
}

bool IdentityFlow::start(const QString& label, CAmount funding_amount, QString& error)
{
    if (active()) {
        error = tr("a username registration is already in progress");
        return false;
    }

    const std::string label_str{label.toStdString()};
    const std::string normalized{platform::st::NormalizeLabel(label_str)};
    if (normalized.empty()) {
        error = tr("invalid username");
        return false;
    }

    Wallet& wallet{m_service.walletModel().wallet()};

    CPubKey funding_pubkey;
    if (!wallet.getPlatformPubKey(PlatformKeyType::RegistrationFunding, 0, 0, funding_pubkey)) {
        error = tr("unable to derive the funding key (is the wallet unlocked?)");
        return false;
    }

    auto res{wallet.createAssetLockTransaction(funding_amount, funding_pubkey, wallet::CCoinControl{})};
    if (!res) {
        error = QString::fromStdString(util::ErrorString(res).translated);
        return false;
    }
    const CTransactionRef& tx{*res};

    // Locate the OP_RETURN burn output.
    std::optional<uint32_t> burn_vout;
    for (uint32_t i = 0; i < tx->vout.size(); ++i) {
        const CScript& spk{tx->vout[i].scriptPubKey};
        if (!spk.empty() && spk[0] == OP_RETURN) {
            burn_vout = i;
            break;
        }
    }
    if (!burn_vout) {
        error = tr("internal error: asset lock transaction has no burn output");
        return false;
    }

    // Persist the full flow state — including the preorder salt — before the
    // irreversible broadcast.
    m_record = Record{};
    m_record.state = State::FUNDING_SENT;
    m_record.funding_txid = tx->GetHash();
    m_record.funding_vout = *burn_vout;
    m_record.funding_key_index = 0;
    m_record.funding_amount = funding_amount;
    m_record.label = label_str;
    m_record.normalized_label = normalized;
    m_record.contested = platform::st::IsContestedLabel(normalized);
    m_record.started_at = GetTime();
    GetStrongRandBytes(m_record.preorder_salt);
    save();

    wallet.commitTransaction(tx, {}, {});

    Q_EMIT stateChanged();
    return true;
}

void IdentityFlow::resume()
{
    if (!active()) return;
    // Every step handler below re-derives its progress from ground truth
    // (wallet tx status, proved platform queries), so resuming is just
    // advancing.
    advance();
}

void IdentityFlow::advance()
{
    if (m_step_in_flight) return;
    switch (m_record.state) {
    case State::NONE:
    case State::REGISTERED:
    case State::FAILED:
        return;
    case State::FUNDING_SENT:
        checkFundingLock();
        return;
    case State::FUNDING_LOCKED:
        broadcastIdentityCreate();
        return;
    case State::IDENTITY_BROADCAST:
        confirmIdentity();
        return;
    case State::IDENTITY_CONFIRMED:
        broadcastPreorder();
        return;
    case State::PREORDER_BROADCAST:
        broadcastPreorder();
        return;
    case State::PREORDER_WAIT:
        broadcastDomain();
        return;
    case State::DOMAIN_BROADCAST:
    case State::CONTESTED_PENDING:
        confirmDomain();
        return;
    }
}

void IdentityFlow::checkFundingLock()
{
    Wallet& wallet{m_service.walletModel().wallet()};
    interfaces::WalletTxStatus status;
    int num_blocks{0};
    int64_t block_time{0};
    if (!wallet.tryGetTxStatus(m_record.funding_txid, status, num_blocks, block_time)) return;

    if (status.is_abandoned || (status.depth_in_main_chain < 0)) {
        fail(tr("funding"), tr("the funding transaction was abandoned or conflicted"), /*retryable=*/false);
        return;
    }
    if (status.is_islocked || status.is_chainlocked || status.depth_in_main_chain >= 8) {
        setState(State::FUNDING_LOCKED);
    }
}

void IdentityFlow::broadcastIdentityCreate()
{
    Wallet& wallet{m_service.walletModel().wallet()};
    interfaces::Node& node{m_service.clientModel().node()};

    // Build the asset lock proof: prefer the InstantSend lock, fall back to
    // a chain-locked block proof. The identity is funded by the (single)
    // credit output in the asset lock payload, at ASSET_LOCK_OUTPUT_INDEX.
    const auto burn_outpoint{BurnOutpoint(m_record.funding_txid, ASSET_LOCK_OUTPUT_INDEX)};
    std::variant<platform::st::InstantAssetLockProof, platform::st::ChainAssetLockProof> proof;

    auto islock{node.llmq().getInstantSendLock(m_record.funding_txid)};
    if (!islock.empty()) {
        const auto wtx{wallet.getWalletTx(m_record.funding_txid)};
        if (!wtx.tx) {
            fail(tr("identity"), tr("funding transaction not found in wallet"), /*retryable=*/true);
            return;
        }
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << *wtx.tx;
        proof = platform::st::InstantAssetLockProof{
            .transaction = {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()},
            .instant_lock = std::move(islock),
            .output_index = ASSET_LOCK_OUTPUT_INDEX,
        };
    } else {
        interfaces::WalletTxStatus status;
        int num_blocks{0};
        int64_t block_time{0};
        if (!wallet.tryGetTxStatus(m_record.funding_txid, status, num_blocks, block_time) ||
            !status.is_chainlocked) {
            return; // wait for a lock
        }
        platform::st::ChainAssetLockProof chain_proof;
        chain_proof.core_chain_locked_height = static_cast<uint32_t>(status.block_height);
        chain_proof.out_point = burn_outpoint;
        proof = chain_proof;
    }

    // Identity keys 0 (MASTER) and 1 (HIGH), DIP-13 authentication keys.
    std::vector<platform::st::NewIdentityKey> keys;
    for (uint32_t key_index : {0u, 1u}) {
        CPubKey pubkey;
        if (!wallet.getPlatformPubKey(PlatformKeyType::IdentityAuth, 0, key_index, pubkey)) {
            fail(tr("identity"), tr("unable to derive identity keys (is the wallet unlocked?)"), /*retryable=*/true);
            return;
        }
        platform::st::NewIdentityKey key;
        key.id = key_index;
        key.purpose = platform::IdentityPublicKey::Purpose::AUTHENTICATION;
        key.security_level = key_index == 0 ? platform::IdentityPublicKey::SecurityLevel::MASTER
                                            : platform::IdentityPublicKey::SecurityLevel::HIGH;
        key.pubkey.assign(pubkey.begin(), pubkey.end());
        key.signer = [&wallet, key_index](const uint256& digest, std::vector<uint8_t>& sig) {
            return wallet.signPlatformDigest(PlatformKeyType::IdentityAuth, 0, key_index, digest, sig);
        };
        keys.push_back(std::move(key));
    }

    const auto asset_lock_signer = [&wallet, index = m_record.funding_key_index](const uint256& digest, std::vector<uint8_t>& sig) {
        return wallet.signPlatformDigest(PlatformKeyType::RegistrationFunding, 0, index, digest, sig);
    };

    auto built{platform::st::BuildIdentityCreate(proof, keys, asset_lock_signer)};
    if (!built.ok()) {
        fail(tr("identity"), QString::fromStdString(built.error), /*retryable=*/true);
        return;
    }

    // Record the (deterministic) identity id before broadcasting.
    m_record.identity_id = platform::st::IdentityIdFromOutpoint(burn_outpoint);
    setState(State::IDENTITY_BROADCAST);

    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().broadcastStateTransition(built.value->bytes, [self](platform::Result<platform::BroadcastResult> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (res.ok() && !res.value->accepted &&
                res.value->error.find("already") == std::string::npos) {
                self->fail(tr("identity"), QString::fromStdString(res.value->error), /*retryable=*/true);
                return;
            }
        });
    });
}

void IdentityFlow::confirmIdentity()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentity(m_record.identity_id, [self](platform::Result<std::optional<platform::Identity>> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!res.ok()) {
                LogPrintf("Platform identity flow: confirmation failed: %s\n", res.error);
                if (++self->m_retries >= MAX_CONFIRM_POLLS) {
                    // The transition may never have left this process (for
                    // example, a crash immediately after persisting
                    // IDENTITY_BROADCAST). Rebuild and rebroadcast it
                    // idempotently after bounded failed/absent reads.
                    self->m_retries = 0;
                    self->setState(State::FUNDING_LOCKED);
                }
                return;
            }
            if (res.value->has_value()) {
                self->setState(State::IDENTITY_CONFIRMED);
            } else if (++self->m_retries >= MAX_CONFIRM_POLLS) {
                // Broadcast may have been lost (e.g. crash between persist and
                // send) — step back and rebroadcast.
                self->m_retries = 0;
                self->setState(State::FUNDING_LOCKED);
            }
        });
    });
}

void IdentityFlow::broadcastPreorder()
{
    const auto salted_hash{platform::st::SaltedDomainHash(m_record.preorder_salt, m_record.normalized_label, "dash")};

    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentityContractNonce(m_record.identity_id, platform::DPNS_CONTRACT_ID,
        [self, salted_hash](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->m_service.post([self, salted_hash, nonce_res = std::move(nonce_res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!nonce_res.ok()) return;

            Wallet& wallet{self->m_service.walletModel().wallet()};
            const auto signer = [&wallet](const uint256& digest, std::vector<uint8_t>& sig) {
                return wallet.signPlatformDigest(PlatformKeyType::IdentityAuth, 0, 1, digest, sig);
            };
            auto built{platform::st::BuildDpnsPreorder(self->m_record.identity_id, *nonce_res.value + 1,
                                                       salted_hash, /*signature_public_key_id=*/1, signer)};
            if (!built.ok()) {
                self->fail(tr("preorder"), QString::fromStdString(built.error), /*retryable=*/true);
                return;
            }
            self->setState(State::PREORDER_BROADCAST);
            self->m_step_in_flight = true;
            QPointer<IdentityFlow> inner{self};
            self->m_service.client().broadcastStateTransition(built.value->bytes, [inner](platform::Result<platform::BroadcastResult> res) {
                if (!inner) return;
                inner->m_service.post([inner, res = std::move(res)] {
                    if (!inner) return;
                    inner->m_step_in_flight = false;
                    if (res.ok() && (res.value->accepted ||
                                     res.value->error.find("already") != std::string::npos)) {
                        inner->setState(State::PREORDER_WAIT);
                    } else if (res.ok()) {
                        inner->fail(tr("preorder"), QString::fromStdString(res.value->error), /*retryable=*/true);
                    }
                });
            });
        });
    });
}

void IdentityFlow::broadcastDomain()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentityContractNonce(m_record.identity_id, platform::DPNS_CONTRACT_ID,
        [self](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->m_service.post([self, nonce_res = std::move(nonce_res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!nonce_res.ok()) return;

            Wallet& wallet{self->m_service.walletModel().wallet()};
            const auto signer = [&wallet](const uint256& digest, std::vector<uint8_t>& sig) {
                return wallet.signPlatformDigest(PlatformKeyType::IdentityAuth, 0, 1, digest, sig);
            };
            const auto& rec{self->m_record};
            auto built{platform::st::BuildDpnsDomain(rec.identity_id, *nonce_res.value + 1, rec.label,
                                                     rec.normalized_label, "dash", rec.preorder_salt,
                                                     /*signature_public_key_id=*/1, signer)};
            if (!built.ok()) {
                self->fail(tr("domain"), QString::fromStdString(built.error), /*retryable=*/true);
                return;
            }
            self->m_step_in_flight = true;
            QPointer<IdentityFlow> inner{self};
            self->m_service.client().broadcastStateTransition(built.value->bytes, [inner](platform::Result<platform::BroadcastResult> res) {
                if (!inner) return;
                inner->m_service.post([inner, res = std::move(res)] {
                    if (!inner) return;
                    inner->m_step_in_flight = false;
                    if (!res.ok()) return;
                    if (res.value->accepted ||
                        res.value->error.find("already") != std::string::npos) {
                        inner->setState(State::DOMAIN_BROADCAST);
                    } else if (res.value->error.find("preorder") != std::string::npos) {
                        // Preorder not visible yet — wait and retry.
                    } else {
                        inner->fail(tr("domain"), QString::fromStdString(res.value->error), /*retryable=*/true);
                    }
                });
            });
        });
    });
}

void IdentityFlow::confirmDomain()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().resolveName(m_record.normalized_label, [self](platform::Result<std::optional<platform::DpnsName>> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!res.ok()) return;

            if (res.value->has_value()) {
                if ((*res.value)->identity == self->m_record.identity_id) {
                    self->setState(State::REGISTERED);
                } else if (!self->m_record.contested) {
                    self->fail(tr("domain"), tr("the name was registered by another identity"), /*retryable=*/false);
                } else {
                    self->fail(tr("vote"), tr("the contested name was awarded to another identity"), /*retryable=*/false);
                }
                return;
            }

            if (self->m_record.contested) {
                // No document yet: a masternode vote is (probably) in
                // progress. Reflect that and keep polling.
                if (self->m_record.state != State::CONTESTED_PENDING) {
                    self->setState(State::CONTESTED_PENDING);
                }
                return;
            }

            if (++self->m_retries >= MAX_CONFIRM_POLLS) {
                // Domain broadcast may have been lost — step back and
                // rebroadcast with the same persisted salt.
                self->m_retries = 0;
                self->setState(State::PREORDER_WAIT);
            }
        });
    });
}
