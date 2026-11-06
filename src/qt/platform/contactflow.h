// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTFLOW_H
#define BITCOIN_QT_PLATFORM_CONTACTFLOW_H

#include <platform/types.h>
#include <qt/sendcoinsrecipient.h>

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class IdentityFlow;
class PlatformService;

/**
 * The DashPay contacts of the wallet's identity, as the mobile wallets keep
 * them: read from the contact requests on Platform, newest per counterparty,
 * and held in memory only. A request each way is a contact; one from them is
 * waiting to be accepted, one from us waiting for them.
 *
 * Each direction is read a page at a time and what was read is kept for the
 * session: a page that fails keeps the pages before it, says why, and the
 * next refresh() goes on from where it stopped. Our requests never wait for
 * theirs.
 *
 * Once our requests are read, our receiving chain towards every recipient
 * we ever sent one to is imported, born at the earliest of those requests,
 * so a wallet restored from its seed sees what they paid it. A record in the
 * wallet keeps the birth each was imported at, so a later session neither
 * imports it again nor asks to. Only the rescans the wallet owes run, on a
 * thread the flow owns, aborted and joined when the flow goes: behind one
 * that needs blocks this node lacks, the new chains are scanned once from
 * the earliest whose blocks it has, and what it has of the rest is read once
 * for each set of chains imported. A pass that fails is tried again at a
 * later block. A locked wallet imports the chains when it is unlocked.
 *
 * Adding a contact (by exact username) and accepting one send our request
 * with one SDK call. The wallet derives the ECDH secret and the receiving
 * chain the request carries, and imports that chain first, so the contact's
 * payments are seen; no private key leaves it.
 *
 * Paying a contact hands the Send tab an address of the payment chain their
 * request carries, labelled with their username, chosen from the wallet's
 * payments to that chain so it stays where their wallet looks.
 */
class ContactFlow : public QObject
{
    Q_OBJECT

public:
    enum class Step {
        IDLE,
        RESOLVING, //!< resolving the username
        FETCHING,  //!< reading the contact's identity
        SENDING,   //!< from the passphrase to Platform's answer
        PAYING,    //!< reading the identity of the contact to pay
    };
    //! In the order contacts() lists them.
    enum class State {
        INCOMING, //!< they sent a request, we did not
        ESTABLISHED,
        OUTGOING, //!< we sent a request, they did not
    };
    struct Contact {
        platform::Identifier id{};
        State state{State::INCOMING};
        //! Their username, once read; nullopt when unread or they have none.
        std::optional<std::string> username;
    };

    //! Incoming requests listed: the newest ones. The rest are counted.
    static constexpr size_t INCOMING_SHOWN{100};
    //! The wallet records of the receiving chains imported towards each
    //! recipient: <hex ours>/<hex theirs>, the birth (seconds, LE64).
    static constexpr const char* CHAIN_PREFIX{"dashpay/chain/"};

    ContactFlow(PlatformService& service, IdentityFlow& identity_flow);

    Step step() const { return m_step; }
    //! Whether every request each way was read since the identity was found.
    bool swept() const { return m_sent.complete && m_received.complete; }
    //! Whether every request we sent was read since the identity was found.
    bool sentRead() const { return m_sent.complete; }
    //! Incoming first, the newest INCOMING_SHOWN of them, then established,
    //! then outgoing, from what has been read.
    std::vector<Contact> contacts() const;
    //! The incoming requests contacts() leaves out.
    size_t moreIncoming() const;
    //! Why the last action failed, else why a read stopped short; empty when
    //! neither did.
    QString error() const;
    //! Recipients of our requests whose receiving chain waits for the wallet
    //! to be unlocked.
    size_t chainsToImport() const;
    //! Whether the owed rescans are running.
    bool rescanning() const { return m_rescanning; }
    //! The first block a rescan could not read, naming it; empty when every
    //! rescan read what it needed.
    const QString& rescanNote() const { return m_rescan_note; }

