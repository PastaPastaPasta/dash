// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/mnsharesession.h>

#include <arith_uint256.h>
#include <chainparams.h>
#include <core_io.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <key_io.h>
#include <messagesigner.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/standard.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <QCoreApplication>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <variant>

namespace {

constexpr const char* ENVELOPE_TYPE{"dash-shared-mn-session"};
constexpr int ENVELOPE_VERSION{1};

QString Tr(const char* text)
{
    return QCoreApplication::translate("MnShareSession", text);
}

QString Tr(const char* text, const char* disambiguation, int n)
{
    return QCoreApplication::translate("MnShareSession", text, disambiguation, n);
}

//! Untranslated stage keys used in the envelope JSON
const char* StageKey(MnShareSession::Stage stage)
{
    switch (stage) {
    case MnShareSession::Stage::Draft: return "draft";
    case MnShareSession::Stage::Frozen: return "frozen";
    case MnShareSession::Stage::Signing: return "signing";
    case MnShareSession::Stage::Combined: return "combined";
    case MnShareSession::Stage::FundingSigned: return "fundingSigned";
    case MnShareSession::Stage::Broadcast: return "broadcast";
    }
    return "draft";
}

std::optional<MnShareSession::Stage> StageFromKey(const std::string& key)
{
    for (const auto stage : {MnShareSession::Stage::Draft, MnShareSession::Stage::Frozen,
                             MnShareSession::Stage::Signing, MnShareSession::Stage::Combined,
                             MnShareSession::Stage::FundingSigned, MnShareSession::Stage::Broadcast}) {
        if (key == StageKey(stage)) return stage;
    }
    return std::nullopt;
}

//! Format duffs as a DASH amount without floating point
QString FormatDash(CAmount amount)
{
    const bool negative{amount < 0};
    if (negative) amount = -amount;
    QString ret = QString::number(static_cast<qlonglong>(amount / COIN));
    if (const CAmount frac{amount % COIN}; frac != 0) {
        QString frac_str = QStringLiteral("%1").arg(static_cast<qlonglong>(frac), 8, 10, QLatin1Char('0'));
        while (frac_str.endsWith(QLatin1Char('0'))) {
            frac_str.chop(1);
        }
        ret += QLatin1Char('.') + frac_str;
    }
    return negative ? QLatin1Char('-') + ret : ret;
}

bool IsTxidHex(const QString& txid)
{
    return txid.size() == 64 && IsHex(txid.toStdString());
}

//! Decode a frozen shared registration (funding tx + CProRegTx payload with a
//! share table) from the envelope's protx hex
bool DecodeSharedProTx(const QString& protx_hex, CMutableTransaction& tx, CProRegTx& payload, QString& error)
{
    if (!DecodeHexTx(tx, protx_hex.toStdString())) {
        error = Tr("The session's prepared transaction is not valid transaction hex.");
        return false;
    }
    // assert_type=false: a malformed file must produce an error, not a debug abort
    auto opt_payload = GetTxPayload<CProRegTx>(tx, /*assert_type=*/false);
    if (!opt_payload || !opt_payload->IsShared()) {
        error = Tr("The session's prepared transaction is not a shared masternode registration.");
        return false;
    }
    payload = std::move(*opt_payload);
    return true;
}

} // anonymous namespace

MnShareSession::MnShareSession()
    : m_network{QString::fromStdString(Params().NetworkIDString())},
      m_session_id{QString::fromStdString(GetRandHash().ToString())}
{
}

QString MnShareSession::StageName(Stage stage)
{
    switch (stage) {
    case Stage::Draft: return Tr("Draft");
    case Stage::Frozen:
        return Tr("Terms locked");
    case Stage::Signing:
        return Tr("Collecting approvals");
    case Stage::Combined:
        return Tr("Approvals combined");
    case Stage::FundingSigned:
        return Tr("Contributions signed");
    case Stage::Broadcast: return Tr("Broadcast");
    }
    return {};
}

UniValue MnShareSession::toJson() const
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("type", ENVELOPE_TYPE);
    ret.pushKV("version", ENVELOPE_VERSION);
    ret.pushKV("network", m_network.toStdString());
    ret.pushKV("sessionId", m_session_id.toStdString());
    ret.pushKV("revision", m_revision);
    ret.pushKV("stage", StageKey(m_stage));
    ret.pushKV("fundingTx", m_funding_tx.toStdString());

    UniValue shares(UniValue::VARR);
    for (const auto& share : m_shares) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("label", share.label.toStdString());
        entry.pushKV("amount", share.amount);
        entry.pushKV("ownerAddress", share.ownerAddress.toStdString());
        entry.pushKV("refundAddress", share.refundAddress.toStdString());
        entry.pushKV("rewardAddress", share.rewardAddress.toStdString());
        shares.push_back(entry);
    }
    ret.pushKV("shares", shares);

    UniValue terms(UniValue::VOBJ);
    terms.pushKV("coreP2PAddrs", m_terms.coreP2PAddrs.toStdString());
    terms.pushKV("operatorPubKey", m_terms.operatorPubKey.toStdString());
    terms.pushKV("votingAddress", m_terms.votingAddress.toStdString());
    terms.pushKV("operatorReward", m_terms.operatorReward);
    terms.pushKV("earlyPeriodBlocks", static_cast<int64_t>(m_terms.earlyPeriodBlocks));
    terms.pushKV("earlyPenalty", m_terms.earlyPenalty);
    ret.pushKV("terms", terms);

    UniValue contributions(UniValue::VARR);
    for (const auto& contribution : m_contributions) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("label", contribution.label.toStdString());
        UniValue inputs(UniValue::VARR);
        for (const auto& input : contribution.inputs) {
            UniValue in(UniValue::VOBJ);
            in.pushKV("txid", input.txid.toStdString());
            in.pushKV("vout", static_cast<int64_t>(input.vout));
            in.pushKV("sequence", static_cast<int64_t>(input.sequence));
            inputs.push_back(in);
        }
        entry.pushKV("inputs", inputs);
        if (contribution.hasChange) {
            UniValue change(UniValue::VOBJ);
            change.pushKV("address", contribution.changeAddress.toStdString());
            change.pushKV("amount", contribution.changeAmount);
            entry.pushKV("change", change);
        }
        entry.pushKV("changeIndex", contribution.changeIndex);
        contributions.push_back(entry);
    }
    ret.pushKV("contributions", contributions);

    ret.pushKV("protx", m_protx.toStdString());
    ret.pushKV("consentHash", m_consent_hash.toStdString());
    ret.pushKV("collateralIndex", m_collateral_index);

    UniValue sigs(UniValue::VARR);
    for (const auto& sig : m_sigs) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("shareIndex", sig.shareIndex);
        entry.pushKV("signature", sig.signatureB64.toStdString());
        sigs.push_back(entry);
    }
    ret.pushKV("sigs", sigs);

    ret.pushKV("prepareWallet", m_prepare_wallet.toStdString());
    ret.pushKV("operatorSecretHolder", m_operator_secret_holder.toStdString());
    return ret;
}

