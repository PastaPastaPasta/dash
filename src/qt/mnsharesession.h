// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MNSHARESESSION_H
#define BITCOIN_QT_MNSHARESESSION_H

#include <consensus/amount.h>

#include <univalue.h>

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

//! Pure-logic engine (no widgets, no QObject) for a shared-masternode
//! multi-party session. It owns the session envelope: a JSON file passed
//! between participants that fully describes the session, so any participant
//! holding the latest copy can continue it.
//!
//! Envelope shape (UniValue JSON, version 1):
//! {type, version, network, sessionId, revision, stage, fundingTx, shares[],
//!  terms{}, contributions[], protx, consentHash, collateralIndex, sigs[],
//!  prepareWallet, operatorSecretHolder}
//!
//! All amounts are duffs (CAmount). All user-displayable errors come back as
//! translated QStrings. Signatures are verified natively at import time
//! (recompute the consent digest from the envelope's protx hex and check each
//! compact signature against the share owner key); an unverifiable signature
//! is never counted.
class MnShareSession
{
public:
    enum class Stage {
        Draft,         //!< terms and contributions still editable
        Frozen,        //!< register_shared_prepare done; consent hash fixed
        Signing,       //!< collecting share owner consent signatures
        Combined,      //!< shared_combine done; join sigs embedded
        FundingSigned, //!< every funding input carries its signature
        Broadcast,     //!< sent to the network
    };

    enum class MergeResult {
        Merged,     //!< envelopes were compatible; new data was absorbed
        OtherNewer, //!< the imported envelope supersedes ours; caller decides
        OtherOlder, //!< ours supersedes the imported one; nothing absorbed
        Conflict,   //!< incompatible envelopes; see the error string
    };

    struct Share {
        QString label;
        CAmount amount{0};
        QString ownerAddress;  //!< P2PKH only (immutable share owner key)
        QString refundAddress; //!< P2PKH or P2SH, immutable
        QString rewardAddress; //!< empty = fall back to the refund address
    };

    struct Input {
        QString txid; //!< 64 hex chars
        uint32_t vout{0};
        //! Fixed at contribution time and never rewritten afterwards: the
        //! consent digest covers every input's nSequence, so a rewrite after
        //! freeze would invalidate all collected signatures.
        uint32_t sequence{0xffffffff};
    };

    struct Contribution {
        QString label;
        std::vector<Input> inputs;
        bool hasChange{false};
        QString changeAddress;
        CAmount changeAmount{0};
        //! Output index of the change in the built funding transaction;
        //! recomputed whenever the funding transaction is rebuilt
        int changeIndex{-1};
    };

    struct Terms {
        QString coreP2PAddrs;   //!< ADDR:PORT, or empty for a later ProUpServTx
        QString operatorPubKey; //!< hex BLS public key (basic scheme)
        QString votingAddress;  //!< P2PKH
        int operatorReward{0};  //!< 0..10000, in 1/100 of a percent
        uint32_t earlyPeriodBlocks{0};
        CAmount earlyPenalty{0};
    };

    struct Signature {
        int shareIndex{-1};
        QString signatureB64; //!< base64 65-byte compact signature
    };

    struct SignatureCheck {
        int shareIndex{-1};
        bool valid{false};
        QString error;
    };

    //! Mirror of the BuildProDisTx output math for previewing a unilateral
    //! dissolution before it is built
    struct PenaltyPreview {
        bool valid{false};
        QString error;
        bool early{false};
        CAmount penalty{0};
        //! First block height at which a unilateral dissolution is penalty-free
        int penaltyFreeHeight{0};
        //! Refund per share index. The actor's entry is principal minus
        //! penalty minus fee (a zero entry is omitted from the real
        //! transaction); every other entry is principal plus its pro-rata
        //! penalty bonus (sequential floor, remainder to the last non-actor).
        std::vector<CAmount> payouts;
    };

    //! New Draft session: random sessionId, revision 1, current network
    MnShareSession();

    Stage stage() const { return m_stage; }
    QString stageName() const { return StageName(m_stage); }
    static QString StageName(Stage stage);

    const QString& sessionId() const { return m_session_id; }
    const QString& network() const { return m_network; }
    int revision() const { return m_revision; }

    std::vector<Share>& shares() { return m_shares; }
    const std::vector<Share>& shares() const { return m_shares; }
    Terms& terms() { return m_terms; }
    const Terms& terms() const { return m_terms; }
    const std::vector<Contribution>& contributions() const { return m_contributions; }
    const std::vector<Signature>& signatures() const { return m_sigs; }

    const QString& fundingTxHex() const { return m_funding_tx; }
    const QString& protxHex() const { return m_protx; }
    const QString& consentHash() const { return m_consent_hash; }
    int collateralIndex() const { return m_collateral_index; }

