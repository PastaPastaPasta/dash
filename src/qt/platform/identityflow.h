// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
#define BITCOIN_QT_PLATFORM_IDENTITYFLOW_H

#include <consensus/amount.h>
#include <platform/client.h>
#include <platform/types.h>
#include <primitives/transaction.h>
#include <qt/walletmodel.h>
#include <uint256.h>
#include <wallet/platformtypes.h>

#include <QObject>
#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PlatformService;

/**
 * Creates and tops up the wallet's DashPay identity (identity index 0), as
 * the mobile wallets do: the wallet builds an asset lock paying a key derived
 * from its seed, the user confirms its amount and exact fee before it is
 * sent, and once the node has the payment's InstantSend lock or ChainLock one
 * SDK call creates the identity or adds the credits.
 *
 * The asset lock's wallet record is written before it is sent and kept
 * until Platform's use of it is proved: the registered identity, a top-up's
 * proved answer, or Platform's proved record that the lock is used up.
 * Whatever else a call ends with leaves the record for Continue, which sends
 * the same asset lock again with a higher fee increase and never funds a new
 * one: a call that was dropped or answered without proof, a node reporting
 * the lock consumed (checked again against Platform's record), Platform
 * behind Core or needing a ChainLock proof (sent again after the next
 * ChainLock), and a refusal, which is set aside without hiding Create or Top
 * up. A payment that was abandoned is kept in case it is mined; only one
 * replaced by a conflicting payment, or missing, ends its record.
 *
 * The passphrase given to fund keeps the wallet unlocked until Platform has
 * answered, through the waits for a lock and for Platform to reach the
 * payment's block, and is released at once when a gate closes. Each call
 * reserves every fee increase it could build, so a dropped one is never
 * resubmitted identically; once the next one would be above the binding's
 * cap, the wait ends, the record is kept, and only Platform's record of the
 * lock is read until FEE_INCREASE_MEMORY after the last attempt.
 */
class IdentityFlow : public QObject
{
    Q_OBJECT

public:
    //! What a new identity is funded with, as the mobile wallets: enough to
    //! register a username.
    static constexpr CAmount REGISTRATION_AMOUNT{3'000'000};
    //! The smallest top-up the mobile wallets offer.
    static constexpr CAmount MIN_TOP_UP{1'000'000};
    //! The wallet records of asset locks whose use Platform has not proved.
    static constexpr const char* FUNDING_PREFIX{"dashpay/funding/"};
    //! How long a fee increase stays used: Tenderdash remembers a refused
    //! transition this long, so within it a resend must differ.
    static constexpr std::chrono::hours FEE_INCREASE_MEMORY{24};

    //! Whether the user sends an asset lock buying `amount` duffs of credits
    //! for a network fee of `fee`.
    using Confirm = std::function<bool(CAmount amount, CAmount fee)>;
    //! The proof Platform takes for a committed asset lock; nullopt until
    //! the node has a lock for it.
    using Proof = std::function<std::optional<platform::AssetLockProof>(const CTransactionRef& tx)>;
    //! The ChainLock proof of a committed asset lock; nullopt until its
    //! block is ChainLocked.
    using ChainProof = std::function<std::optional<platform::ChainAssetLockProof>(const CTransactionRef& tx)>;

    //! Where an asset lock Platform has not been proved to use stands.
    enum class FundingState : uint8_t {
        //! Not sent, or nothing settled: Continue sends it.
        PENDING,
        //! Platform could not use the InstantSend proof: sent with a
        //! ChainLock proof once its block is ChainLocked.
        NEEDS_CHAINLOCK,
        //! Platform had not reached the proof's Core height: sent again after
        //! the next ChainLock.
        PLATFORM_BEHIND,
        //! A top-up Platform answered without proving the lock used: settled
        //! by Platform's record of the lock.
        CONFIRMING,
        //! A node reported the lock consumed, which proves nothing: settled
        //! by Platform's record of the lock (and the identity it registers).
        CHECK_AGAIN,
        //! Platform refused it. Kept, but it does not stop a new funding.
        SET_ASIDE,
    };

    //! An asset lock this wallet sent whose use Platform has not proved.
    struct Funding {
        std::string record;
        //! The key the lock's credit output pays, which signs for it.
        wallet::AssetLockFundingKey key;
        //! The identity it adds credits to; nullopt: it registers the identity.
        std::optional<platform::Identifier> top_up;
        uint256 txid;
        uint32_t credit_index{0};
        CAmount amount{0};
        FundingState state{FundingState::PENDING};
        //! The highest user_fee_increase a submission may have used, and when
        //! the last one started (UNIX seconds).
        std::optional<uint16_t> last_fee_increase;
        int64_t last_attempt{0};