QString MnShareSession::toJsonString() const
{
    return QString::fromStdString(toJson().write(/*prettyIndent=*/2));
}

bool MnShareSession::fromJson(const std::string& text, QString& error)
{
    UniValue json;
    if (!json.read(text)) {
        error = Tr("The file is not valid JSON.");
        return false;
    }
    return fromJson(json, error);
}

bool MnShareSession::fromJson(const UniValue& json, QString& error)
{
    try {
        if (!json.isObject()) {
            error = Tr("The file is not a shared masternode session file.");
            return false;
        }
        const UniValue& type{json.find_value("type")};
        if (!type.isStr() || type.get_str() != ENVELOPE_TYPE) {
            error = Tr("The file is not a shared masternode session file.");
            return false;
        }
        const UniValue& version{json.find_value("version")};
        if (!version.isNum() || version.getInt<int>() != ENVELOPE_VERSION) {
            error = Tr("The session file uses an unsupported format version. Update Dash Core to open it.");
            return false;
        }
        const UniValue& network{json.find_value("network")};
        const std::string our_network{Params().NetworkIDString()};
        if (!network.isStr() || network.get_str() != our_network) {
            error = Tr("The session file is for the \"%1\" network but this node runs \"%2\". It cannot be used here.")
                        .arg(network.isStr() ? QString::fromStdString(network.get_str()) : Tr("unknown"),
                             QString::fromStdString(our_network));
            return false;
        }
        const UniValue& session_id{json.find_value("sessionId")};
        if (!session_id.isStr() || session_id.get_str().empty()) {
            error = Tr("The session file has no session id.");
            return false;
        }
        const UniValue& revision{json.find_value("revision")};
        if (!revision.isNum() || revision.getInt<int>() < 1) {
            error = Tr("The session file has an invalid revision.");
            return false;
        }
        const UniValue& stage_value{json.find_value("stage")};
        if (!stage_value.isStr()) {
            error = Tr("The session file has no stage.");
            return false;
        }
        const auto stage{StageFromKey(stage_value.get_str())};
        if (!stage) {
            error = Tr("The session file is at the unknown stage \"%1\". It was probably created by a newer Dash Core.")
                        .arg(QString::fromStdString(stage_value.get_str()));
            return false;
        }

        MnShareSession parsed;
        parsed.m_network = QString::fromStdString(network.get_str());
        parsed.m_session_id = QString::fromStdString(session_id.get_str());
        parsed.m_revision = revision.getInt<int>();
        parsed.m_stage = *stage;

        if (const UniValue& v{json.find_value("fundingTx")}; v.isStr()) {
            parsed.m_funding_tx = QString::fromStdString(v.get_str());
        }

        if (const UniValue& shares{json.find_value("shares")}; shares.isArray()) {
            for (size_t i = 0; i < shares.size(); ++i) {
                const UniValue& entry{shares[i]};
                const bool well_formed{entry.isObject() && entry.find_value("amount").isNum() &&
                                       entry.find_value("ownerAddress").isStr() &&
                                       entry.find_value("refundAddress").isStr()};
                if (!well_formed) {
                    error = Tr("Share entry %1 in the session file is malformed.").arg(i + 1);
                    return false;
                }
                Share share;
                if (const UniValue& v{entry.find_value("label")}; v.isStr()) share.label = QString::fromStdString(v.get_str());
                share.amount = entry.find_value("amount").getInt<int64_t>();
                share.ownerAddress = QString::fromStdString(entry.find_value("ownerAddress").get_str());
                share.refundAddress = QString::fromStdString(entry.find_value("refundAddress").get_str());
                if (const UniValue& v{entry.find_value("rewardAddress")}; v.isStr()) {
                    share.rewardAddress = QString::fromStdString(v.get_str());
                }
                parsed.m_shares.push_back(share);
            }
        }

        if (const UniValue& terms{json.find_value("terms")}; terms.isObject()) {
            if (const UniValue& v{terms.find_value("coreP2PAddrs")}; v.isStr()) parsed.m_terms.coreP2PAddrs = QString::fromStdString(v.get_str());
            if (const UniValue& v{terms.find_value("operatorPubKey")}; v.isStr()) parsed.m_terms.operatorPubKey = QString::fromStdString(v.get_str());
            if (const UniValue& v{terms.find_value("votingAddress")}; v.isStr()) parsed.m_terms.votingAddress = QString::fromStdString(v.get_str());
            if (const UniValue& v{terms.find_value("operatorReward")}; v.isNum()) parsed.m_terms.operatorReward = v.getInt<int>();
            if (const UniValue& v{terms.find_value("earlyPeriodBlocks")}; v.isNum()) {
                const int64_t blocks{v.getInt<int64_t>()};
                if (blocks < 0 || blocks > std::numeric_limits<uint32_t>::max()) {
                    error = Tr("The session file has an invalid early period.");
                    return false;
                }
                parsed.m_terms.earlyPeriodBlocks = static_cast<uint32_t>(blocks);
            }
            if (const UniValue& v{terms.find_value("earlyPenalty")}; v.isNum()) parsed.m_terms.earlyPenalty = v.getInt<int64_t>();
        }

        if (const UniValue& contributions{json.find_value("contributions")}; contributions.isArray()) {
            for (size_t i = 0; i < contributions.size(); ++i) {
                const UniValue& entry{contributions[i]};
                if (!entry.isObject() || !entry.find_value("label").isStr() || !entry.find_value("inputs").isArray()) {
                    error = Tr("Contribution entry %1 in the session file is malformed.").arg(i + 1);
                    return false;
                }
                Contribution contribution;
                contribution.label = QString::fromStdString(entry.find_value("label").get_str());
                const UniValue& inputs{entry.find_value("inputs")};
                for (size_t j = 0; j < inputs.size(); ++j) {
                    const UniValue& in{inputs[j]};
                    const bool well_formed{in.isObject() && in.find_value("txid").isStr() &&
                                           in.find_value("vout").isNum() && in.find_value("sequence").isNum()};
                    if (!well_formed) {
                        error = Tr("Contribution entry %1 in the session file is malformed.").arg(i + 1);
                        return false;
                    }
                    const int64_t vout{in.find_value("vout").getInt<int64_t>()};
                    const int64_t sequence{in.find_value("sequence").getInt<int64_t>()};
                    if (vout < 0 || vout > std::numeric_limits<uint32_t>::max() ||
                        sequence < 0 || sequence > std::numeric_limits<uint32_t>::max()) {
                        error = Tr("Contribution entry %1 in the session file is malformed.").arg(i + 1);
                        return false;
                    }
                    Input input;
                    input.txid = QString::fromStdString(in.find_value("txid").get_str());
                    input.vout = static_cast<uint32_t>(vout);
                    input.sequence = static_cast<uint32_t>(sequence);
                    if (!IsTxidHex(input.txid)) {
                        error = Tr("Contribution entry %1 in the session file is malformed.").arg(i + 1);
                        return false;
                    }
                    contribution.inputs.push_back(input);
                }
                if (const UniValue& change{entry.find_value("change")}; change.isObject()) {
                    if (!change.find_value("address").isStr() || !change.find_value("amount").isNum()) {
                        error = Tr("Contribution entry %1 in the session file is malformed.").arg(i + 1);
                        return false;
                    }
                    contribution.hasChange = true;
                    contribution.changeAddress = QString::fromStdString(change.find_value("address").get_str());
                    contribution.changeAmount = change.find_value("amount").getInt<int64_t>();
                    if (contribution.changeAmount < 0) {
                        error = Tr("Contribution entry %1 in the session file is malformed.").arg(i + 1);
                        return false;
                    }
                }
                if (const UniValue& v{entry.find_value("changeIndex")}; v.isNum()) contribution.changeIndex = v.getInt<int>();
                parsed.m_contributions.push_back(contribution);
            }
        }

        if (const UniValue& v{json.find_value("protx")}; v.isStr()) parsed.m_protx = QString::fromStdString(v.get_str());
        if (const UniValue& v{json.find_value("consentHash")}; v.isStr()) {
            const std::string& hash{v.get_str()};
            if (!hash.empty() && !(hash.size() == 64 && IsHex(hash))) {
                error = Tr("The session file has a malformed consent hash.");
                return false;
            }
            parsed.m_consent_hash = QString::fromStdString(hash).toLower();
        }
        if (const UniValue& v{json.find_value("collateralIndex")}; v.isNum()) parsed.m_collateral_index = v.getInt<int>();

        if (const UniValue& sigs{json.find_value("sigs")}; sigs.isArray()) {
            for (size_t i = 0; i < sigs.size(); ++i) {
                const UniValue& entry{sigs[i]};
                if (!entry.isObject() || !entry.find_value("shareIndex").isNum() || !entry.find_value("signature").isStr()) {
                    error = Tr("Signature entry %1 in the session file is malformed.").arg(i + 1);
                    return false;
                }
                Signature sig;
                sig.shareIndex = entry.find_value("shareIndex").getInt<int>();
                sig.signatureB64 = QString::fromStdString(entry.find_value("signature").get_str());
                if (std::any_of(parsed.m_sigs.begin(), parsed.m_sigs.end(),
                                [&](const Signature& s) { return s.shareIndex == sig.shareIndex; })) {
                    error = Tr("The session file lists more than one signature for share %1.").arg(sig.shareIndex + 1);
                    return false;
                }
                parsed.m_sigs.push_back(sig);
            }
        }

        if (const UniValue& v{json.find_value("prepareWallet")}; v.isStr()) parsed.m_prepare_wallet = QString::fromStdString(v.get_str());
        if (const UniValue& v{json.find_value("operatorSecretHolder")}; v.isStr()) parsed.m_operator_secret_holder = QString::fromStdString(v.get_str());

        if (parsed.m_stage != Stage::Draft) {
            if (parsed.m_protx.isEmpty() || parsed.m_consent_hash.isEmpty()) {
                error = Tr("The session file is marked \"%1\" but is missing its frozen transaction or consent hash.")
                            .arg(StageName(parsed.m_stage));
                return false;
            }
            // A frozen file must register exactly the shares/terms it displays,
            // and its prepared transaction must match its consent hash
            CMutableTransaction frozen_tx;
            CProRegTx frozen_payload;
            if (!DecodeSharedProTx(parsed.m_protx, frozen_tx, frozen_payload, error)) return false;
            if (frozen_payload.MakeSharedRegConsentHash(CTransaction(frozen_tx)) != uint256S(parsed.m_consent_hash.toStdString())) {
                error = Tr("The session file's prepared transaction does not match its consent hash. Do not use it; request a fresh copy.");
                return false;
            }
            if (!parsed.payloadMatchesEnvelope(error)) return false;
            for (const auto& sig : parsed.m_sigs) {
                if (sig.shareIndex < 0 || static_cast<size_t>(sig.shareIndex) >= parsed.m_shares.size()) {
                    error = Tr("The session file has a signature for a share that does not exist.");
                    return false;
                }
            }
        }

        *this = std::move(parsed);
        return true;
    } catch (const std::exception& e) {
        error = Tr("The session file could not be parsed: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
}

QStringList MnShareSession::validateShares() const
{
    QStringList errors;
    const CAmount required_collateral{GetMnType(MnType::Regular).collat_amount};

    if (m_shares.size() < CProRegTx::MIN_SHARES || m_shares.size() > CProRegTx::MAX_SHARES) {
        errors << Tr("A shared masternode needs between %1 and %2 shares (currently %3).")
                      .arg(CProRegTx::MIN_SHARES)
                      .arg(CProRegTx::MAX_SHARES)
                      .arg(m_shares.size());
    }

    std::optional<CKeyID> voting_key_id;
    if (CTxDestination dest{DecodeDestination(m_terms.votingAddress.toStdString())}; const auto* pkhash = std::get_if<PKHash>(&dest)) {
        voting_key_id = ToKeyID(*pkhash);
    } else {
        errors << Tr("The voting address must be a valid P2PKH address.");
    }

    // Resolve owner keys first: refund/reward scripts must not pay to any of them
    std::vector<std::optional<CKeyID>> owner_key_ids;
    std::map<CKeyID, size_t> seen_owner_keys;
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const QString row{Tr("Share %1:").arg(i + 1)};
        std::optional<CKeyID> key_id;
        if (CTxDestination dest{DecodeDestination(m_shares[i].ownerAddress.toStdString())}; const auto* pkhash = std::get_if<PKHash>(&dest)) {
            if (const CKeyID id{ToKeyID(*pkhash)}; id.IsNull()) {
                errors << row + QLatin1Char(' ') + Tr("the owner address must be a valid P2PKH address.");
            } else {
                key_id = id;
            }
        } else {
            errors << row + QLatin1Char(' ') + Tr("the owner address must be a valid P2PKH address.");
        }
        if (key_id) {
            if (const auto [it, inserted] = seen_owner_keys.emplace(*key_id, i); !inserted) {
                errors << row + QLatin1Char(' ') +
                              Tr("the owner address is already used by share %1. Every owner key must be unique.")
                                  .arg(it->second + 1);
            }
        }
        owner_key_ids.push_back(key_id);
    }

    CAmount total_amount{0};
    CAmount min_amount{std::numeric_limits<CAmount>::max()};
    std::map<CScript, size_t> seen_refund_scripts;
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const Share& share{m_shares[i]};
        const QString row{Tr("Share %1:").arg(i + 1)};

        if (share.amount < CCollateralShare::MIN_AMOUNT || share.amount > required_collateral) {
            errors << row + QLatin1Char(' ') +
                          Tr("the amount must be between %1 and %2 DASH.")
                              .arg(FormatDash(CCollateralShare::MIN_AMOUNT), FormatDash(required_collateral));
        } else {
            total_amount += share.amount;
            min_amount = std::min(min_amount, share.amount);
        }

        const bool has_reward{!share.rewardAddress.isEmpty()};
        const std::pair<const QString*, QString> payout_addrs[]{
            {&share.refundAddress, Tr("refund")},
            {&share.rewardAddress, Tr("reward")},
        };
        for (const auto& [addr, kind] : payout_addrs) {
            if (addr == &share.rewardAddress && !has_reward) continue; // empty reward = fall back to refund
            const CTxDestination dest{DecodeDestination(addr->toStdString())};
            if (!std::holds_alternative<PKHash>(dest) && !std::holds_alternative<ScriptHash>(dest)) {
                errors << row + QLatin1Char(' ') + Tr("the %1 address must be a valid P2PKH or P2SH address.").arg(kind);
                continue;
            }
            if (addr == &share.refundAddress) {
                const CScript script{GetScriptForDestination(dest)};
                if (const auto [it, inserted] = seen_refund_scripts.emplace(script, i); !inserted) {
                    errors << row + QLatin1Char(' ') +
                                  Tr("the refund address is already used by share %1. Every refund address must be unique.")
                                      .arg(it->second + 1);
                }
            }
            if (voting_key_id && dest == CTxDestination(PKHash(*voting_key_id))) {
                errors << row + QLatin1Char(' ') + Tr("the %1 address must not be the voting address.").arg(kind);
            }
            for (size_t j = 0; j < owner_key_ids.size(); ++j) {
                if (owner_key_ids[j] && dest == CTxDestination(PKHash(*owner_key_ids[j]))) {
                    errors << row + QLatin1Char(' ') +
                                  Tr("the %1 address must not be share %2's owner address.").arg(kind).arg(j + 1);
                }
            }
        }
    }

    if (!m_shares.empty() && total_amount != required_collateral) {
        errors << Tr("The share amounts must sum to exactly %1 DASH (currently %2 DASH).")
                      .arg(FormatDash(required_collateral), FormatDash(total_amount));
    }
    if (m_terms.earlyPeriodBlocks > CProRegTx::MAX_EARLY_PERIOD_BLOCKS) {
        errors << Tr("The early period must be at most %1 blocks (about two years).").arg(CProRegTx::MAX_EARLY_PERIOD_BLOCKS);
    }
    if (m_terms.earlyPenalty < 0) {
        errors << Tr("The early penalty cannot be negative.");
    } else if (min_amount != std::numeric_limits<CAmount>::max() && m_terms.earlyPenalty >= min_amount) {
        errors << Tr("The early penalty must be below the smallest share amount (%1 DASH).").arg(FormatDash(min_amount));
    }
    return errors;
}

