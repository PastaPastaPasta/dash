//! Generates DPP test vectors for the C++ port in src/platform/dpp/.
//!
//! Everything is produced by the real rs-dpp code (dashpay/platform tag
//! v4.0.0, protocol version 12): state transitions are built through the same
//! factory methods the Rust SDK uses, signed with dashcore::signer (double
//! SHA256 + 65-byte compact recoverable ECDSA), and serialized with
//! rs-platform-serialization (bincode standard() + big-endian + varint).
//!
//! Usage: cargo run -- <output-dir>
//! Writes dpp_identity_vectors.json and dpp_st_vectors.json.

use std::collections::BTreeMap;

use dpp::dashcore::bls_sig_utils::BLSSignature;
use dpp::dashcore::consensus::serialize as consensus_serialize;
use dpp::dashcore::hashes::Hash;
use dpp::dashcore::secp256k1::{PublicKey as SecpPublicKey, Secp256k1, SecretKey};
use dpp::dashcore::transaction::special_transaction::asset_lock::AssetLockPayload;
use dpp::dashcore::transaction::special_transaction::TransactionPayload;
use dpp::dashcore::{signer, InstantLock, OutPoint, ScriptBuf, Transaction, TxIn, Txid, TxOut};
use dpp::document::{Document, DocumentV0};
use dpp::identity::identity_public_key::accessors::v0::IdentityPublicKeyGettersV0;
use dpp::identity::signer::Signer;
use dpp::identity::state_transition::asset_lock_proof::chain::ChainAssetLockProof;
use dpp::identity::state_transition::asset_lock_proof::{AssetLockProof, InstantAssetLockProof};
use dpp::identity::contract_bounds::ContractBounds;
use dpp::identity::{
    Identity, IdentityPublicKey, IdentityV0, KeyType, Purpose, SecurityLevel,
};
use dpp::identity::identity_public_key::v0::IdentityPublicKeyV0;
use dpp::data_contract::accessors::v0::DataContractV0Getters;
use dpp::data_contract::document_type::methods::DocumentTypeV0Methods;
use dpp::data_contract::DataContract;
use dpp::serialization::{PlatformSerializable, Signable};
use dpp::state_transition::batch_transition::methods::v0::DocumentsBatchTransitionMethodsV0;
use dpp::state_transition::batch_transition::BatchTransition;
use dpp::state_transition::identity_create_transition::methods::IdentityCreateTransitionMethodsV0;
use dpp::state_transition::identity_create_transition::IdentityCreateTransition;
use dpp::state_transition::StateTransition;
use dpp::system_data_contracts::{load_system_data_contract, SystemDataContract};
use dpp::util::hash::hash_double;
use dpp::util::strings::convert_to_homograph_safe_chars;
use dpp::address_funds::AddressWitness;
use dpp::{BlsModule, ProtocolError, PublicKeyValidationError};
use platform_value::{BinaryData, Identifier, Value};
use platform_version::version::PlatformVersion;
use serde_json::{json, Value as JsonValue};

const MASTER_KEY_PRIV: [u8; 32] = [0x11; 32];
const HIGH_KEY_PRIV: [u8; 32] = [0x22; 32];
const ASSET_LOCK_PRIV: [u8; 32] = [0x33; 32];
const PREORDER_SALT: [u8; 32] = [0x55; 32];

/// Document entropy convention of the C++ builders (src/platform/dpp/
/// statetransitions.cpp): rs-dpp uses caller-provided random entropy, the
/// dash-qt builders derive it deterministically as
/// DSHA256(owner_id || document_type_name || identity_contract_nonce (LE u64))
/// so that retries of the same (identity, nonce) rebuild the identical
/// transition. DPNS documents use flow-specific 32-byte values instead
/// (preorder: the salted domain hash; domain: the preorder salt).
fn derived_entropy(owner: &Identifier, document_type_name: &str, nonce: u64) -> [u8; 32] {
    let mut buf: Vec<u8> = Vec::new();
    buf.extend_from_slice(owner.as_slice());
    buf.extend_from_slice(document_type_name.as_bytes());
    buf.extend_from_slice(&nonce.to_le_bytes());
    hash_double(buf.as_slice())
}