    //! Send a contact request to the identity a username resolves to.
    void add(const std::string& username);
    //! Accept the incoming request of a contact.
    void accept(const platform::Identifier& contact);
    //! Read the contact requests again.
    void refresh();
    //! Ask for the passphrase and import the chains chainsToImport() counts.
    void importChains();

    ~ContactFlow() override;
    //! Choose the address to pay an established contact at, and hand it to
    //! payTo().
    void pay(const platform::Identifier& contact);

Q_SIGNALS:
    void changed();
    //! Pay the contact: an address of their payment chain, labelled with
    //! their username.
    void payTo(const SendCoinsRecipient& recipient);

private:
    using Requests = std::map<platform::Identifier, platform::ContactRequest>;

    //! What was read of one direction this session.
    struct Direction {
        //! Every request read, oldest first.
        std::vector<platform::ContactRequest> read;
        bool complete{false};
        //! Where the next page starts.
        uint64_t next_since_ms{0};
        //! Why the last page failed; empty when it did not. No page is read
        //! again until refresh().
        QString error;
        std::weak_ptr<void> call;
    };

    void update();
    //! Read whichever direction is unread and not being read.
    void sweep();
    //! The next page of a direction; `next`: from the callback of the page
    //! before, which still holds the call.
    void readRequests(platform::ContactDirection direction, bool next = false);
    //! Our newest request to each recipient, and theirs from each sender.
    Requests sent() const;
    Requests received() const;
    void readNames();
    //! One changed() for every name read until the event loop runs.
    void namesChanged();
    //! The recipients of our requests whose chain the wallet records no
    //! import of at or before the earliest request, with that birth.
    std::map<platform::Identifier, int64_t> unimported() const;
    //! Import our receiving chain towards each unimported() recipient, then
    //! run the rescans owed. `ask`: ask for the passphrase of a locked wallet.
    void recover(bool ask = false);
    //! The rescans owed, on the rescan thread; births: those just imported,
    //! which start a generation.
    void rescan(std::vector<int64_t> births);
    //! Back on the flow's thread: the owed rescan whose first missing block
    //! was named, if any, whether the pass failed, and whether the scan of
    //! the generation's births ran.
    void rescanned(std::optional<std::pair<int64_t, int>> unavailable, bool failed, bool pushed_scanned);
    void resolve();
    void onResolved(platform::Result<platform::Identifier> result);
    void fetch();
    void onFetched(platform::Result<platform::Identity> result);
    void onSent(const platform::Status& status);
    void prefill(const platform::Identity& theirs);
    void finish(const QString& error = {});

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    Step m_step{Step::IDLE};
    //! The username being added, and the contact the request goes to or
    //! the payment is for.
    std::string m_label;
    platform::Identifier m_contact{};
    //! The step's call, until it calls back or is dropped.
    std::weak_ptr<void> m_call;
    Direction m_sent;
    Direction m_received;
    //! Usernames read, by identity; nullopt: none.
    std::map<platform::Identifier, std::optional<std::string>> m_names;
    std::map<platform::Identifier, std::weak_ptr<void>> m_name_calls;
    bool m_names_changed{false};
    QString m_error;
    //! Births whose rescan waits for the one running.
    std::vector<int64_t> m_births;
    //! The births of this generation, until a scan settles them.
    std::vector<int64_t> m_pushed;
    std::thread m_rescan_thread;
    std::atomic<bool> m_rescan_abort{false};
    //! The thread runs; m_rescanning stays set until the flow has its result.
    std::atomic<bool> m_rescanning_thread{false};
    bool m_rescanning{false};
    //! Owed rescans whose missing block was named this generation.
    std::set<std::pair<int64_t, int>> m_named;
    //! A failed rescan is retried no earlier than this (UNIX seconds), at a
    //! block.
    int64_t m_rescan_retry_at{0};
    bool m_rescan_retry{false};
    int m_rescan_failures{0};
    QString m_rescan_note;
};

#endif // BITCOIN_QT_PLATFORM_CONTACTFLOW_H