bool MnShareSession::rebuildFundingTx(QString& error)
{
    CMutableTransaction tx;
    tx.nVersion = 2;
    for (const auto& contribution : m_contributions) {
        for (const auto& input : contribution.inputs) {
            if (!IsTxidHex(input.txid)) {
                error = Tr("Contribution \"%1\" has a malformed input transaction id.").arg(contribution.label);
                return false;
            }
            tx.vin.emplace_back(COutPoint(uint256S(input.txid.toStdString()), input.vout), CScript(), input.sequence);
        }
    }
    for (auto& contribution : m_contributions) {
        if (!contribution.hasChange) {
            contribution.changeIndex = -1;
            continue;
        }
        const CTxDestination dest{DecodeDestination(contribution.changeAddress.toStdString())};
        if (!IsValidDestination(dest)) {
            error = Tr("Contribution \"%1\" has an invalid change address.").arg(contribution.label);
            return false;
        }
        contribution.changeIndex = static_cast<int>(tx.vout.size());
        tx.vout.emplace_back(contribution.changeAmount, GetScriptForDestination(dest));
    }
    m_funding_tx = QString::fromStdString(EncodeHexTx(CTransaction(tx)));
    return true;
}

bool MnShareSession::addContribution(const Contribution& contribution, QString& error)
{
    if (m_stage != Stage::Draft) {
        error = Tr("Funding can only be changed while the session is editable. Unlock the terms first (this discards "
                   "all collected approvals).");
        return false;
    }
    if (contribution.label.isEmpty()) {
        error = Tr("The contribution needs a participant label.");
        return false;
    }
    if (contribution.inputs.empty()) {
        error = Tr("The contribution has no funding inputs.");
        return false;
    }
    for (const auto& existing : m_contributions) {
        if (existing.label == contribution.label) {
            error = Tr("\"%1\" has already contributed. Remove the existing contribution first.").arg(contribution.label);
            return false;
        }
        for (const auto& in : existing.inputs) {
            for (const auto& new_in : contribution.inputs) {
                if (in.txid.compare(new_in.txid, Qt::CaseInsensitive) == 0 && in.vout == new_in.vout) {
                    error = Tr("Input %1:%2 is already contributed by \"%3\".")
                                .arg(new_in.txid)
                                .arg(new_in.vout)
                                .arg(existing.label);
                    return false;
                }
            }
        }
    }
    if (contribution.hasChange && contribution.changeAmount <= 0) {
        error = Tr("The change amount must be positive.");
        return false;
    }

    m_contributions.push_back(contribution);
    if (!rebuildFundingTx(error)) {
        m_contributions.pop_back();
        return false;
    }
    ++m_revision;
    return true;
}

