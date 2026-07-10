//! Generates Drive per-query proof vectors and Tenderdash quorum-signature
//! vectors for the C++ verifier in src/platform/drive/ (tested by
//! src/test/platform_drive_tests.cpp).
//!
//! Two JSON files are written to src/test/data/platform/:
//!   - drive_query_vectors.json : one real GroveDB proof per GUI query, built
//!     on a synthetic database whose tree layout mirrors Drive's
//!     (RootTree children + identity sub-structure), plus the expected decoded
//!     result. Proofs are produced with grovedb v5.0.0 and re-verified with
//!     GroveDb::verify_query_raw so the recorded root hash / result set is
//!     ground truth.
//!   - quorum_sig_vectors.json  : Tenderdash Proof envelopes with a real
//!     BLS12-381 basic-scheme signature over the sign digest, the quorum
//!     public key, the block context, all digest intermediates
//!     (request_id, state_id bytes+hash, canonical-vote bytes+hash,
//!     sign_digest) and the expected verdict. Includes negative vectors.
//!
//! The digest construction mirrors rs-tenderdash-abci v1.5.1
//! abci/src/signatures.rs and is self-checked against that crate's own unit
//! test vectors (see `selftest_*`). StateId is encoded with a prost message
//! declared with the exact field tags of tenderdash v1.5.3
//! proto/tendermint/types/types.proto, so its bytes match a real node without
//! needing protoc.
//!
//! Usage (from the repo root):
//!   cd contrib/devtools/platform-drive-vectors
//!   cargo run --release -- ../../../src/test/data/platform

use std::path::Path;

use dpp::identity::identity_public_key::v0::IdentityPublicKeyV0;
use dpp::identity::identity_public_key::IdentityPublicKey;
use dpp::identity::{KeyType, Purpose, SecurityLevel};
use dpp::serialization::PlatformSerializable;
use platform_value::BinaryData;

use dpp::bls_signatures::{Bls12381G2Impl, SecretKey, SerializationFormat, SignatureSchemes};

use grovedb::{Element, GroveDb, PathQuery, Query, QueryItem, SizedQuery};
use grovedb_version::version::GroveVersion;

use integer_encoding::VarInt;
use prost::Message;
use sha2::{Digest, Sha256};

use serde_json::{json, Value as JsonValue};

// ---------------------------------------------------------------------------
// Drive tree layout constants (dashpay/platform tag v4.0.0, protocol 12)
//   packages/rs-drive/src/drive/mod.rs           RootTree
//   packages/rs-drive/src/drive/identity/mod.rs  IdentityRootStructure
//   packages/rs-drive/src/drive/balances/mod.rs  balance_path_vec
// ---------------------------------------------------------------------------
const ROOT_IDENTITIES: u8 = 32;
const ROOT_BALANCES: u8 = 96;
const ROOT_UNIQUE_PKH_TO_IDENTITIES: u8 = 24;

const ID_TREE_REVISION: u8 = 192;
const ID_TREE_NONCE: u8 = 64;
const ID_TREE_KEYS: u8 = 128;

fn hex_of(b: &[u8]) -> String {
    hex::encode(b)
}

fn sha256(b: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(b);
    h.finalize().into()
}

fn double_sha256(b: &[u8]) -> [u8; 32] {
    sha256(&sha256(b))
}

// ---------------------------------------------------------------------------
// Tenderdash StateId protobuf (tenderdash v1.5.3
// proto/tendermint/types/types.proto). Field tags/wire types are declared here
// exactly as tenderdash's build generates them, so prost reproduces node bytes.
// ---------------------------------------------------------------------------
#[derive(Clone, PartialEq, Message)]
struct StateId {
    #[prost(fixed64, tag = "1")]
    app_version: u64,
    #[prost(fixed64, tag = "2")]
    height: u64,
    #[prost(bytes = "vec", tag = "3")]
    app_hash: Vec<u8>,
    #[prost(fixed32, tag = "4")]
    core_chain_locked_height: u32,
    #[prost(fixed64, tag = "5")]
    time: u64,
}

