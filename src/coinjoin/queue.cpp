// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/queue.h>

#include <bls/bls.h>
#include <logging.h>
#include <masternode/node.h>
#include <span.h>
#include <tinyformat.h>
#include <timedata.h>

#include <version.h>

uint256 CCoinJoinQueue::GetSignatureHash() const
{
    return SerializeHash(*this, SER_GETHASH, PROTOCOL_VERSION);
}

uint256 CCoinJoinQueue::GetHash() const
{
    return SerializeHash(*this, SER_NETWORK, PROTOCOL_VERSION);
}

bool CCoinJoinQueue::Sign(const CActiveMasternodeManager& mn_activeman)
{
    uint256 hash = GetSignatureHash();
    CBLSSignature sig = mn_activeman.Sign(hash, /*is_legacy=*/ false);
    if (!sig.IsValid()) {
        return false;
    }
    vchSig = sig.ToByteVector(false);
    return true;
}

bool CCoinJoinQueue::CheckSignature(const CBLSPublicKey& blsPubKey) const
{
    if (!CBLSSignature(Span{vchSig}, false).VerifyInsecure(blsPubKey, GetSignatureHash(), false)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinQueue::CheckSignature -- VerifyInsecure() failed\n");
        return false;
    }
    return true;
}

bool CCoinJoinQueue::IsTimeOutOfBounds(int64_t current_time) const
{
    return current_time - nTime > COINJOIN_QUEUE_TIMEOUT ||
           nTime - current_time > COINJOIN_QUEUE_TIMEOUT;
}

bool CCoinJoinQueue::IsTimeOutOfBounds() const
{
    return IsTimeOutOfBounds(GetAdjustedTime());
}

std::string CCoinJoinQueue::ToString() const
{
    return strprintf("nDenom=%d, nTime=%lld, fReady=%s, fTried=%s, masternode=%s",
        nDenom, nTime, fReady ? "true" : "false", fTried ? "true" : "false", masternodeOutpoint.ToStringShort());
}