bool MnShareSession::removeContribution(const QString& label, QString& error)
{
    if (m_stage != Stage::Draft) {
        error = Tr("Funding can only be changed while the session is editable. Unlock the terms first (this discards "
                   "all collected approvals).");
        return false;
    }
    for (auto it = m_contributions.begin(); it != m_contributions.end(); ++it) {
        if (it->label == label) {
            m_contributions.erase(it);
            if (!rebuildFundingTx(error)) return false;
            ++m_revision;
            return true;
        }
    }
    error = Tr("No contribution from \"%1\" was found.").arg(label);
    return false;
}

bool MnShareSession::freeze(const QString& protx_hex, const QString& consent_hash_hex, int collateral_index, QString& error)
{
    if (m_stage != Stage::Draft) {
        error = Tr("Only an editable session can be locked.");
        return false;
    }
    CMutableTransaction tx;
    CProRegTx payload;
    if (!DecodeSharedProTx(protx_hex, tx, payload, error)) return false;
    if (!(consent_hash_hex.size() == 64 && IsHex(consent_hash_hex.toStdString()))) {
        error = Tr("The consent hash is malformed.");
        return false;
    }
    const uint256 recomputed{payload.MakeSharedRegConsentHash(CTransaction(tx))};
    if (recomputed != uint256S(consent_hash_hex.toStdString())) {
        error = Tr("The consent hash does not match the prepared transaction. Do not sign; restart the preparation step.");
        return false;
    }
    if (collateral_index < 0 || payload.collateralOutpoint.n != static_cast<uint32_t>(collateral_index)) {
        error = Tr("The collateral output index does not match the prepared transaction.");
        return false;
    }
    // The share table and terms the user reviewed must be exactly what the
    // prepared transaction registers, otherwise the displayed session lies
    m_protx = protx_hex;
    if (!payloadMatchesEnvelope(error)) {
        m_protx.clear();
        return false;
    }
    m_consent_hash = consent_hash_hex.toLower();
    m_collateral_index = collateral_index;
    m_sigs.clear();
    m_stage = Stage::Frozen;
    return true;
}