fn hex_of(bytes: &[u8]) -> String {
    hex::encode(bytes)
}

fn pubkey_of(priv_bytes: &[u8; 32]) -> Vec<u8> {
    let secp = Secp256k1::new();
    let sk = SecretKey::from_slice(priv_bytes).expect("valid secp256k1 secret key");
    SecpPublicKey::from_secret_key(&secp, &sk).serialize().to_vec()
}

/// Signs with fixed private keys, looked up by public key data. Mirrors what
/// interfaces::Wallet::signPlatformDigest does in dash-qt: RFC6979
/// deterministic ECDSA, 65-byte compact recoverable signature over
/// double-SHA256 of the payload.
#[derive(Debug)]
struct FixedSigner {
    keys: BTreeMap<Vec<u8>, [u8; 32]>,
}

#[async_trait::async_trait]
impl Signer<IdentityPublicKey> for FixedSigner {
    async fn sign(
        &self,
        key: &IdentityPublicKey,
        data: &[u8],
    ) -> Result<BinaryData, ProtocolError> {
        let sk = self
            .keys
            .get(key.data().as_slice())
            .expect("signer asked for an unknown key");
        let sig = signer::sign(data, sk).map_err(|e| ProtocolError::Generic(e.to_string()))?;
        Ok(BinaryData::new(sig.to_vec()))
    }

    async fn sign_create_witness(
        &self,
        _key: &IdentityPublicKey,
        _data: &[u8],
    ) -> Result<AddressWitness, ProtocolError> {
        Err(ProtocolError::Generic("not supported".to_string()))
    }

    fn can_sign_with(&self, key: &IdentityPublicKey) -> bool {
        self.keys.contains_key(key.data().as_slice())
    }
}

/// BLS is never exercised (all vector keys are ECDSA_SECP256K1); the module
/// only satisfies the try_from_identity_with_signer_and_private_key signature.
#[derive(Debug)]
struct NoBls;

impl BlsModule for NoBls {
    fn validate_public_key(&self, _pk: &[u8]) -> Result<(), PublicKeyValidationError> {
        Ok(())
    }
    fn verify_signature(
        &self,
        _signature: &[u8],
        _data: &[u8],
        _public_key: &[u8],
    ) -> Result<bool, ProtocolError> {
        Err(ProtocolError::Generic("bls not supported".to_string()))
    }
    fn private_key_to_public_key(&self, _private_key: &[u8]) -> Result<Vec<u8>, ProtocolError> {
        Err(ProtocolError::Generic("bls not supported".to_string()))
    }
    fn sign(&self, _data: &[u8], _private_key: &[u8]) -> Result<Vec<u8>, ProtocolError> {
        Err(ProtocolError::Generic("bls not supported".to_string()))
    }
}

fn identity_key(
    id: u32,
    purpose: Purpose,
    security_level: SecurityLevel,
    data: Vec<u8>,
    contract_bounds: Option<ContractBounds>,
    disabled_at: Option<u64>,
) -> IdentityPublicKey {
    IdentityPublicKey::V0(IdentityPublicKeyV0 {
        id,
        purpose,
        security_level,
        contract_bounds,
        key_type: KeyType::ECDSA_SECP256K1,
        read_only: false,
        data: BinaryData::new(data),
        disabled_at,
    })
}

fn key_json(key: &IdentityPublicKey) -> JsonValue {
    json!({
        "id": key.id(),
        "purpose": key.purpose() as u8,
        "security_level": key.security_level() as u8,
        "key_type": key.key_type() as u8,
        "read_only": key.read_only(),
        "data": hex_of(key.data().as_slice()),
        "disabled_at": key.disabled_at(),
    })
}

