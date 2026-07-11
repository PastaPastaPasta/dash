// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
#define BITCOIN_QT_PLATFORM_IDENTITYFLOW_H

#include <consensus/amount.h>
#include <platform/types.h>
#include <uint256.h>

#include <QObject>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

class PlatformService;

/**
 * Persistent, resumable state machine driving identity creation and DPNS
 * username registration:
 *
 *   NONE -> FUNDING_SENT -> FUNDING_LOCKED -> IDENTITY_BROADCAST
 *        -> IDENTITY_CONFIRMED -> PREORDER_BROADCAST -> PREORDER_WAIT
 *        -> DOMAIN_BROADCAST -> { REGISTERED | CONTESTED_PENDING -> ... }
 *
 * Every mutation of the persisted record happens BEFORE the corresponding
 * irreversible action (in particular the preorder salt is stored before the
 * preorder is broadcast). advance() is idempotent and event-driven; on
 * restart the flow re-derives ground truth (wallet tx status, proved
 * platform queries) instead of trusting the recorded step blindly.
 *
 * The record is stored in the wallet DB under the "identity/0" platform
 * data key (single identity per wallet in this version).
 */
class IdentityFlow : public QObject
{
    Q_OBJECT

public:
    enum class State : uint8_t {
        NONE = 0,
        FUNDING_SENT = 1,
        FUNDING_LOCKED = 2,
        IDENTITY_BROADCAST = 3,
        IDENTITY_CONFIRMED = 4,
        PREORDER_BROADCAST = 5,
        PREORDER_WAIT = 6,
        DOMAIN_BROADCAST = 7,
        REGISTERED = 8,
        CONTESTED_PENDING = 9,
        FAILED = 255,
    };
    Q_ENUM(State)

    //! Persisted snapshot (serialized with CDataStream into the wallet DB).
    struct Record {
        static constexpr uint8_t CURRENT_VERSION{1};
        uint8_t version{CURRENT_VERSION};
        State state{State::NONE};
        //! L1 funding
        uint256 funding_txid;
        uint32_t funding_vout{0};      //!< index of the OP_RETURN burn output
        uint32_t funding_key_index{0}; //!< registration funding derivation index
        CAmount funding_amount{0};
        //! Identity
        platform::Identifier identity_id{};
        //! Username
        std::string label;             //!< as typed, e.g. "Alice"
        std::string normalized_label;  //!< homograph-safe
        std::array<uint8_t, 32> preorder_salt{};
        bool contested{false};
        //! Diagnostics
        std::string last_error;
        int64_t started_at{0};
    };

    explicit IdentityFlow(PlatformService& service, QObject* parent = nullptr);

    const Record& record() const { return m_record; }
    bool active() const
    {
        return m_record.state != State::NONE && m_record.state != State::REGISTERED &&
               m_record.state != State::FAILED;
    }

    //! Begin a new registration. Creates+commits the asset lock transaction
    //! and persists the initial record. Returns false with error set when the
    //! flow is already active or the wallet refuses.
    bool start(const QString& label, CAmount funding_amount, QString& error);

    //! Drive the state machine one step if possible. Safe to call at any
    //! time (timer ticks, islock signals, client callbacks).
    void advance();

    //! Re-derive progress from ground truth after a restart, then advance.
    void resume();

    //! Clear a FAILED flow so the user can retry from scratch. Never clears
    //! an in-progress flow.
    void reset();

Q_SIGNALS:
    void stateChanged();
    void failed(const QString& step, const QString& error);

private:
    void setState(State state);
    void fail(const QString& step, const QString& error, bool retryable);
    bool load();
    void save();

    void checkFundingLock();
    void broadcastIdentityCreate();
    void confirmIdentity();
    void broadcastPreorder();
    void broadcastDomain();
    void confirmDomain();
    void checkContestedOutcome();

    PlatformService& m_service;
    Record m_record;
    bool m_step_in_flight{false};
    int m_retries{0};
};

#endif // BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