void MnShareSession::unfreeze()
{
    m_protx.clear();
    m_consent_hash.clear();
    m_collateral_index = -1;
    m_sigs.clear();
    m_stage = Stage::Draft;
    // Signatures collected for the old consent hash can never apply to the
    // reworked session; a new revision makes every circulating copy stale
    ++m_revision;
}

bool MnShareSession::verifySignature(int share_index, const QString& sig_b64, QString& error) const
{
    if (m_protx.isEmpty() || m_consent_hash.isEmpty()) {
        error = Tr("The terms are not locked yet; there is nothing to approve.");
        return false;
    }
    CMutableTransaction tx;
    CProRegTx payload;
    if (!DecodeSharedProTx(m_protx, tx, payload, error)) return false;
    if (share_index < 0 || static_cast<size_t>(share_index) >= payload.shares.size()) {
        error = Tr("Share index %1 is out of range.").arg(share_index);
        return false;
    }
    const uint256 consent_hash{payload.MakeSharedRegConsentHash(CTransaction(tx))};
    if (consent_hash != uint256S(m_consent_hash.toStdString())) {
        error = Tr("The session file is internally inconsistent: its prepared transaction no longer matches its consent hash. Do not sign; request a fresh copy.");
        return false;
    }
    // A signature over the payload digest is meaningless if the payload is not
    // the share table the participant reviewed
    if (!payloadMatchesEnvelope(error)) return false;
    const auto sig{DecodeBase64(sig_b64.toStdString())};
    if (!sig) {
        error = Tr("The signature is not valid base64.");
        return false;
    }
    const QString label{static_cast<size_t>(share_index) < m_shares.size() && !m_shares[share_index].label.isEmpty()
                            ? m_shares[share_index].label
                            : Tr("share %1").arg(share_index + 1)};
    if (std::string str_error; !CHashSigner::VerifyHashCanonical(consent_hash, payload.shares[share_index].keyIDOwner, *sig, str_error)) {
        error = Tr("The signature from %1 does not verify against the current terms. It was most likely made for an older version of this session — ask %1 to re-sign the current version.")
                    .arg(label);
        return false;
    }
    return true;
}