/// request_id = SHA256("dpbvote" || height i64 LE || round i32 LE)
/// (rs-tenderdash-abci signatures.rs::sign_request_id, VOTE_REQUEST_ID_PREFIX).
fn sign_request_id(height: i64, round: i32) -> [u8; 32] {
    let mut buf = Vec::from(b"dpbvote".as_slice());
    buf.extend_from_slice(&height.to_le_bytes());
    buf.extend_from_slice(&round.to_le_bytes());
    sha256(&buf)
}

/// CanonicalVote sign bytes: a fixed layout (NOT protobuf), mirroring
/// rs-tenderdash-abci signatures.rs `impl SignBytes for CanonicalVote`:
///   type i32 LE (4) | height i64 LE (8) | round i64 LE (8) |
///   block_id (32) | state_id (32) | chain_id bytes
fn canonical_vote_sign_bytes(
    vote_type: i32,
    height: i64,
    round: i64,
    block_id: &[u8],
    state_id_hash: &[u8],
    chain_id: &str,
) -> Vec<u8> {
    let mut buf = Vec::with_capacity(4 + 8 + 8 + 32 + 32 + chain_id.len());
    buf.extend_from_slice(&vote_type.to_le_bytes());
    buf.extend_from_slice(&height.to_le_bytes());
    buf.extend_from_slice(&round.to_le_bytes());
    buf.extend_from_slice(block_id);
    buf.extend_from_slice(state_id_hash);
    buf.extend_from_slice(chain_id.as_bytes());
    buf
}

/// sign digest = SHA256(SHA256(quorum_type u8 || rev(quorum_hash) ||
///   rev(request_id) || rev(vote_hash))), rs-tenderdash-abci signatures.rs
///   `sign_hash` (reverse each 32-byte field, then double sha256).
fn sign_hash(
    quorum_type: u8,
    quorum_hash: &[u8; 32],
    request_id: &[u8; 32],
    sign_bytes_hash: &[u8; 32],
) -> [u8; 32] {
    let mut qh = quorum_hash.to_vec();
    qh.reverse();
    let mut rid = request_id.to_vec();
    rid.reverse();
    let mut sbh = sign_bytes_hash.to_vec();
    sbh.reverse();

    let mut buf = Vec::new();
    buf.push(quorum_type);
    buf.append(&mut qh);
    buf.append(&mut rid);
    buf.append(&mut sbh);
    double_sha256(&buf)
}

const SIGNED_MSG_TYPE_PRECOMMIT: i32 = 2;

struct QuorumSigIntermediates {
    request_id: [u8; 32],
    state_id_bytes: Vec<u8>,
    state_id_hash: [u8; 32],
    vote_bytes: Vec<u8>,
    vote_hash: [u8; 32],
    sign_digest: [u8; 32],
}

#[allow(clippy::too_many_arguments)]
fn compute_intermediates(
    app_hash: &[u8; 32],
    quorum_type: u8,
    quorum_hash: &[u8; 32],
    block_id_hash: &[u8; 32],
    height: u64,
    round: i32,
    app_version: u64,
    core_chain_locked_height: u32,
    time_ms: u64,
    chain_id: &str,
) -> QuorumSigIntermediates {
    let request_id = sign_request_id(height as i64, round);

    let state_id = StateId {
        app_version,
        height,
        app_hash: app_hash.to_vec(),
        core_chain_locked_height,
        time: time_ms,
    };
    // encode_length_delimited: message length varint prefix + fields in tag
    // order, proto3 default (zero/empty) fields omitted (prost behaviour, which
    // matches tenderdash's generated code).
    let mut state_id_bytes = Vec::new();
    state_id
        .encode_length_delimited(&mut state_id_bytes)
        .expect("encode state id");
    let state_id_hash = sha256(&state_id_bytes);

    let vote_bytes = canonical_vote_sign_bytes(
        SIGNED_MSG_TYPE_PRECOMMIT,
        height as i64,
        round as i64,
        block_id_hash,
        &state_id_hash,
        chain_id,
    );
    let vote_hash = sha256(&vote_bytes);

    let sign_digest = sign_hash(quorum_type, quorum_hash, &request_id, &vote_hash);

    QuorumSigIntermediates {
        request_id,
        state_id_bytes,
        state_id_hash,
        vote_bytes,
        vote_hash,
        sign_digest,
    }
}

