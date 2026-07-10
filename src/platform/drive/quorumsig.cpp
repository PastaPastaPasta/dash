// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/drive/quorumsig.h>

#include <bls/bls.h>
#include <crypto/sha256.h>
#include <uint256.h>

#include <algorithm>
#include <cstring>

namespace platform::drive {
namespace {

Hash256 Sha256(Span<const uint8_t> data)
{
    Hash256 out{};
    CSHA256().Write(data.data(), data.size()).Finalize(out.data());
    return out;
}

Hash256 DoubleSha256(Span<const uint8_t> data)
{
    return Sha256(Span<const uint8_t>(Sha256(data)));
}

void AppendU32LE(Bytes& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void AppendU64LE(Bytes& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void AppendI32LE(Bytes& out, int32_t v) { AppendU32LE(out, static_cast<uint32_t>(v)); }
void AppendI64LE(Bytes& out, int64_t v) { AppendU64LE(out, static_cast<uint64_t>(v)); }

//! protobuf base-128 varint (unsigned LEB128).
void AppendProtoVarint(Bytes& out, uint64_t v) { WriteLEB128(out, v); }

Hash256 Reversed(const Hash256& in)
{
    Hash256 out{};
    for (size_t i = 0; i < in.size(); ++i) out[i] = in[in.size() - 1 - i];
    return out;
}

} // namespace

Hash256 ComputeRequestId(uint64_t height, int32_t round)
{
    static const char kPrefix[] = "dpbvote"; // 7 bytes, no NUL
    Bytes buf(kPrefix, kPrefix + 7);
    AppendI64LE(buf, static_cast<int64_t>(height));
    AppendI32LE(buf, round);
    return Sha256(Span<const uint8_t>(buf));
}

Bytes EncodeStateIdSignBytes(const Hash256& app_hash, const BlockContext& ctx, uint64_t height)
{
    // Field body in ascending tag order; proto3 omits default (zero/empty)
    // scalar/bytes fields (prost behaviour, matching tenderdash's generated
    // StateId).
    Bytes body;
    if (ctx.protocol_version != 0) {
        body.push_back(0x09); // field 1, wire type 1 (64-bit)
        AppendU64LE(body, ctx.protocol_version);
    }
    if (height != 0) {
        body.push_back(0x11); // field 2, wire type 1
        AppendU64LE(body, height);
    }
    // app_hash is a 32-byte value here, never the empty default -> always emitted.
    {
        body.push_back(0x1a); // field 3, wire type 2 (length-delimited)
        AppendProtoVarint(body, app_hash.size());
        body.insert(body.end(), app_hash.begin(), app_hash.end());
    }
    if (ctx.core_chain_locked_height != 0) {
        body.push_back(0x25); // field 4, wire type 5 (32-bit)
        AppendU32LE(body, ctx.core_chain_locked_height);
    }
    if (ctx.time_ms != 0) {
        body.push_back(0x29); // field 5, wire type 1
        AppendU64LE(body, ctx.time_ms);
    }
    // encode_length_delimited: varint message length prefix + body.
    Bytes out;
    AppendProtoVarint(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes EncodeCanonicalVoteSignBytes(int32_t vote_type, uint64_t height, int32_t round,
                                   const Hash256& block_id_hash, const Hash256& state_id_hash,
                                   const std::string& chain_id)
{
    Bytes buf;
    AppendI32LE(buf, vote_type);
    AppendI64LE(buf, static_cast<int64_t>(height));
    AppendI64LE(buf, static_cast<int64_t>(round));
    buf.insert(buf.end(), block_id_hash.begin(), block_id_hash.end());
    buf.insert(buf.end(), state_id_hash.begin(), state_id_hash.end());
    buf.insert(buf.end(), chain_id.begin(), chain_id.end());
    return buf;
}

Hash256 ComputeSignHash(uint8_t quorum_type, const Hash256& quorum_hash, const Hash256& request_id,
                        const Hash256& vote_hash)
{
    const Hash256 qh{Reversed(quorum_hash)};
    const Hash256 rid{Reversed(request_id)};
    const Hash256 vh{Reversed(vote_hash)};
    Bytes buf;
    buf.push_back(quorum_type);
    buf.insert(buf.end(), qh.begin(), qh.end());
    buf.insert(buf.end(), rid.begin(), rid.end());
    buf.insert(buf.end(), vh.begin(), vh.end());
    return DoubleSha256(Span<const uint8_t>(buf));
}

// SignedMsgType::Precommit
static constexpr int32_t kSignedMsgTypePrecommit = 2;

SignDigestIntermediates ComputeSignDigest(const Hash256& app_hash, const ProofEnvelope& envelope,
                                          const BlockContext& ctx)
{
    SignDigestIntermediates out;
    out.request_id = ComputeRequestId(ctx.height, envelope.round);
    out.state_id_bytes = EncodeStateIdSignBytes(app_hash, ctx, ctx.height);
    out.state_id_hash = Sha256(Span<const uint8_t>(out.state_id_bytes));
    out.canonical_vote_bytes = EncodeCanonicalVoteSignBytes(
        kSignedMsgTypePrecommit, ctx.height, envelope.round, envelope.block_id_hash,
        out.state_id_hash, ctx.chain_id);
    out.vote_hash = Sha256(Span<const uint8_t>(out.canonical_vote_bytes));
    out.sign_digest =
        ComputeSignHash(envelope.quorum_type, envelope.quorum_hash, out.request_id, out.vote_hash);
    return out;
}

bool VerifyQuorumSig(const Hash256& app_hash, const ProofEnvelope& envelope,
                     const BlockContext& ctx, const std::vector<uint8_t>& quorum_pubkey,
                     std::string& error, SignDigestIntermediates* out_digest)
{
    if (quorum_pubkey.size() != 48) {
        error = "quorum public key must be 48 bytes (basic BLS scheme)";
        return false;
    }
    // An all-zero signature is never valid; reject it explicitly to match
    // rs-drive-proof-verifier::verify_signature_digest.
    if (std::ranges::all_of(envelope.signature, [](uint8_t c) { return c == 0; })) {
        error = "empty signature";
        return false;
    }

    const SignDigestIntermediates inter{ComputeSignDigest(app_hash, envelope, ctx)};
    if (out_digest != nullptr) *out_digest = inter;

    CBLSPublicKey pubkey;
    pubkey.SetBytes(Span<const uint8_t>(quorum_pubkey), /*specificLegacyScheme=*/false);
    if (!pubkey.IsValid()) {
        error = "invalid quorum public key";
        return false;
    }

    CBLSSignature sig;
    sig.SetBytes(Span<const uint8_t>(envelope.signature), /*specificLegacyScheme=*/false);
    if (!sig.IsValid()) {
        error = "invalid signature encoding";
        return false;
    }

    uint256 digest;
    std::memcpy(digest.begin(), inter.sign_digest.data(), inter.sign_digest.size());
    if (!sig.VerifyInsecure(pubkey, digest, /*specificLegacyScheme=*/false)) {
        error = "quorum signature verification failed";
        return false;
    }
    return true;
}

} // namespace platform::drive