std::vector<MnShareSession::SignatureCheck> MnShareSession::verifyAllSignatures() const
{
    std::vector<SignatureCheck> ret;
    ret.reserve(m_sigs.size());
    for (const auto& sig : m_sigs) {
        SignatureCheck check;
        check.shareIndex = sig.shareIndex;
        check.valid = verifySignature(sig.shareIndex, sig.signatureB64, check.error);
        ret.push_back(check);
    }
    return ret;
}

const MnShareSession::Signature* MnShareSession::findSignature(int share_index) const
{
    for (const auto& sig : m_sigs) {
        if (sig.shareIndex == share_index) return &sig;
    }
    return nullptr;
}

bool MnShareSession::payloadMatchesEnvelope(QString& error) const
{
    CMutableTransaction tx;
    CProRegTx payload;
    if (!DecodeSharedProTx(m_protx, tx, payload, error)) return false;

    if (payload.shares.size() != m_shares.size()) {
        error = Tr("The session file's share table does not match its prepared transaction. Do not sign; request a fresh copy.");
        return false;
    }
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const Share& share{m_shares[i]};
        const CCollateralShare& signed_share{payload.shares[i]};

        const CTxDestination owner{DecodeDestination(share.ownerAddress.toStdString())};
        const auto* owner_pkhash{std::get_if<PKHash>(&owner)};
        if (!owner_pkhash || ToKeyID(*owner_pkhash) != signed_share.keyIDOwner ||
            share.amount != signed_share.amount ||
            GetScriptForDestination(DecodeDestination(share.refundAddress.toStdString())) != signed_share.scriptRefund) {
            error = Tr("Share %1 in the session file does not match its prepared transaction. Do not sign; request a fresh copy.").arg(i + 1);
            return false;
        }
        const CScript expected_reward{share.rewardAddress.isEmpty()
                                          ? CScript()
                                          : GetScriptForDestination(DecodeDestination(share.rewardAddress.toStdString()))};
        if (expected_reward != signed_share.scriptReward) {
            error = Tr("Share %1's reward address in the session file does not match its prepared transaction. Do not sign; request a fresh copy.").arg(i + 1);
            return false;
        }
    }

    const CTxDestination voting{DecodeDestination(m_terms.votingAddress.toStdString())};
    const auto* voting_pkhash{std::get_if<PKHash>(&voting)};
    const bool operator_matches{QString::fromStdString(payload.pubKeyOperator.Get().ToString(/*specificLegacyScheme=*/false))
                                    .compare(m_terms.operatorPubKey, Qt::CaseInsensitive) == 0};
    if (!voting_pkhash || ToKeyID(*voting_pkhash) != payload.keyIDVoting ||
        !operator_matches ||
        m_terms.operatorReward != payload.nOperatorReward ||
        m_terms.earlyPeriodBlocks != payload.nEarlyPeriodBlocks ||
        m_terms.earlyPenalty != payload.nEarlyPenalty) {
        error = Tr("The terms shown in the session file do not match its prepared transaction. Do not sign; request a fresh copy.");
        return false;
    }
    return true;
}

bool MnShareSession::sameDraftState(const MnShareSession& other) const
{
    if (m_funding_tx != other.m_funding_tx) return false;
    if (m_shares.size() != other.m_shares.size()) return false;
    for (size_t i = 0; i < m_shares.size(); ++i) {
        const Share& a{m_shares[i]};
        const Share& b{other.m_shares[i]};
        // Labels are local annotations, not consensus terms; ignore them
        if (a.amount != b.amount || a.ownerAddress != b.ownerAddress ||
            a.refundAddress != b.refundAddress || a.rewardAddress != b.rewardAddress) {
            return false;
        }
    }
    return m_terms.coreP2PAddrs == other.m_terms.coreP2PAddrs &&
           m_terms.operatorPubKey == other.m_terms.operatorPubKey &&
           m_terms.votingAddress == other.m_terms.votingAddress &&
           m_terms.operatorReward == other.m_terms.operatorReward &&
           m_terms.earlyPeriodBlocks == other.m_terms.earlyPeriodBlocks &&
           m_terms.earlyPenalty == other.m_terms.earlyPenalty;
}

bool MnShareSession::addSignature(int share_index, const QString& sig_b64, QString& error)
{
    if (!verifySignature(share_index, sig_b64, error)) return false;
    if (const Signature* existing = findSignature(share_index)) {
        // RFC6979 signatures are deterministic: the same key signing the same
        // digest always yields the same bytes, so a byte-identical duplicate
        // is a harmless re-import and anything else signed a stale version
        if (DecodeBase64(existing->signatureB64.toStdString()) == DecodeBase64(sig_b64.toStdString())) {
            return true;
        }
        error = Tr("A different signature for this share is already recorded. One of the copies signed a stale version of the session — agree on the latest revision and have it re-signed.");
        return false;
    }
    Signature sig;
    sig.shareIndex = share_index;
    sig.signatureB64 = sig_b64;
    const auto pos = std::find_if(m_sigs.begin(), m_sigs.end(),
                                  [share_index](const Signature& s) { return s.shareIndex > share_index; });
    m_sigs.insert(pos, sig);
    if (m_stage == Stage::Frozen) m_stage = Stage::Signing;
    return true;
}

int MnShareSession::signedCount() const
{
    int count{0};
    for (const auto& sig : m_sigs) {
        if (sig.shareIndex >= 0 && static_cast<size_t>(sig.shareIndex) < m_shares.size()) ++count;
    }
    return count;
}

std::vector<int> MnShareSession::missingIndexes() const
{
    std::vector<int> ret;
    for (size_t i = 0; i < m_shares.size(); ++i) {
        if (!findSignature(static_cast<int>(i))) ret.push_back(static_cast<int>(i));
    }
    return ret;
}

QString MnShareSession::signatureFor(int share_index) const
{
    const Signature* sig{findSignature(share_index)};
    return sig ? sig->signatureB64 : QString();
}