/// Self-check the digest construction against rs-tenderdash-abci's own unit
/// test vectors (abci/src/signatures.rs tests). Panics on any mismatch.
fn selftest_digest() {
    // test_sign_digest: quorum_type 100, quorum_hash, request_id from
    // (height 1001, round 0), given sign_bytes_hash -> expected sign hash.
    let quorum_hash: [u8; 32] =
        hex::decode("6A12D9CF7091D69072E254B297AEF15997093E480FDE295E09A7DE73B31CEEDD")
            .unwrap()
            .try_into()
            .unwrap();
    let request_id = sign_request_id(1001, 0);
    let sign_bytes_hash: [u8; 32] =
        hex::decode("0CA3D5F42BDFED0C4FDE7E6DE0F046CC76CDA6CEE734D65E8B2EE0E375D4C57D")
            .unwrap()
            .try_into()
            .unwrap();
    let expect =
        hex::decode("DA25B746781DDF47B5D736F30B1D9D0CC86981EEC67CBE255265C4361DEF8C2E").unwrap();
    let got = sign_hash(100, &quorum_hash, &request_id, &sign_bytes_hash);
    assert_eq!(got.to_vec(), expect, "sign_hash self-test failed");

    // commit_sign_bytes: type Precommit(2), height 1, round 2, block_id_hash
    // (computed) and state_id_hash -> expected canonical-vote sign bytes.
    let block_id_hash =
        hex::decode("fb7c89bf010a91d50f890455582b7fed0c346e53ab33df7da0bcd85c10fa92ea").unwrap();
    let state_id_hash =
        hex::decode("d7509905b5407ee72dadd93b4ae70a24ad8a7755fc677acd2b215710a05cfc47").unwrap();
    let expect_vote = hex::decode(
        "0200000001000000000000000200000000000000fb7c89bf010a91d50f890455582b7fed0c346e53ab33df7da0bcd85c10fa92ead7509905b5407ee72dadd93b4ae70a24ad8a7755fc677acd2b215710a05cfc47736f6d652d636861696e",
    )
    .unwrap();
    let got_vote = canonical_vote_sign_bytes(
        SIGNED_MSG_TYPE_PRECOMMIT,
        1,
        2,
        &block_id_hash,
        &state_id_hash,
        "some-chain",
    );
    assert_eq!(got_vote, expect_vote, "canonical vote sign bytes self-test failed");

    println!("digest self-test against rs-tenderdash-abci vectors: OK");
}

// ---------------------------------------------------------------------------
// Identity fixture
// ---------------------------------------------------------------------------
struct KeyFixture {
    id: u32,
    purpose: Purpose,
    security_level: SecurityLevel,
    key_type: KeyType,
    data: Vec<u8>,
    disabled_at: Option<u64>,
}

fn build_key(k: &KeyFixture) -> IdentityPublicKey {
    IdentityPublicKeyV0 {
        id: k.id,
        purpose: k.purpose,
        security_level: k.security_level,
        contract_bounds: None,
        key_type: k.key_type,
        read_only: false,
        data: BinaryData::new(k.data.clone()),
        disabled_at: k.disabled_at,
    }
    .into()
}

struct IdentityFixture {
    id: [u8; 32],
    balance: u64,
    revision: u64,
    nonce: u64,
    pubkey_hash: [u8; 20],
    keys: Vec<IdentityPublicKey>,
}

fn build_identity_fixture() -> IdentityFixture {
    let key_fixtures = vec![
        KeyFixture {
            id: 0,
            purpose: Purpose::AUTHENTICATION,
            security_level: SecurityLevel::MASTER,
            key_type: KeyType::ECDSA_SECP256K1,
            data: hex::decode("034f355bdcb7cc0af728ef3cceb9615d90684bb5b2ca5f859ab0f0b704075871aa")
                .unwrap(),
            disabled_at: None,
        },
        KeyFixture {
            id: 1,
            purpose: Purpose::AUTHENTICATION,
            security_level: SecurityLevel::HIGH,
            key_type: KeyType::ECDSA_SECP256K1,
            data: hex::decode("02466d7fcae563e5cb09a0d1870bb580344804617879a14949cf22285f1bae3f27")
                .unwrap(),
            disabled_at: None,
        },
        KeyFixture {
            id: 2,
            purpose: Purpose::ENCRYPTION,
            security_level: SecurityLevel::MEDIUM,
            key_type: KeyType::ECDSA_SECP256K1,
            data: hex::decode("035ab4689e400a4a160cf01cd44730845a54768df8547dcdf073d964f109f18c30")
                .unwrap(),
            disabled_at: Some(1_700_000_000_123),
        },
    ];
    IdentityFixture {
        id: [0x77u8; 32],
        balance: 5_000_000_000,
        revision: 3,
        nonce: 7,
        pubkey_hash: [0xABu8; 20],
        keys: key_fixtures.iter().map(build_key).collect(),
    }
}