fn st_json(st: &StateTransition, extra: JsonValue) -> JsonValue {
    let signable = st.signable_bytes().expect("signable bytes");
    let digest = hash_double(signable.as_slice());
    let serialized = st.serialize_to_bytes().expect("serialize");
    let mut obj = json!({
        "signable_hex": hex_of(&signable),
        "digest_hex": hex_of(&digest),
        "serialized_hex": hex_of(&serialized),
    });
    if let (Some(base), Some(add)) = (obj.as_object_mut(), extra.as_object()) {
        for (k, v) in add {
            base.insert(k.clone(), v.clone());
        }
    }
    obj
}

fn asset_lock_transaction() -> Transaction {
    // One credit output inside the asset-lock payload; tx.vout carries the
    // OP_RETURN burn output as on the wire.
    let credit_output = TxOut {
        value: 100_000,
        script_pubkey: ScriptBuf::from_bytes(vec![
            0x76, 0xa9, 0x14, // OP_DUP OP_HASH160 push20
            0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99,
            0x99, 0x99, 0x99, 0x99, 0x99, 0x99, //
            0x88, 0xac, // OP_EQUALVERIFY OP_CHECKSIG
        ]),
    };
    Transaction {
        version: 3,
        lock_time: 0,
        input: vec![TxIn {
            previous_output: OutPoint {
                txid: Txid::from_byte_array([0xdd; 32]),
                vout: 1,
            },
            script_sig: ScriptBuf::new(),
            sequence: 0xffffffff,
            witness: Default::default(),
        }],
        output: vec![TxOut {
            value: 100_000,
            script_pubkey: ScriptBuf::from_bytes(vec![0x6a]), // OP_RETURN
        }],
        special_transaction_payload: Some(TransactionPayload::AssetLockPayloadType(
            AssetLockPayload {
                version: 1,
                credit_outputs: vec![credit_output],
            },
        )),
    }
}

fn document(id: Identifier, owner: Identifier, properties: BTreeMap<String, Value>, revision: Option<u64>) -> Document {
    Document::V0(DocumentV0 {
        id,
        owner_id: owner,
        properties,
        revision,
        created_at: None,
        updated_at: None,
        transferred_at: None,
        created_at_block_height: None,
        updated_at_block_height: None,
        transferred_at_block_height: None,
        created_at_core_block_height: None,
        updated_at_core_block_height: None,
        transferred_at_core_block_height: None,
        creator_id: None,
    })
}

#[allow(clippy::too_many_arguments)]
fn document_create_st(
    contract: &DataContract,
    document_type_name: &str,
    owner: Identifier,
    properties: BTreeMap<String, Value>,
    entropy: [u8; 32],
    nonce: u64,
    key: &IdentityPublicKey,
    signer: &FixedSigner,
    platform_version: &PlatformVersion,
) -> (Identifier, StateTransition) {
    let document_type = contract
        .document_type_for_name(document_type_name)
        .expect("document type");
    let id = Document::generate_document_id_v0(
        &contract.id(),
        &owner,
        document_type_name,
        entropy.as_slice(),
    );
    let doc = document(id, owner, properties, Some(1));
    let st = futures::executor::block_on(
        BatchTransition::new_document_creation_transition_from_document(
            doc,
            document_type,
            entropy,
            key,
            nonce,
            0, // user fee increase
            None,
            signer,
            platform_version,
            None,
        ),
    )
    .expect("create document transition");
    (id, st)
}