MnShareSession::MergeResult MnShareSession::mergeEnvelope(const MnShareSession& other, QString& error)
{
    if (other.m_session_id != m_session_id) {
        error = Tr("The imported file belongs to a different session (id %1, this session is %2).")
                    .arg(other.m_session_id.left(8), m_session_id.left(8));
        return MergeResult::Conflict;
    }
    if (other.m_revision > m_revision) {
        error = Tr("The imported copy is revision %1 of this session; this copy is revision %2 and is superseded by it.")
                    .arg(other.m_revision)
                    .arg(m_revision);
        return MergeResult::OtherNewer;
    }
    if (other.m_revision < m_revision) {
        error = Tr("The imported copy is revision %1 of this session; this copy is already at revision %2. Send the sender the newer file.")
                    .arg(other.m_revision)
                    .arg(m_revision);
        return MergeResult::OtherOlder;
    }

    const bool frozen{!m_consent_hash.isEmpty()};
    const bool other_frozen{!other.m_consent_hash.isEmpty()};
    if (frozen != other_frozen) {
        // Freezing does not bump the revision, so a frozen copy supersedes a
        // draft copy of the same revision
        error = other_frozen ? Tr("The imported copy has locked terms and supersedes this editable copy.")
                             : Tr("This session's terms are already locked; the imported copy is still editable. Send "
                                  "the sender the newer update.");
        return other_frozen ? MergeResult::OtherNewer : MergeResult::OtherOlder;
    }
    if (frozen && m_consent_hash.compare(other.m_consent_hash, Qt::CaseInsensitive) != 0) {
        error = Tr("Both copies claim revision %1 but locked different terms (check phrase %2 vs %3). They cannot be "
                   "merged — agree on one authoritative copy, bump its revision and re-circulate it.")
                    .arg(m_revision)
                    .arg(consentCheckPhrase(), other.consentCheckPhrase());
        return MergeResult::Conflict;
    }
    if (!frozen) {
        if (!sameDraftState(other)) {
            error = Tr("Both copies claim revision %1 but their share tables, terms or funding differ. They cannot be merged — whoever changed the draft must bump the revision and re-circulate the file.")
                        .arg(m_revision);
            return MergeResult::Conflict;
        }
        return MergeResult::Merged;
    }

    // Same frozen terms: absorb the union of the other copy's verified signatures
    QStringList warnings;
    for (const auto& sig : other.m_sigs) {
        if (QString sig_error; !addSignature(sig.shareIndex, sig.signatureB64, sig_error)) {
            warnings << sig_error;
        }
    }

    // Contributors sign the same Combined transaction independently. Merge
    // those copies input-by-input so this is one parallel signing round,
    // rather than forcing participants to pass one partially signed copy
    // around serially. Nothing except scriptSig may differ between copies.
    if (m_stage == Stage::Combined && other.m_stage == Stage::Combined) {
        CMutableTransaction ours;
        CMutableTransaction theirs;
        CProRegTx ours_payload;
        CProRegTx theirs_payload;
        QString decode_error;
        if (!DecodeSharedProTx(m_protx, ours, ours_payload, decode_error) ||
            !DecodeSharedProTx(other.m_protx, theirs, theirs_payload, decode_error)) {
            error = Tr("A funding-signed copy contains an invalid registration transaction. It was not merged.");
            return MergeResult::Conflict;
        }
        if (ours_payload.MakeSharedRegConsentHash(CTransaction(ours)) != uint256S(m_consent_hash.toStdString()) ||
            theirs_payload.MakeSharedRegConsentHash(CTransaction(theirs)) != uint256S(m_consent_hash.toStdString())) {
            error = Tr("A funding-signed copy does not match the terms everyone approved. It was not merged.");
            return MergeResult::Conflict;
        }

        CMutableTransaction unsigned_ours{ours};
        CMutableTransaction unsigned_theirs{theirs};
        for (auto& input : unsigned_ours.vin)
            input.scriptSig.clear();
        for (auto& input : unsigned_theirs.vin)
            input.scriptSig.clear();
        if (EncodeHexTx(CTransaction(unsigned_ours)) != EncodeHexTx(CTransaction(unsigned_theirs))) {
            error = Tr("The funding-signed copies are not the same transaction. They cannot be merged.");
            return MergeResult::Conflict;
        }

        for (size_t i = 0; i < ours.vin.size(); ++i) {
            const CScript& incoming{theirs.vin[i].scriptSig};
            if (incoming.empty()) continue;
            if (!ours.vin[i].scriptSig.empty() && ours.vin[i].scriptSig != incoming) {
                error = Tr("Funding input %1 has two different signatures. The copies cannot be merged.").arg(i + 1);
                return MergeResult::Conflict;
            }
            ours.vin[i].scriptSig = incoming;
        }
        m_protx = QString::fromStdString(EncodeHexTx(CTransaction(ours)));
        if (std::all_of(ours.vin.begin(), ours.vin.end(), [](const CTxIn& input) { return !input.scriptSig.empty(); })) {
            m_stage = Stage::FundingSigned;
        }
    }

    // Adopt a further-advanced stage together with its transaction (the protx
    // field evolves along the stages: combined join sigs, then funding
    // signatures live in the same transaction hex). Validate the incoming
    // transaction the same way the direct setters do so a tampered "advanced"
    // copy can neither overwrite a good protx nor fake completion.
    if (other.m_stage > m_stage) {
        CMutableTransaction tx;
        CProRegTx payload;
        QString decode_error;
        if (!DecodeSharedProTx(other.m_protx, tx, payload, decode_error) ||
            payload.MakeSharedRegConsentHash(CTransaction(tx)) != uint256S(m_consent_hash.toStdString())) {
            error = Tr("The imported copy claims to be further along but its transaction does not match the terms everyone signed. It was not adopted.");
            return MergeResult::Conflict;
        }
        m_stage = other.m_stage;
        m_protx = other.m_protx;
    }
    error = warnings.join(QLatin1Char('\n'));
    return MergeResult::Merged;
}