        COutPoint outpoint() const { return {txid, credit_index}; }
    };

    enum class Step {
        IDLE,
        LOOKING_UP,
        FUNDING,          //!< building the asset lock, asking for the passphrase or the confirmation
        WAITING_FOR_LOCK, //!< committed; waiting for a lock, a ChainLock or Platform to catch up
        SENDING,
        CHECKING, //!< reading Platform's record of a lock
    };

    //! An empty proof uses the node: the payment's InstantSend lock, else its
    //! ChainLock; an empty chain proof its ChainLock.
    IdentityFlow(PlatformService& service, Confirm confirm, Proof proof = {}, ChainProof chain_proof = {});

    Step step() const { return m_step; }
    //! Whether the identity was looked up since the client started.
    bool lookedUp() const { return m_looked_up; }
    const std::optional<platform::Identity>& identity() const { return m_identity; }
    //! The asset lock Continue acts on, if any: one not set aside, not
    //! abandoned, and not a registration while the wallet has an identity.
    //! While there is one, nothing new is funded.
    std::optional<Funding> funding() const;
    //! Every asset lock record, the kept ones included.
    std::vector<Funding> fundings() const;
    //! Why the last step failed; empty when it did not.
    const QString& error() const { return m_error; }
    //! What a check of Platform's record of a lock found, until the next
    //! action.
    const QString& note() const { return m_note; }

    //! Look up the identity registered under the wallet's MASTER key,
    //! asking for the passphrase if the key must be derived.
    void lookUp();
    //! Fund and register the identity.
    void create();
    //! Fund `amount` duffs of credits for the identity.
    void topUp(CAmount amount);
    //! Send funding() to Platform again, or check it again.
    void resume();

    //! Whether the wallet abandoned the payment while it is not in a block.
    bool inactive(const Funding& funding) const;

Q_SIGNALS:
    void changed();

private:
    void serviceChanged();
    void chainLocked();
    //! Check a CONFIRMING or CHECK_AGAIN funding() when nothing else runs.
    void checkPending();
    void startLookUp(bool unlock);
    void onLookedUp(platform::Result<platform::Identity> result);
    void fund(CAmount amount, Funding pending);
    //! Ask for the passphrase; nullopt (and IDLE again) if the wallet stays locked.
    std::optional<WalletModel::UnlockContext> beginFunding();
    //! Whether a funding can still go ahead after a dialog: every gate open,
    //! a client, nothing else started, and the identity as it was.
    bool stillCurrent(const Funding& pending) const;
    void awaitLock(Funding pending, CTransactionRef tx, WalletModel::UnlockContext unlock);
    //! Send the funding to Platform once the node has a proof for it and a
    //! client; one PLATFORM_BEHIND waits for a ChainLock.
    void send(bool chain_locked = false);
    //! Platform's answer to a call that started at `floor`, made after an
    //! attempt at `previous_attempt`.
    void sent(Funding funding, uint16_t floor, int64_t previous_attempt, const platform::Status& status,
              const platform::FundingReport& report);
    //! Settle a CONFIRMING or CHECK_AGAIN lock, or a capped one, from
    //! Platform's proved records: for a registration the identity it
    //! registers, then the lock.
    void check(Funding funding);
    void readLockState(Funding funding);
    //! Send a lock Platform has not used again: at once if the wallet can
    //! sign without asking, else on Continue.
    void sendAgain(Funding funding);
    //! A check settled nothing.
    void unsettled(const Funding& funding);
    //! Why a funding whose fee steps are all used waits, and until when.
    QString cappedNote(const Funding& funding) const;
    bool save(const Funding& funding);
    //! Platform's use of the lock is proved: end its record.
    void retire(const Funding& funding);
    void finish(const QString& error = {});
    //! The callback of the one call in flight; it expires if the call is dropped.
    template <typename T>
    std::function<void(T)> tracked(std::function<void(T)> fn);

    PlatformService& m_service;
    const Confirm m_confirm;
    Proof m_proof;
    ChainProof m_chain_proof;
    Step m_step{Step::IDLE};
    bool m_looked_up{false};
    std::optional<platform::Identity> m_identity;
    QString m_error;
    QString m_note;
    //! Hash160 of the MASTER key, kept once derived: deriving needs an
    //! unlocked wallet.
    std::optional<std::array<uint8_t, 20>> m_key_hash;
    std::weak_ptr<void> m_call;
    //! The funding being waited for or sent, and the unlock it signs with,
    //! held until Platform has answered.
    std::optional<Funding> m_funding;
    CTransactionRef m_tx;
    std::optional<WalletModel::UnlockContext> m_unlock;
};

#endif // BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
