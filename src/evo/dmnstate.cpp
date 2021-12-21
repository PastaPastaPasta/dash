#include <evo/dmnstate.h>
#include <evo/simplifiedmns.h>
#include <evo/providertx.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <script/standard.h>
#include <validationinterface.h>

#include <univalue.h>
#include "messagesigner.h"


CDeterministicMNState::CDeterministicMNState(const CProRegTx& proTx)
{
    keyIDOwner = proTx.keyIDOwner;
    pubKeyOperator.Set(proTx.pubKeyOperator);
    keyIDVoting = proTx.keyIDVoting;
    addr = proTx.addr;
    scriptPayout = proTx.scriptPayout;
}

void CDeterministicMNState::ResetOperatorFields()
{
    pubKeyOperator.Set(CBLSPublicKey());
    addr = CService();
    scriptOperatorPayout = CScript();
    nRevocationReason = CProUpRevTx::REASON_NOT_SPECIFIED;
}

void CDeterministicMNState::BanIfNotBanned(int height)
{
    if (!IsBanned()) {
        nPoSeBanHeight = height;
    }
}

int CDeterministicMNState::GetBannedHeight() const
{
    return nPoSeBanHeight;
}
bool CDeterministicMNState::IsBanned() const
{
    return nPoSeBanHeight != -1;
}
void CDeterministicMNState::Revive(int nRevivedHeight)
{
    nPoSePenalty = 0;
    nPoSeBanHeight = -1;
    nPoSeRevivedHeight = nRevivedHeight;
}
void CDeterministicMNState::UpdateConfirmedHash(const uint256& _proTxHash, const uint256& _confirmedHash)
{
    confirmedHash = _confirmedHash;
    CSHA256 h;
    h.Write(_proTxHash.begin(), _proTxHash.size());
    h.Write(_confirmedHash.begin(), _confirmedHash.size());
    h.Finalize(confirmedHashWithProRegTxHash.begin());
}

std::string CDeterministicMNState::ToString() const
{
    CTxDestination dest;
    std::string payoutAddress = "unknown";
    std::string operatorPayoutAddress = "none";
    if (ExtractDestination(scriptPayout, dest)) {
        payoutAddress = EncodeDestination(dest);
    }
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        operatorPayoutAddress = EncodeDestination(dest);
    }

    return strprintf("CDeterministicMNState(nRegisteredHeight=%d, nLastPaidHeight=%d, nPoSePenalty=%d, nPoSeRevivedHeight=%d, nPoSeBanHeight=%d, nRevocationReason=%d, "
                     "ownerAddress=%s, pubKeyOperator=%s, votingAddress=%s, addr=%s, payoutAddress=%s, operatorPayoutAddress=%s)",
                     nRegisteredHeight, nLastPaidHeight, nPoSePenalty, nPoSeRevivedHeight, nPoSeBanHeight, nRevocationReason,
                     EncodeDestination(keyIDOwner), pubKeyOperator.Get().ToString(), EncodeDestination(keyIDVoting), addr.ToStringIPPort(false), payoutAddress, operatorPayoutAddress);
}

void CDeterministicMNState::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.pushKV("service", addr.ToStringIPPort(false));
    obj.pushKV("registeredHeight", nRegisteredHeight);
    obj.pushKV("lastPaidHeight", nLastPaidHeight);
    obj.pushKV("PoSePenalty", nPoSePenalty);
    obj.pushKV("PoSeRevivedHeight", nPoSeRevivedHeight);
    obj.pushKV("PoSeBanHeight", nPoSeBanHeight);
    obj.pushKV("revocationReason", nRevocationReason);
    obj.pushKV("ownerAddress", EncodeDestination(keyIDOwner));
    obj.pushKV("votingAddress", EncodeDestination(keyIDVoting));

    CTxDestination dest;
    if (ExtractDestination(scriptPayout, dest)) {
        obj.pushKV("payoutAddress", EncodeDestination(dest));
    }
    obj.pushKV("pubKeyOperator", pubKeyOperator.Get().ToString());
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        obj.pushKV("operatorPayoutAddress", EncodeDestination(dest));
    }
}

void CDeterministicMNStateDiff::ApplyToState(CDeterministicMNState& target) const
{
#define DMN_STATE_DIFF_LINE(f) if (fields & Field_##f) target.f = state.f;
    DMN_STATE_DIFF_ALL_FIELDS
#undef DMN_STATE_DIFF_LINE
}

CDeterministicMNStateDiff::CDeterministicMNStateDiff(const CDeterministicMNState& a, const CDeterministicMNState& b)
{
#define DMN_STATE_DIFF_LINE(f) if (a.f != b.f) { state.f = b.f; fields |= Field_##f; }
    DMN_STATE_DIFF_ALL_FIELDS
#undef DMN_STATE_DIFF_LINE
}