    const QString& prepareWallet() const { return m_prepare_wallet; }
    void setPrepareWallet(const QString& name) { m_prepare_wallet = name; }
    const QString& operatorSecretHolder() const { return m_operator_secret_holder; }
    void setOperatorSecretHolder(const QString& label) { m_operator_secret_holder = label; }

    //! Serialize the envelope
    UniValue toJson() const;
    QString toJsonString() const;

    //! Strict parse of an envelope. The network must match the running node
    //! (hard error naming both networks) and the stage must be known.
    //! Signatures are stored as-is; callers must run verifyAllSignatures()
    //! (or rely on addSignature/mergeEnvelope, which verify) before counting
    //! them.
    bool fromJson(const UniValue& json, QString& error);
    bool fromJson(const std::string& text, QString& error);

    //! All current draft-side share-table problems at once, mirroring the
    //! consensus checks in IsShareListTriviallyValid (empty list = valid)
    QStringList validateShares() const;

    //! Draft only: record a participant's funding contribution and rebuild
    //! the funding transaction deterministically (inputs, then change
    //! outputs, both in contribution order). Bumps the revision.
    bool addContribution(const Contribution& contribution, QString& error);
    //! Draft only: remove a contribution by label and rebuild. Bumps the revision.
    bool removeContribution(const QString& label, QString& error);

    //! Record the register_shared_prepare result and advance Draft -> Frozen.
    //! Cross-checks that the protx hex parses as a shared registration and
    //! that its recomputed consent digest equals consent_hash_hex.
    bool freeze(const QString& protx_hex, const QString& consent_hash_hex, int collateral_index, QString& error);
    //! Discard the frozen transaction and all signatures, return to Draft and
    //! bump the revision (previously collected signatures can never apply again)
    void unfreeze();

    //! Verify one base64 compact signature against the frozen transaction:
    //! recompute the consent digest from the protx hex, hard-compare it to
    //! the stored consentHash, then check the signature against the share
    //! owner key (canonical encoding required)
    bool verifySignature(int share_index, const QString& sig_b64, QString& error) const;
    //! Re-verify every stored signature (import-time verification)
    std::vector<SignatureCheck> verifyAllSignatures() const;

    //! Store a signature after verifying it. A byte-identical duplicate is a
    //! silent success; a different signature for an already-signed index is
    //! rejected as a stale-version conflict.
    bool addSignature(int share_index, const QString& sig_b64, QString& error);
    int signedCount() const;
    std::vector<int> missingIndexes() const;
    //! Base64 signature for a share index, or empty if not signed yet
    QString signatureFor(int share_index) const;

    //! Absorb another copy of the same session. Never merges across
    //! diverging terms: a higher/lower revision or a consent-hash mismatch is
    //! reported to the caller (supersede dialog) instead. On Merged, the
    //! union of the other envelope's *verified* signatures is absorbed and a
    //! further-advanced stage (with its transaction hex) is adopted; error
    //! carries any skipped-signature warnings even on success.
    MergeResult mergeEnvelope(const MnShareSession& other, QString& error);

    //! Record the shared_combine result and advance to Combined
    bool setCombinedTx(const QString& tx_hex, QString& error);
    //! Record the fully signed funding transaction and advance to FundingSigned
    bool setFundingSignedTx(const QString& tx_hex, QString& error);
    //! Mark the session broadcast
    bool setBroadcast(QString& error);

    //! First 8 hex characters of the consent hash, uppercased: the phrase
    //! participants read aloud to each other before signing
    QString consentCheckPhrase() const;
    //! Approximate duration of a block count at 2.5 minutes per block
    static QString HumanEarlyPeriod(uint32_t blocks);

    //! Preview of a unilateral dissolution built at chain height at_height
    //! (the transaction would confirm at at_height + 1), using this
    //! session's share table and penalty terms
    PenaltyPreview penaltyPreview(int actor_index, CAmount fee, int at_height, int registered_height) const;
    //! Same math for an already-registered masternode's share amounts
    static PenaltyPreview PenaltyPreviewFor(const std::vector<CAmount>& share_amounts, int actor_index,
                                            CAmount early_penalty, uint32_t early_period_blocks, CAmount fee,
                                            int at_height, int registered_height);

private:
    //! Rebuild m_funding_tx from m_contributions (deterministic order) and
    //! refresh each contribution's changeIndex
    bool rebuildFundingTx(QString& error);
    const Signature* findSignature(int share_index) const;

    QString m_network;
    QString m_session_id;
    int m_revision{1};
    Stage m_stage{Stage::Draft};
    QString m_funding_tx;
    std::vector<Share> m_shares;
    Terms m_terms;
    std::vector<Contribution> m_contributions;
    QString m_protx;
    QString m_consent_hash;
    int m_collateral_index{-1};
    std::vector<Signature> m_sigs;
    QString m_prepare_wallet;
    QString m_operator_secret_holder;
};

#endif // BITCOIN_QT_MNSHARESESSION_H
