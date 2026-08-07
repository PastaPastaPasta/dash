// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_FFI_SIGNER_H
#define BITCOIN_PLATFORM_FFI_SIGNER_H

#include <rust/cxx.h>

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace platform_ffi {

//! Wallet-backed signer handed by reference into the Rust state-transition
//! builders (rust/platform). Rust calls SignDigestForKey with the id of the
//! identity key being signed (or ASSET_LOCK_KEY_ID for the one-time
//! asset-lock key of an identity registration) and the 32-byte double-SHA256
//! digest of the transition's signable bytes; the callback must return a
//! 65-byte compact recoverable ECDSA signature. Private keys never cross
//! the FFI boundary.
class WalletSigner
{
public:
    //! Matches rust/platform st::ASSET_LOCK_KEY_ID (u32::MAX).
    static constexpr uint32_t ASSET_LOCK_KEY_ID{0xffffffff};

    using SignFn = std::function<bool(uint32_t key_id, const std::array<uint8_t, 32>& digest,
                                      std::vector<uint8_t>& sig_out)>;

    explicit WalletSigner(SignFn sign_fn) : m_sign_fn(std::move(sign_fn)) {}

    bool SignDigestForKey(uint32_t key_id, rust::Slice<const uint8_t> digest,
                          rust::Vec<uint8_t>& sig_out) const
    {
        if (!m_sign_fn || digest.size() != 32) return false;
        std::array<uint8_t, 32> digest_array;
        std::copy(digest.begin(), digest.end(), digest_array.begin());
        std::vector<uint8_t> signature;
        if (!m_sign_fn(key_id, digest_array, signature)) return false;
        sig_out.clear();
        sig_out.reserve(signature.size());
        for (const uint8_t byte : signature) sig_out.push_back(byte);
        return true;
    }

private:
    SignFn m_sign_fn;
};

} // namespace platform_ffi

#endif // BITCOIN_PLATFORM_FFI_SIGNER_H