// ---------------------------------------------------------------------------
// GroveDB (Drive-layout) construction and proving
// ---------------------------------------------------------------------------
fn build_db(db: &GroveDb, v: &GroveVersion, f: &IdentityFixture) {
    let insert = |path: &[&[u8]], key: &[u8], element: Element| {
        db.insert(path, key, element, None, None, v)
            .unwrap()
            .expect("insert failed");
    };

    // RootTree::Balances (96) is a sum tree of identity credit balances.
    insert(&[], &[ROOT_BALANCES], Element::new_sum_tree(None));
    insert(
        &[&[ROOT_BALANCES]],
        &f.id,
        Element::new_sum_item(f.balance as i64),
    );

    // RootTree::Identities (32): per-identity subtree keyed by identity id.
    insert(&[], &[ROOT_IDENTITIES], Element::empty_tree());
    insert(&[&[ROOT_IDENTITIES]], &f.id, Element::empty_tree());
    // revision (key 192) and nonce (key 64) stored as 8-byte big-endian items.
    insert(
        &[&[ROOT_IDENTITIES], &f.id],
        &[ID_TREE_REVISION],
        Element::new_item(f.revision.to_be_bytes().to_vec()),
    );
    insert(
        &[&[ROOT_IDENTITIES], &f.id],
        &[ID_TREE_NONCE],
        Element::new_item(f.nonce.to_be_bytes().to_vec()),
    );
    // keys subtree (key 128): each identity public key serialized under its
    // varint-encoded KeyID.
    insert(
        &[&[ROOT_IDENTITIES], &f.id],
        &[ID_TREE_KEYS],
        Element::empty_tree(),
    );
    for key in &f.keys {
        let key_id = IdentityPublicKeyGettersV0Id(key);
        let serialized =
            IdentityPublicKey::serialize_to_bytes(key).expect("serialize identity public key");
        insert(
            &[&[ROOT_IDENTITIES], &f.id, &[ID_TREE_KEYS]],
            &key_id.encode_var_vec(),
            Element::new_item(serialized),
        );
    }

    // RootTree::UniquePublicKeyHashesToIdentities (24): pubkey hash -> id.
    insert(&[], &[ROOT_UNIQUE_PKH_TO_IDENTITIES], Element::empty_tree());
    insert(
        &[&[ROOT_UNIQUE_PKH_TO_IDENTITIES]],
        &f.pubkey_hash,
        Element::new_item(f.id.to_vec()),
    );
}

// small helper: KeyID accessor without pulling the accessor trait into scope
#[allow(non_snake_case)]
fn IdentityPublicKeyGettersV0Id(key: &IdentityPublicKey) -> u32 {
    use dpp::identity::identity_public_key::accessors::v0::IdentityPublicKeyGettersV0;
    key.id()
}

fn vec_path(segs: &[&[u8]]) -> Vec<Vec<u8>> {
    segs.iter().map(|s| s.to_vec()).collect()
}

fn single_key_query(path: Vec<Vec<u8>>, key: Vec<u8>, limit: Option<u16>) -> PathQuery {
    let mut query = Query::new();
    query.insert_key(key);
    PathQuery::new(path, SizedQuery::new(query, limit, None))
}

fn range_full_query(path: Vec<Vec<u8>>) -> PathQuery {
    let mut query = Query::new();
    query.insert_item(QueryItem::RangeFull(std::ops::RangeFull));
    PathQuery::new(path, SizedQuery::new(query, None, None))
}

