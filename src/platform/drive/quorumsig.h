// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DRIVE_QUORUMSIG_H
#define BITCOIN_PLATFORM_DRIVE_QUORUMSIG_H

#include <platform/proof/merk.h>
#include <platform/serialize.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//! Tenderdash quorum threshold-signature verification: binds a GroveDB root
//! hash (the "app hash" a DAPI proof commits to) to a Platform block signed by
//! an LLMQ quorum. Pure C++ port of:
//!  - dashpay/platform rs-drive-proof-verifier v4.0.0 src/verify.rs
//!    (verify_tenderdash_proof, verify_signature_digest);
//!  - dashpay/rs-tenderdash-abci v1.5.1 abci/src/signatures.rs
//!    (sign_request_id, StateId::sign_bytes, CanonicalVote::sign_bytes,
//!    sign_hash);
//!  - tenderdash v1.5.3 proto/tendermint/types/types.proto (StateId field
//!    tags), proto/tendermint/types/canonical.proto (CanonicalVote layout).
//!
//! The BLS check uses the BASIC (non-legacy, IETF) scheme: 48-byte G1 public
//! key, 96-byte G2 signature, message = the 32-byte sign digest.
namespace platform::drive {

using platform::merk::Hash256;

//! The fields of a DAPI Proof protobuf envelope (dapi-grpc
//! platform/v0/platform.proto message Proof) that the signature check needs.
//! grovedb_proof is verified separately (see queries.h); only its resulting
//! root hash enters here as `app_hash`.
struct ProofEnvelope {
    uint8_t quorum_type{0};
    Hash256 quorum_hash{};
    Hash256 block_id_hash{};
    int32_t round{0};
    std::array<uint8_t, 96> signature{};
};

//! Block context taken from the DAPI ResponseMetadata (height, core-locked
//! height, time, protocol version) plus the Tenderdash chain id.
struct BlockContext {
    uint64_t height{0};
    uint32_t core_chain_locked_height{0};
    uint64_t time_ms{0};
    uint64_t protocol_version{0};
    std::string chain_id;
};

//! Intermediate digests, exposed so each layer can be checked independently
//! against the Rust-generated vectors.
struct SignDigestIntermediates {
    Hash256 request_id{};
    Bytes state_id_bytes;
    Hash256 state_id_hash{};
    Bytes canonical_vote_bytes;
    Hash256 vote_hash{};
    Hash256 sign_digest{};
};

//! request_id = SHA256("dpbvote" || height:i64-LE || round:i32-LE).
Hash256 ComputeRequestId(uint64_t height, int32_t round);

//! StateId protobuf sign bytes: prost length-delimited encoding of
//! StateId{app_version=1(fixed64), height=2(fixed64), app_hash=3(bytes),
//! core_chain_locked_height=4(fixed32), time=5(fixed64)}, proto3 default
//! (zero/empty) fields omitted, fields in ascending tag order, message
//! length-prefixed with a protobuf varint.
Bytes EncodeStateIdSignBytes(const Hash256& app_hash, const BlockContext& ctx, uint64_t height);

//! CanonicalVote sign bytes (a fixed layout, NOT protobuf):
//! type:i32-LE(=2 Precommit) | height:i64-LE | round:i64-LE | block_id(32) |
//! state_id_hash(32) | chain_id.
Bytes EncodeCanonicalVoteSignBytes(int32_t vote_type, uint64_t height, int32_t round,
                                   const Hash256& block_id_hash, const Hash256& state_id_hash,
                                   const std::string& chain_id);

//! sign_digest = SHA256(SHA256(quorum_type:u8 | reverse(quorum_hash) |
//! reverse(request_id) | reverse(vote_hash))).
Hash256 ComputeSignHash(uint8_t quorum_type, const Hash256& quorum_hash, const Hash256& request_id,
                        const Hash256& vote_hash);

//! Computes every intermediate up to and including the final sign digest.
SignDigestIntermediates ComputeSignDigest(const Hash256& app_hash, const ProofEnvelope& envelope,
                                          const BlockContext& ctx);

//! Verifies the quorum threshold signature over the sign digest that binds
//! `app_hash` to the block described by `ctx`/`envelope`. `quorum_pubkey`
//! is the raw 48-byte basic-scheme public key of the signing quorum. Returns
//! false (with `error` set) on a malformed key/signature or a failed check.
//! On success `out_digest`, if provided, receives the intermediates.
bool VerifyQuorumSig(const Hash256& app_hash, const ProofEnvelope& envelope,
                     const BlockContext& ctx, const std::vector<uint8_t>& quorum_pubkey,
                     std::string& error, SignDigestIntermediates* out_digest = nullptr);

} // namespace platform::drive

#endif // BITCOIN_PLATFORM_DRIVE_QUORUMSIG_H