fn main() {
    let out_dir = std::env::args().nth(1).unwrap_or_else(|| ".".to_string());
    let platform_version = PlatformVersion::latest();
    let protocol_version = platform_version.protocol_version;

    let master_pub = pubkey_of(&MASTER_KEY_PRIV);
    let high_pub = pubkey_of(&HIGH_KEY_PRIV);
    let asset_lock_pub = pubkey_of(&ASSET_LOCK_PRIV);

    let signer = FixedSigner {
        keys: BTreeMap::from([
            (master_pub.clone(), MASTER_KEY_PRIV),
            (high_pub.clone(), HIGH_KEY_PRIV),
            (asset_lock_pub.clone(), ASSET_LOCK_PRIV),
        ]),
    };

    // ---------------------------------------------------------------- identity vectors
    let master_key = identity_key(
        0,
        Purpose::AUTHENTICATION,
        SecurityLevel::MASTER,
        master_pub.clone(),
        None,
        None,
    );
    let high_key = identity_key(
        1,
        Purpose::AUTHENTICATION,
        SecurityLevel::HIGH,
        high_pub.clone(),
        None,
        None,
    );
    // Exercises Option<ContractBounds> and Option<disabled_at>.
    let bounded_key = identity_key(
        2,
        Purpose::ENCRYPTION,
        SecurityLevel::MEDIUM,
        pubkey_of(&[0x66; 32]),
        Some(ContractBounds::SingleContract {
            id: Identifier::from(dpp::system_data_contracts::SystemDataContract::Dashpay.id()),
        }),
        Some(1_700_000_000_123),
    );

    let identity_id = Identifier::from([0x77; 32]);
    let identity = Identity::V0(IdentityV0 {
        id: identity_id,
        public_keys: BTreeMap::from([
            (0, master_key.clone()),
            (1, high_key.clone()),
            (2, bounded_key.clone()),
        ]),
        // > u32::MAX so the u64 varint marker + big-endian payload is
        // exercised in the vector.
        balance: 5_000_000_000,
        revision: 3,
    });
    let identity_bytes = identity.serialize_to_bytes().expect("identity serialize");

    let identity_vectors = json!({
        "platform_repo_tag": "v4.0.0",
        "protocol_version": protocol_version,
        "identity": {
            "serialized_hex": hex_of(&identity_bytes),
            "id": hex_of(identity_id.as_slice()),
            "balance": 5_000_000_000u64,
            "revision": 3,
            "public_keys": [key_json(&master_key), key_json(&high_key), key_json(&bounded_key)],
            "bounded_key_contract_id": hex_of(dpp::system_data_contracts::SystemDataContract::Dashpay.id().as_slice()),
        },
        "identity_public_key": {
            "serialized_hex": hex_of(&high_key.serialize_to_bytes().expect("key serialize")),
            "fields": key_json(&high_key),
        },
    });

    // ---------------------------------------------------------------- identity create
    let tx = asset_lock_transaction();
    let txid = tx.txid();
    let islock = InstantLock {
        version: 1,
        inputs: vec![OutPoint {
            txid: Txid::from_byte_array([0xaa; 32]),
            vout: 0,
        }],
        txid,
        cyclehash: dpp::dashcore::hash_types::CycleHash::from_byte_array([0xbb; 32]),
        signature: BLSSignature::from([0xcc; 96]),
    };
    let tx_bytes = consensus_serialize(&tx);
    let islock_bytes = consensus_serialize(&islock);

    // The identity registered by both create transitions (master + high key).
    let created_identity = Identity::V0(IdentityV0 {
        id: Identifier::from([0u8; 32]), // replaced by proof-derived id
        public_keys: BTreeMap::from([(0, master_key.clone()), (1, high_key.clone())]),
        balance: 0,
        revision: 0,
    });

    let instant_proof = AssetLockProof::Instant(InstantAssetLockProof::new(
        islock.clone(),
        tx.clone(),
        0,
    ));
    let instant_outpoint: [u8; 36] = instant_proof.out_point().expect("outpoint").into();
    let instant_identity_id = instant_proof.create_identifier().expect("identifier");

    let st_instant = futures::executor::block_on(
        IdentityCreateTransition::try_from_identity_with_signer_and_private_key(
            &created_identity,
            instant_proof,
            &ASSET_LOCK_PRIV,
            &signer,
            &NoBls,
            0,
            platform_version,
        ),
    )
    .expect("identity create (instant)");

    let chain_outpoint_raw: [u8; 36] = {
        let mut buf = [0u8; 36];
        buf[..32].copy_from_slice(&txid.to_byte_array());
        buf[32..].copy_from_slice(&0u32.to_le_bytes());
        buf
    };
    let chain_proof = AssetLockProof::Chain(ChainAssetLockProof::new(1_050_000, chain_outpoint_raw));
    let chain_identity_id = chain_proof.create_identifier().expect("identifier");

    let st_chain = futures::executor::block_on(
        IdentityCreateTransition::try_from_identity_with_signer_and_private_key(
            &created_identity,
            chain_proof,
            &ASSET_LOCK_PRIV,
            &signer,
            &NoBls,
            0,
            platform_version,
        ),
    )
    .expect("identity create (chain)");

    // ---------------------------------------------------------------- documents
    let dpns = load_system_data_contract(SystemDataContract::DPNS, platform_version)
        .expect("dpns contract");
    let dashpay = load_system_data_contract(SystemDataContract::Dashpay, platform_version)
        .expect("dashpay contract");

    let owner = instant_identity_id;

    // DPNS preorder.
    let label = "Alice";
    let normalized_label = convert_to_homograph_safe_chars(label);
    let mut salted: Vec<u8> = Vec::new();
    salted.extend_from_slice(&PREORDER_SALT);
    salted.extend_from_slice((normalized_label.clone() + ".dash").as_bytes());
    let salted_domain_hash = hash_double(salted.as_slice());

    let (preorder_id, st_preorder) = document_create_st(
        &dpns,
        "preorder",
        owner,
        BTreeMap::from([(
            "saltedDomainHash".to_string(),
            Value::Bytes32(salted_domain_hash),
        )]),
        salted_domain_hash,
        2,
        &high_key,
        &signer,
        platform_version,
    );

    // DPNS domain (label "Alice" normalizes to "a11ce": contested, so the
    // factory adds the prefunded voting balance tuple).
    let domain_properties = |label: &str, normalized: &str| {
        BTreeMap::from([
            ("parentDomainName".to_string(), Value::Text("dash".to_string())),
            (
                "normalizedParentDomainName".to_string(),
                Value::Text("dash".to_string()),
            ),
            ("label".to_string(), Value::Text(label.to_string())),
            ("normalizedLabel".to_string(), Value::Text(normalized.to_string())),
            ("preorderSalt".to_string(), Value::Bytes32(PREORDER_SALT)),
            (
                "records".to_string(),
                Value::Map(vec![(
                    Value::Text("identity".to_string()),
                    Value::Identifier(owner.to_buffer()),
                )]),
            ),
            (
                "subdomainRules".to_string(),
                Value::Map(vec![(
                    Value::Text("allowSubdomains".to_string()),
                    Value::Bool(false),
                )]),
            ),
        ])
    };

    // identity_contract_nonce 0x1234 pins the big-endian u16 varint encoding.
    let (domain_id, st_domain) = document_create_st(
        &dpns,
        "domain",
        owner,
        domain_properties(label, &normalized_label),
        PREORDER_SALT,
        0x1234,
        &high_key,
        &signer,
        platform_version,
    );

    // Non-contested domain: digits 2-9 fall outside the contested character
    // set of the DPNS contract's parentNameAndLabel contested index.
    let label2 = "quantum42";
    let normalized_label2 = convert_to_homograph_safe_chars(label2);
    let (domain2_id, st_domain2) = document_create_st(
        &dpns,
        "domain",
        owner,
        domain_properties(label2, &normalized_label2),
        PREORDER_SALT,
        5,
        &high_key,
        &signer,
        platform_version,
    );

    // Contested-label truth table straight from the DPNS document type
    // (prefunded_voting_balance_for_document over the real contract).
    let domain_type = dpns.document_type_for_name("domain").expect("domain type");
    let contested_cases: Vec<JsonValue> = [
        "alice", "a11ce", "bob", "b0b", "ab", "abc", "x2y", "up", "-ab-",
        "aaaaaaaaaaaaaaaaaaa",  // 19 chars
        "aaaaaaaaaaaaaaaaaaaa", // 20 chars
        "quantum42", "dash", "test-name", "name2",
    ]
    .iter()
    .map(|raw| {
        let normalized = convert_to_homograph_safe_chars(raw);
        let doc = document(
            Identifier::from([1u8; 32]),
            owner,
            domain_properties(raw, &normalized),
            Some(1),
        );
        let contested = domain_type
            .prefunded_voting_balance_for_document(&doc, platform_version)
            .expect("prefunded voting balance")
            .is_some();
        json!({"label": raw, "normalized": normalized, "contested": contested})
    })
    .collect();

    // DashPay profile create + replace.
    let profile_properties = BTreeMap::from([
        ("displayName".to_string(), Value::Text("Alice в Wonderland".to_string())),
        ("publicMessage".to_string(), Value::Text("hello platform".to_string())),
        ("avatarUrl".to_string(), Value::Text("https://example.com/a.png".to_string())),
        ("avatarHash".to_string(), Value::Bytes32([0x88; 32])),
        ("avatarFingerprint".to_string(), Value::Bytes(vec![0x99; 8])),
    ]);
    let profile_entropy = derived_entropy(&owner, "profile", 3);
    let (profile_id, st_profile_create) = document_create_st(
        &dashpay,
        "profile",
        owner,
        profile_properties.clone(),
        profile_entropy,
        3,
        &high_key,
        &signer,
        platform_version,
    );

    let mut replace_properties = profile_properties.clone();
    replace_properties.insert(
        "publicMessage".to_string(),
        Value::Text("updated message".to_string()),
    );
    let profile_doc_replace = document(profile_id, owner, replace_properties.clone(), Some(2));
    let profile_type = dashpay.document_type_for_name("profile").expect("profile type");
    let st_profile_replace = futures::executor::block_on(
        BatchTransition::new_document_replacement_transition_from_document(
            profile_doc_replace,
            profile_type,
            &high_key,
            4,
            0,
            None,
            &signer,
            platform_version,
            None,
        ),
    )
    .expect("profile replace transition");

    // DashPay contactRequest create.
    let to_user_id = Identifier::from([0xee; 32]);
    let contact_properties = BTreeMap::from([
        ("toUserId".to_string(), Value::Identifier(to_user_id.to_buffer())),
        ("encryptedPublicKey".to_string(), Value::Bytes(vec![0xab; 96])),
        ("senderKeyIndex".to_string(), Value::U32(2)),
        ("recipientKeyIndex".to_string(), Value::U32(3)),
        ("accountReference".to_string(), Value::U32(0x0badc0de)),
        ("encryptedAccountLabel".to_string(), Value::Bytes(vec![0xcd; 48])),
    ]);
    let contact_entropy = derived_entropy(&owner, "contactRequest", 6);
    let (contact_id, st_contact) = document_create_st(
        &dashpay,
        "contactRequest",
        owner,
        contact_properties,
        contact_entropy,
        6,
        &high_key,
        &signer,
        platform_version,
    );

    let st_vectors = json!({
        "platform_repo_tag": "v4.0.0",
        "protocol_version": protocol_version,
        "signature_scheme": "double-SHA256 of signable bytes; 65-byte compact recoverable ECDSA; header byte = 27 + recovery_id + 4 (compressed)",
        "keys": {
            "master": {"id": 0, "private_key_hex": hex_of(&MASTER_KEY_PRIV), "public_key_hex": hex_of(&master_pub)},
            "high": {"id": 1, "private_key_hex": hex_of(&HIGH_KEY_PRIV), "public_key_hex": hex_of(&high_pub)},
            "asset_lock": {"private_key_hex": hex_of(&ASSET_LOCK_PRIV), "public_key_hex": hex_of(&asset_lock_pub)},
        },
        "identity_create_instant": st_json(&st_instant, json!({
            "transaction_hex": hex_of(&tx_bytes),
            "instant_lock_hex": hex_of(&islock_bytes),
            "output_index": 0,
            "out_point_hex": hex_of(&instant_outpoint),
            "identity_id_hex": hex_of(instant_identity_id.as_slice()),
        })),
        "identity_create_chain": st_json(&st_chain, json!({
            "core_chain_locked_height": 1_050_000,
            "out_point_hex": hex_of(&chain_outpoint_raw),
            "identity_id_hex": hex_of(chain_identity_id.as_slice()),
        })),
        "dpns_preorder": st_json(&st_preorder, json!({
            "owner_id_hex": hex_of(owner.as_slice()),
            "identity_contract_nonce": 2,
            "entropy_hex": hex_of(&salted_domain_hash),
            "salt_hex": hex_of(&PREORDER_SALT),
            "label": label,
            "normalized_label": normalized_label,
            "salted_domain_hash_hex": hex_of(&salted_domain_hash),
            "document_id_hex": hex_of(preorder_id.as_slice()),
            "signature_public_key_id": 1,
        })),
        "dpns_domain_contested": st_json(&st_domain, json!({
            "owner_id_hex": hex_of(owner.as_slice()),
            "identity_contract_nonce": 0x1234,
            "entropy_hex": hex_of(&PREORDER_SALT),
            "salt_hex": hex_of(&PREORDER_SALT),
            "label": label,
            "normalized_label": normalized_label,
            "document_id_hex": hex_of(domain_id.as_slice()),
            "signature_public_key_id": 1,
            "contested": true,
            "prefunded_voting_balance_credits": 20_000_000_000u64,
        })),
        "dpns_domain": st_json(&st_domain2, json!({
            "owner_id_hex": hex_of(owner.as_slice()),
            "identity_contract_nonce": 5,
            "entropy_hex": hex_of(&PREORDER_SALT),
            "salt_hex": hex_of(&PREORDER_SALT),
            "label": label2,
            "normalized_label": normalized_label2,
            "document_id_hex": hex_of(domain2_id.as_slice()),
            "signature_public_key_id": 1,
            "contested": false,
        })),
        "dashpay_profile_create": st_json(&st_profile_create, json!({
            "owner_id_hex": hex_of(owner.as_slice()),
            "identity_contract_nonce": 3,
            "entropy_hex": hex_of(&profile_entropy),
            "document_id_hex": hex_of(profile_id.as_slice()),
            "signature_public_key_id": 1,
            "display_name": "Alice в Wonderland",
            "public_message": "hello platform",
            "avatar_url": "https://example.com/a.png",
            "avatar_hash_hex": hex_of(&[0x88; 32]),
            "avatar_fingerprint_hex": hex_of(&[0x99; 8]),
        })),
        "dashpay_profile_replace": st_json(&st_profile_replace, json!({
            "owner_id_hex": hex_of(owner.as_slice()),
            "identity_contract_nonce": 4,
            "revision": 2,
            "document_id_hex": hex_of(profile_id.as_slice()),
            "signature_public_key_id": 1,
            "public_message": "updated message",
        })),
        "dashpay_contact_request": st_json(&st_contact, json!({
            "owner_id_hex": hex_of(owner.as_slice()),
            "identity_contract_nonce": 6,
            "entropy_hex": hex_of(&contact_entropy),
            "document_id_hex": hex_of(contact_id.as_slice()),
            "signature_public_key_id": 1,
            "to_user_id_hex": hex_of(to_user_id.as_slice()),
            "encrypted_public_key_hex": hex_of(&[0xab; 96]),
            "sender_key_index": 2,
            "recipient_key_index": 3,
            "account_reference": 0x0badc0deu32,
            "encrypted_account_label_hex": hex_of(&[0xcd; 48]),
        })),
        "contested_labels": contested_cases,
    });

    std::fs::create_dir_all(&out_dir).expect("create output dir");
    std::fs::write(
        format!("{}/dpp_identity_vectors.json", out_dir),
        serde_json::to_string_pretty(&identity_vectors).unwrap() + "\n",
    )
    .expect("write identity vectors");
    std::fs::write(
        format!("{}/dpp_st_vectors.json", out_dir),
        serde_json::to_string_pretty(&st_vectors).unwrap() + "\n",
    )
    .expect("write st vectors");
    println!(
        "wrote vectors for protocol version {} to {}",
        protocol_version, out_dir
    );
}