/// Prove a path query, re-verify it raw, and return (proof_bytes, root_hash,
/// result tuples). Panics on any proving/verification failure.
fn prove_and_verify(
    db: &GroveDb,
    v: &GroveVersion,
    pq: &PathQuery,
) -> (Vec<u8>, [u8; 32], Vec<(Vec<Vec<u8>>, Vec<u8>, Vec<u8>)>) {
    let proof = db.prove_query(pq, None, v).unwrap().expect("prove");
    let root_hash = db.root_hash(None, v).unwrap().unwrap();
    let (verified_root, results) =
        GroveDb::verify_query_raw(&proof, pq, v).expect("verify_query_raw");
    assert_eq!(verified_root, root_hash, "root hash mismatch");
    let tuples = results
        .into_iter()
        .map(|r| (r.path, r.key, r.value))
        .collect();
    (proof, root_hash, tuples)
}

fn tuples_json(tuples: &[(Vec<Vec<u8>>, Vec<u8>, Vec<u8>)]) -> JsonValue {
    JsonValue::Array(
        tuples
            .iter()
            .map(|(path, key, value)| {
                json!({
                    "path": path.iter().map(|s| hex_of(s)).collect::<Vec<_>>(),
                    "key": hex_of(key),
                    "element": hex_of(value),
                })
            })
            .collect(),
    )
}