bool MnShareSession::setCombinedTx(const QString& tx_hex, QString& error)
{
    if (m_stage != Stage::Frozen && m_stage != Stage::Signing) {
        error = Tr("The session is not collecting approvals.");
        return false;
    }
    if (signedCount() < static_cast<int>(m_shares.size())) {
        error = Tr("Not every share has signed yet (%1 of %2). Combine only a fully signed session.")
                    .arg(signedCount())
                    .arg(m_shares.size());
        return false;
    }
    CMutableTransaction tx;
    CProRegTx payload;
    if (!DecodeSharedProTx(tx_hex, tx, payload, error)) return false;
    // Combining embeds the join signatures into the payload; everything the
    // consent digest covers must be unchanged
    if (payload.MakeSharedRegConsentHash(CTransaction(tx)) != uint256S(m_consent_hash.toStdString())) {
        error = Tr("The combined transaction does not match the terms everyone signed. Do not broadcast it; restart from the prepared transaction.");
        return false;
    }
    m_protx = tx_hex;
    m_stage = Stage::Combined;
    return true;
}

bool MnShareSession::setFundingSignedTx(const QString& tx_hex, QString& error)
{
    if (m_stage != Stage::Combined && m_stage != Stage::FundingSigned) {
        error = Tr("The owner approvals have to be combined before the contributions are signed.");
        return false;
    }
    CMutableTransaction tx;
    CProRegTx payload;
    if (!DecodeSharedProTx(tx_hex, tx, payload, error)) return false;
    if (payload.MakeSharedRegConsentHash(CTransaction(tx)) != uint256S(m_consent_hash.toStdString())) {
        error = Tr("The signed transaction does not match the terms everyone signed. Do not broadcast it.");
        return false;
    }
    int unsigned_inputs{0};
    for (const auto& in : tx.vin) {
        if (in.scriptSig.empty()) ++unsigned_inputs;
    }
    if (unsigned_inputs > 0) {
        error = Tr("%n funding input(s) are still unsigned.", nullptr, unsigned_inputs);
        return false;
    }
    m_protx = tx_hex;
    m_stage = Stage::FundingSigned;
    return true;
}

bool MnShareSession::setBroadcast(QString& error)
{
    if (m_stage != Stage::FundingSigned) {
        error = Tr("The session cannot be broadcast before every consent and funding signature is in place.");
        return false;
    }
    m_stage = Stage::Broadcast;
    return true;
}

QString MnShareSession::consentCheckPhrase() const
{
    return m_consent_hash.left(8).toUpper();
}

QString MnShareSession::HumanEarlyPeriod(uint32_t blocks)
{
    const qint64 minutes{static_cast<qint64>(blocks) * 5 / 2}; // 2.5-minute blocks
    if (minutes < 120) {
        return Tr("about %n minute(s)", nullptr, static_cast<int>(minutes));
    }
    if (minutes < 72 * 60) {
        return Tr("about %n hour(s)", nullptr, static_cast<int>(minutes / 60));
    }
    return Tr("about %n day(s)", nullptr, static_cast<int>((minutes + 720) / 1440));
}

MnShareSession::PenaltyPreview MnShareSession::penaltyPreview(int actor_index, CAmount fee, int at_height,
                                                              int registered_height) const
{
    std::vector<CAmount> amounts;
    amounts.reserve(m_shares.size());
    for (const auto& share : m_shares) {
        amounts.push_back(share.amount);
    }
    return PenaltyPreviewFor(amounts, actor_index, m_terms.earlyPenalty, m_terms.earlyPeriodBlocks, fee, at_height,
                             registered_height);
}

MnShareSession::PenaltyPreview MnShareSession::PenaltyPreviewFor(const std::vector<CAmount>& share_amounts,
                                                                 int actor_index, CAmount early_penalty,
                                                                 uint32_t early_period_blocks, CAmount fee,
                                                                 int at_height, int registered_height)
{
    PenaltyPreview preview;
    if (actor_index < 0 || static_cast<size_t>(actor_index) >= share_amounts.size()) {
        preview.error = Tr("Share index %1 is out of range.").arg(actor_index);
        return preview;
    }
    if (fee <= 0) {
        preview.error = Tr("The fee must be positive.");
        return preview;
    }

    // Mirrors the early-period decision in "protx dissolve": the transaction
    // would confirm at at_height + 1
    preview.penaltyFreeHeight = registered_height + static_cast<int>(early_period_blocks);
    preview.early = static_cast<int64_t>(at_height) + 1 - registered_height < static_cast<int64_t>(early_period_blocks);
    preview.penalty = preview.early ? early_penalty : 0;

    const CAmount actor_output{share_amounts[actor_index] - preview.penalty - fee};
    if (actor_output < 0) {
        preview.error = Tr("The penalty and fee exceed the dissolving participant's share.");
        return preview;
    }

    // Mirrors BuildProDisTx: pro-rata bonus with sequential floor, remainder
    // to the last non-actor share, so the payouts always sum exactly
    CAmount non_actor_total{0};
    size_t last_non_actor{0};
    bool have_non_actor{false};
    for (size_t i = 0; i < share_amounts.size(); ++i) {
        if (static_cast<int>(i) != actor_index) {
            non_actor_total += share_amounts[i];
            last_non_actor = i;
            have_non_actor = true;
        }
    }
    // A valid shared masternode always has at least two shares, but guard the
    // division anyway so a malformed table cannot divide by zero
    if (!have_non_actor || non_actor_total <= 0) {
        preview.error = Tr("A unilateral dissolution needs at least one other share to receive the penalty.");
        return preview;
    }
    preview.payouts.assign(share_amounts.size(), 0);
    CAmount distributed{0};
    for (size_t i = 0; i < share_amounts.size(); ++i) {
        if (static_cast<int>(i) == actor_index) {
            preview.payouts[i] = actor_output;
            continue;
        }
        CAmount bonus;
        if (i == last_non_actor) {
            bonus = preview.penalty - distributed;
        } else {
            arith_uint256 v{static_cast<uint64_t>(preview.penalty)};
            v *= arith_uint256{static_cast<uint64_t>(share_amounts[i])};
            v /= arith_uint256{static_cast<uint64_t>(non_actor_total)};
            bonus = static_cast<CAmount>(v.GetLow64());
        }
        distributed += bonus;
        preview.payouts[i] = share_amounts[i] + bonus;
    }
    preview.valid = true;
    return preview;
}
