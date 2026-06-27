// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INSTANTSEND_LOCK_H
#define BITCOIN_INSTANTSEND_LOCK_H

#include <bls/bls.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <vector>

class COutPoint;

namespace instantsend {
struct InstantSendLock {
    static constexpr uint8_t CURRENT_VERSION{1};
    // An islock mirrors the vin of the locked transaction. A consensus-valid Dash
    // transaction must fit in a 2 MB block and each input is at least 41 bytes on
    // the wire, so no valid transaction can have more than ~48,780 inputs. This
    // ceiling can therefore never reject a valid islock, but it bounds the per-message
    // hashing/dedup work and the size of each retained pending entry, preventing a
    // ~3 MiB amplification per islock (codex/F-008, codex/F-028, codex/F-038).
    static constexpr size_t MAX_INPUTS{50000};

    uint8_t nVersion{CURRENT_VERSION};
    std::vector<COutPoint> inputs;
    uint256 txid;
    uint256 cycleHash;
    CBLSLazySignature sig;

    InstantSendLock() = default;

    SERIALIZE_METHODS(InstantSendLock, obj)
    {
        READWRITE(obj.nVersion);
        READWRITE(obj.inputs);
        READWRITE(obj.txid);
        READWRITE(obj.cycleHash);
        READWRITE(obj.sig);
    }

    uint256 GetRequestId() const;
    bool TriviallyValid() const;
};

uint256 GenInputLockRequestId(const COutPoint& outpoint);

using InstantSendLockPtr = std::shared_ptr<InstantSendLock>;
} // namespace instantsend

#endif // BITCOIN_INSTANTSEND_LOCK_H