fn main() {
    let out_dir = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "../../../src/test/data/platform".to_string());
    let out_dir = Path::new(&out_dir);
    std::fs::create_dir_all(out_dir).expect("create output dir");

    selftest_digest();

    let v = GroveVersion::latest();
    let f = build_identity_fixture();

    let tmp = tempfile::tempdir().expect("tempdir");
    let db = GroveDb::open(tmp.path()).expect("open grovedb");
    build_db(&db, v, &f);

    // ---- Drive query vectors ----------------------------------------------
    let mut queries = Vec::new();

    // getIdentityBalance
    {
        let pq = single_key_query(vec_path(&[&[ROOT_BALANCES]]), f.id.to_vec(), Some(1));
        let (proof, root, tuples) = prove_and_verify(&db, v, &pq);
        queries.push(json!({
            "name": "identity_balance",
            "query": { "identity_id": hex_of(&f.id) },
            "grovedb_proof_hex": hex_of(&proof),
            "expected_root_hash_hex": hex_of(&root),
            "results": tuples_json(&tuples),
            "expected": { "balance": f.balance },
        }));
    }

    // getIdentity revision (single key 192) — building block for full identity
    {
        let pq = single_key_query(
            vec_path(&[&[ROOT_IDENTITIES], &f.id]),
            vec![ID_TREE_REVISION],
            Some(1),
        );
        let (proof, root, tuples) = prove_and_verify(&db, v, &pq);
        queries.push(json!({
            "name": "identity_revision",
            "query": { "identity_id": hex_of(&f.id) },
            "grovedb_proof_hex": hex_of(&proof),
            "expected_root_hash_hex": hex_of(&root),
            "results": tuples_json(&tuples),
            "expected": { "revision": f.revision },
        }));
    }

    // getIdentityNonce (single key 64)
    {
        let pq = single_key_query(
            vec_path(&[&[ROOT_IDENTITIES], &f.id]),
            vec![ID_TREE_NONCE],
            Some(1),
        );
        let (proof, root, tuples) = prove_and_verify(&db, v, &pq);
        queries.push(json!({
            "name": "identity_nonce",
            "query": { "identity_id": hex_of(&f.id) },
            "grovedb_proof_hex": hex_of(&proof),
            "expected_root_hash_hex": hex_of(&root),
            "results": tuples_json(&tuples),
            "expected": { "nonce": f.nonce },
        }));
    }

    // getIdentityKeys (all keys, RangeFull)
    {
        let pq = range_full_query(vec_path(&[&[ROOT_IDENTITIES], &f.id, &[ID_TREE_KEYS]]));
        let (proof, root, tuples) = prove_and_verify(&db, v, &pq);
        let key_hexes: Vec<String> = f
            .keys
            .iter()
            .map(|k| hex_of(&IdentityPublicKey::serialize_to_bytes(k).unwrap()))
            .collect();
        queries.push(json!({
            "name": "identity_keys",
            "query": { "identity_id": hex_of(&f.id) },
            "grovedb_proof_hex": hex_of(&proof),
            "expected_root_hash_hex": hex_of(&root),
            "results": tuples_json(&tuples),
            "expected": { "serialized_keys": key_hexes },
        }));
    }

    // getIdentityByPublicKeyHash (present)
    {
        let pq = single_key_query(
            vec_path(&[&[ROOT_UNIQUE_PKH_TO_IDENTITIES]]),
            f.pubkey_hash.to_vec(),
            Some(1),
        );
        let (proof, root, tuples) = prove_and_verify(&db, v, &pq);
        queries.push(json!({
            "name": "identity_by_public_key_hash_present",
            "query": { "public_key_hash": hex_of(&f.pubkey_hash) },
            "grovedb_proof_hex": hex_of(&proof),
            "expected_root_hash_hex": hex_of(&root),
            "results": tuples_json(&tuples),
            "expected": { "identity_id": hex_of(&f.id) },
        }));
    }

    // getIdentityByPublicKeyHash (absent -> name/identity not found)
    {
        let absent_hash = [0x01u8; 20];
        let pq = single_key_query(
            vec_path(&[&[ROOT_UNIQUE_PKH_TO_IDENTITIES]]),
            absent_hash.to_vec(),
            Some(1),
        );
        let (proof, root, tuples) = prove_and_verify(&db, v, &pq);
        assert!(tuples.is_empty(), "absence proof must have no result tuples");
        queries.push(json!({
            "name": "identity_by_public_key_hash_absent",
            "query": { "public_key_hash": hex_of(&absent_hash) },
            "grovedb_proof_hex": hex_of(&proof),
            "expected_root_hash_hex": hex_of(&root),
            "results": tuples_json(&tuples),
            "expected": { "identity_id": JsonValue::Null },
        }));
    }

    let drive_json = json!({
        "platform_repo_tag": "v4.0.0",
        "grovedb_repo_tag": "v5.0.0",
        "protocol_version": 12,
        "identity_id": hex_of(&f.id),
        "queries": queries,
    });

    // ---- Quorum signature vectors -----------------------------------------
    // Use the balance proof's real GroveDB root hash as the app_hash so the
    // grovedb-proof -> root -> quorum-sig binding is exercised end to end.
    let balance_pq = single_key_query(vec_path(&[&[ROOT_BALANCES]]), f.id.to_vec(), Some(1));
    let (balance_proof, app_hash, _t) = prove_and_verify(&db, v, &balance_pq);

    let quorum_type: u8 = 106; // testnet Platform LLMQ type (llmq_25_67)
    let quorum_hash: [u8; 32] = {
        let mut a = [0u8; 32];
        for (i, b) in a.iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(7).wrapping_add(1);
        }
        a
    };
    let block_id_hash: [u8; 32] = {
        let mut a = [0u8; 32];
        for (i, b) in a.iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(3).wrapping_add(9);
        }
        a
    };
    let height: u64 = 123_456;
    let round: i32 = 0;
    let app_version: u64 = 12;
    let core_chain_locked_height: u32 = 2_000_000;
    let time_ms: u64 = 1_700_000_000_000;
    let chain_id = "dash-testnet-51";

    let inter = compute_intermediates(
        &app_hash,
        quorum_type,
        &quorum_hash,
        &block_id_hash,
        height,
        round,
        app_version,
        core_chain_locked_height,
        time_ms,
        chain_id,
    );

    // Real BLS12-381 basic-scheme key + signature over the sign digest.
    // blsful "Modern" serialization == IETF format == dashbls non-legacy
    // (basic) scheme, so the recorded 48-byte pubkey / 96-byte signature
    // interoperate with src/bls CBLSPublicKey/CBLSSignature (legacy=false).
    let sk = SecretKey::<Bls12381G2Impl>::from_hash(b"dash-platform-drive-vectors-quorum-key");
    let pk = sk.public_key();
    let pubkey_bytes: Vec<u8> = (&pk).into();
    assert_eq!(pubkey_bytes.len(), 48);
    let signature = sk
        .sign(SignatureSchemes::Basic, &inter.sign_digest)
        .expect("bls sign");
    let signature_bytes = signature.to_bytes_with_mode(SerializationFormat::Modern);
    assert_eq!(signature_bytes.len(), 96);
    // sanity: it verifies with blsful itself
    signature
        .verify(&pk, inter.sign_digest)
        .expect("self-verify");

    // A second, unrelated quorum key for the "wrong quorum key" negative.
    let wrong_sk =
        SecretKey::<Bls12381G2Impl>::from_hash(b"dash-platform-drive-vectors-WRONG-key");
    let wrong_pubkey_bytes: Vec<u8> = (&wrong_sk.public_key()).into();

    let intermediates_json = json!({
        "request_id": hex_of(&inter.request_id),
        "state_id_bytes": hex_of(&inter.state_id_bytes),
        "state_id_hash": hex_of(&inter.state_id_hash),
        "canonical_vote_bytes": hex_of(&inter.vote_bytes),
        "vote_hash": hex_of(&inter.vote_hash),
        "sign_digest": hex_of(&inter.sign_digest),
    });

    let block_context = json!({
        "height": height,
        "round": round,
        "core_chain_locked_height": core_chain_locked_height,
        "time_ms": time_ms,
        "protocol_version": app_version,
        "chain_id": chain_id,
    });

    let make_envelope = |grovedb_proof: &[u8],
                         quorum_hash: &[u8; 32],
                         block_id_hash: &[u8; 32],
                         signature: &[u8]| {
        json!({
            "grovedb_proof_hex": hex_of(grovedb_proof),
            "quorum_type": quorum_type,
            "quorum_hash": hex_of(quorum_hash),
            "block_id_hash": hex_of(block_id_hash),
            "round": round,
            "signature": hex_of(signature),
        })
    };

    // valid vector
    let valid = json!({
        "name": "valid",
        "app_hash": hex_of(&app_hash),
        "envelope": make_envelope(&balance_proof, &quorum_hash, &block_id_hash, &signature_bytes),
        "quorum_public_key": hex_of(&pubkey_bytes),
        "block_context": block_context.clone(),
        "intermediates": intermediates_json.clone(),
        "expected_valid": true,
    });

    // negative: tampered signature (flip one byte)
    let mut tampered_sig = signature_bytes.clone();
    tampered_sig[10] ^= 0x01;
    let neg_sig = json!({
        "name": "tampered_signature",
        "app_hash": hex_of(&app_hash),
        "envelope": make_envelope(&balance_proof, &quorum_hash, &block_id_hash, &tampered_sig),
        "quorum_public_key": hex_of(&pubkey_bytes),
        "block_context": block_context.clone(),
        "expected_valid": false,
    });

    // negative: wrong quorum key
    let neg_key = json!({
        "name": "wrong_quorum_key",
        "app_hash": hex_of(&app_hash),
        "envelope": make_envelope(&balance_proof, &quorum_hash, &block_id_hash, &signature_bytes),
        "quorum_public_key": hex_of(&wrong_pubkey_bytes),
        "block_context": block_context.clone(),
        "expected_valid": false,
    });

    // negative: wrong app hash (root hash mutated -> digest changes -> sig fails)
    let mut wrong_app_hash = app_hash;
    wrong_app_hash[0] ^= 0xff;
    let neg_apphash = json!({
        "name": "wrong_app_hash",
        "app_hash": hex_of(&wrong_app_hash),
        "envelope": make_envelope(&balance_proof, &quorum_hash, &block_id_hash, &signature_bytes),
        "quorum_public_key": hex_of(&pubkey_bytes),
        "block_context": block_context.clone(),
        "expected_valid": false,
    });

    // negative: wrong block id hash (part of the canonical vote preimage)
    let mut wrong_block_id = block_id_hash;
    wrong_block_id[5] ^= 0x0f;
    let neg_blockid = json!({
        "name": "wrong_block_id_hash",
        "app_hash": hex_of(&app_hash),
        "envelope": make_envelope(&balance_proof, &quorum_hash, &wrong_block_id, &signature_bytes),
        "quorum_public_key": hex_of(&pubkey_bytes),
        "block_context": block_context.clone(),
        "expected_valid": false,
    });

    let quorum_json = json!({
        "tenderdash_abci_tag": "v1.5.1",
        "tenderdash_proto_tag": "v1.5.3",
        "bls_scheme": "basic",
        "vectors": [valid, neg_sig, neg_key, neg_apphash, neg_blockid],
    });

    // ---- write -------------------------------------------------------------
    let write = |name: &str, value: JsonValue| {
        let path = out_dir.join(name);
        let mut text = serde_json::to_string_pretty(&value).expect("serialize json");
        text.push('\n');
        std::fs::write(&path, text).expect("write json");
        println!("wrote {}", path.display());
    };
    write("drive_query_vectors.json", drive_json);
    write("quorum_sig_vectors.json", quorum_json);
}
