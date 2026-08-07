// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! Dash Platform client internals for Dash Core, backed by the real
//! dashpay/platform crates:
//!
//! - `verify`: Drive/GroveDB proof verification for the DAPI queries the
//!   Platform GUI makes (replaces src/platform/drive/queries.cpp);
//! - `decode`: DPP decoders for identities and DPNS/DashPay documents
//!   (replaces src/platform/dpp/{identity,document}.cpp);
//! - `st`: DPP state transition construction with wallet-callback signing
//!   (replaces src/platform/dpp/statetransitions.cpp).
//!
//! The `#[cxx::bridge]` below exposes thin adapters over those modules.
//! Signing crosses the FFI as a digest callback (`WalletSigner`,
//! src/platform/ffi/signer.h) so private keys never leave the wallet.

pub mod decode;
pub mod st;
pub mod types;
pub mod verify;

use types::{BuiltTransition, ContestedResult, DocsResult, IdResult, KeyInfo, U64Result};

#[cxx::bridge(namespace = "platform_ffi")]
mod ffi {
    /// A byte vector, wrapped because cxx shared structs cannot hold
    /// Vec<Vec<u8>>.
    #[derive(Clone)]
    struct FfiBytes {
        data: Vec<u8>,
    }

    /// Proven root hash plus an optional u64 value; `present == false`
    /// means the value was cryptographically proven absent.
    struct FfiU64Result {
        root_hash: Vec<u8>,
        present: bool,
        value: u64,
    }

    /// One identity public key. `has_disabled_at == false` means the key is
    /// not disabled.
    #[derive(Clone)]
    struct FfiIdentityKey {
        id: u32,
        purpose: u8,
        security_level: u8,
        key_type: u8,
        read_only: bool,
        data: Vec<u8>,
        has_disabled_at: bool,
        disabled_at: u64,
    }

    /// Proven root hash plus the identity's keys; `present == false` means
    /// the identity was proven absent.
    struct FfiKeysResult {
        root_hash: Vec<u8>,
        present: bool,
        keys: Vec<FfiIdentityKey>,
    }

    /// Proven root hash plus an optional 32-byte identity id.
    struct FfiIdResult {
        root_hash: Vec<u8>,
        present: bool,
        id: Vec<u8>,
    }

    /// Proven root hash plus the matching serialized documents.
    struct FfiDocsResult {
        root_hash: Vec<u8>,
        documents: Vec<FfiBytes>,
    }

    /// One contender of a contested resource: identity id and its vote
    /// tally (when requested/available).
    struct FfiContender {
        identity: Vec<u8>,
        has_votes: bool,
        votes: u32,
    }

    /// Decoded contested-resource vote state.
    struct FfiContestedResult {
        root_hash: Vec<u8>,
        /// False when the contest was cryptographically proven absent.
        contest_found: bool,
        contenders: Vec<FfiContender>,
        has_abstain: bool,
        abstain_votes: u32,
        has_lock: bool,
        lock_votes: u32,
        /// True once the poll finished (awarded or locked); the tallies and
        /// contenders then come from the final vote event.
        finished: bool,
        locked: bool,
        has_winner: bool,
        winner: Vec<u8>,
        finished_at_time_ms: u64,
    }

    /// Decoded identity.
    struct FfiIdentity {
        id: Vec<u8>,
        balance: u64,
        revision: u64,
        keys: Vec<FfiIdentityKey>,
    }

    /// Decoded DPNS domain document.
    struct FfiDpnsName {
        label: String,
        normalized_label: String,
        parent_domain: String,
        identity: Vec<u8>,
        document_id: Vec<u8>,
        owner_id: Vec<u8>,
    }

    /// Decoded DashPay profile document. Empty vectors/strings and zero
    /// timestamps mean the field is absent.
    struct FfiProfile {
        document_id: Vec<u8>,
        owner_id: Vec<u8>,
        display_name: String,
        public_message: String,
        avatar_url: String,
        avatar_hash: Vec<u8>,
        avatar_fingerprint: Vec<u8>,
        created_at: u64,
        updated_at: u64,
        revision: u64,
    }

    /// Decoded DashPay contactRequest document.
    struct FfiContactRequest {
        owner_id: Vec<u8>,
        to_user_id: Vec<u8>,
        encrypted_public_key: Vec<u8>,
        sender_key_index: u32,
        recipient_key_index: u32,
        account_reference: u32,
        encrypted_account_label: Vec<u8>,
        core_height_created_at: u32,
        created_at: u64,
        document_id: Vec<u8>,
    }

    /// A public key to register with a new identity.
    struct FfiNewIdentityKey {
        id: u32,
        purpose: u8,
        security_level: u8,
        /// Compressed secp256k1 public key (33 bytes).
        pubkey: Vec<u8>,
    }

    /// A built, signed state transition. `hash` is sha256(bytes), the wait
    /// handle for waitForStateTransitionResult.
    struct FfiBuiltTransition {
        bytes: Vec<u8>,
        hash: Vec<u8>,
    }

    unsafe extern "C++" {
        include!("platform/ffi/signer.h");

        /// Wallet-backed signer. `SignDigestForKey` signs a 32-byte digest
        /// with the wallet key identified by `key_id` (`u32::MAX` selects
        /// the one-time asset-lock key of an identity registration) and
        /// returns a 65-byte compact recoverable ECDSA signature, or false
        /// on failure.
        type WalletSigner;
        fn SignDigestForKey(
            self: &WalletSigner,
            key_id: u32,
            digest: &[u8],
            sig_out: &mut Vec<u8>,
        ) -> bool;
    }

    extern "Rust" {
        // --- Drive proof verification -----------------------------------
        fn verify_identity_balance(proof: &[u8], identity_id: &[u8]) -> Result<FfiU64Result>;
        fn verify_identity_revision(proof: &[u8], identity_id: &[u8]) -> Result<FfiU64Result>;
        fn verify_identity_nonce(proof: &[u8], identity_id: &[u8]) -> Result<FfiU64Result>;
        fn verify_identity_contract_nonce(
            proof: &[u8],
            identity_id: &[u8],
            contract_id: &[u8],
        ) -> Result<FfiU64Result>;
        fn verify_identity_keys(proof: &[u8], identity_id: &[u8]) -> Result<FfiKeysResult>;
        fn verify_identity_id_by_pubkey_hash(
            proof: &[u8],
            pubkey_hash: &[u8],
        ) -> Result<FfiIdResult>;
        fn verify_dpns_name_exact(proof: &[u8], normalized_label: &str) -> Result<FfiDocsResult>;
        fn verify_dpns_name_prefix(
            proof: &[u8],
            normalized_prefix: &str,
            limit: u16,
        ) -> Result<FfiDocsResult>;
        fn verify_dpns_names_by_identity(
            proof: &[u8],
            identity_id: &[u8],
            limit: u16,
        ) -> Result<FfiDocsResult>;
        fn verify_dashpay_profile_by_owner(
            proof: &[u8],
            owner_id: &[u8],
        ) -> Result<FfiDocsResult>;
        fn verify_dashpay_contact_requests(
            proof: &[u8],
            identity_id: &[u8],
            to_identity: bool,
            limit: u16,
        ) -> Result<FfiDocsResult>;
        fn verify_contested_vote_state(
            proof: &[u8],
            contract_id: &[u8],
            document_type: &str,
            index_values: Vec<String>,
            count: u16,
        ) -> Result<FfiContestedResult>;

        // --- DPP decoders -----------------------------------------------
        fn decode_identity(bytes: &[u8]) -> Result<FfiIdentity>;
        fn decode_identity_public_key(bytes: &[u8]) -> Result<FfiIdentityKey>;
        fn decode_dpns_domain(doc_bytes: &[u8]) -> Result<FfiDpnsName>;
        fn decode_dashpay_profile(doc_bytes: &[u8]) -> Result<FfiProfile>;
        fn decode_contact_request(doc_bytes: &[u8]) -> Result<FfiContactRequest>;

        // --- State transitions ------------------------------------------
        fn st_build_dpns_preorder(
            identity_id: &[u8],
            identity_contract_nonce: u64,
            salted_domain_hash: &[u8],
            signature_public_key_id: u32,
            key: FfiIdentityKey,
            entropy: &[u8],
            signer: &WalletSigner,
        ) -> Result<FfiBuiltTransition>;
        fn st_build_dpns_domain(
            identity_id: &[u8],
            identity_contract_nonce: u64,
            label: &str,
            normalized_label: &str,
            parent_domain: &str,
            preorder_salt: &[u8],
            entropy: &[u8],
            signature_public_key_id: u32,
            key: FfiIdentityKey,
            signer: &WalletSigner,
        ) -> Result<FfiBuiltTransition>;
        fn st_build_profile(
            identity_id: &[u8],
            identity_contract_nonce: u64,
            display_name: &str,
            public_message: &str,
            avatar_url: &str,
            avatar_hash: &[u8],
            avatar_fingerprint: &[u8],
            revision: u64,
            has_existing_doc_id: bool,
            existing_document_id: &[u8],
            entropy: &[u8],
            signature_public_key_id: u32,
            key: FfiIdentityKey,
            signer: &WalletSigner,
        ) -> Result<FfiBuiltTransition>;
        fn st_build_contact_request(
            identity_id: &[u8],
            identity_contract_nonce: u64,
            to_user_id: &[u8],
            encrypted_public_key: &[u8],
            sender_key_index: u32,
            recipient_key_index: u32,
            account_reference: u32,
            encrypted_account_label: &[u8],
            entropy: &[u8],
            signature_public_key_id: u32,
            key: FfiIdentityKey,
            signer: &WalletSigner,
        ) -> Result<FfiBuiltTransition>;
        fn st_build_identity_create(
            is_instant: bool,
            transaction: &[u8],
            instant_lock: &[u8],
            output_index: u32,
            core_chain_locked_height: u32,
            out_point: &[u8],
            keys: Vec<FfiNewIdentityKey>,
            signer: &WalletSigner,
        ) -> Result<FfiBuiltTransition>;
    }
}

// ---------------------------------------------------------------------------
// Conversions between the plain-Rust core types and the flat FFI structs.
// ---------------------------------------------------------------------------

fn ffi_u64(result: U64Result) -> ffi::FfiU64Result {
    ffi::FfiU64Result {
        root_hash: result.root_hash.to_vec(),
        present: result.value.is_some(),
        value: result.value.unwrap_or(0),
    }
}

fn ffi_key(key: &KeyInfo) -> ffi::FfiIdentityKey {
    ffi::FfiIdentityKey {
        id: key.id,
        purpose: key.purpose,
        security_level: key.security_level,
        key_type: key.key_type,
        read_only: key.read_only,
        data: key.data.clone(),
        has_disabled_at: key.disabled_at.is_some(),
        disabled_at: key.disabled_at.unwrap_or(0),
    }
}

fn key_info(key: &ffi::FfiIdentityKey) -> KeyInfo {
    KeyInfo {
        id: key.id,
        purpose: key.purpose,
        security_level: key.security_level,
        key_type: key.key_type,
        read_only: key.read_only,
        data: key.data.clone(),
        disabled_at: key.has_disabled_at.then_some(key.disabled_at),
    }
}

fn ffi_docs(result: DocsResult) -> ffi::FfiDocsResult {
    ffi::FfiDocsResult {
        root_hash: result.root_hash.to_vec(),
        documents: result
            .documents
            .into_iter()
            .map(|data| ffi::FfiBytes { data })
            .collect(),
    }
}

fn ffi_id_result(result: IdResult) -> ffi::FfiIdResult {
    ffi::FfiIdResult {
        root_hash: result.root_hash.to_vec(),
        present: result.id.is_some(),
        id: result.id.map(|id| id.to_vec()).unwrap_or_default(),
    }
}

fn ffi_contested(result: ContestedResult) -> ffi::FfiContestedResult {
    let state = result.state;
    ffi::FfiContestedResult {
        root_hash: result.root_hash.to_vec(),
        contest_found: state.contest_found,
        contenders: state
            .contenders
            .iter()
            .map(|(identity, votes)| ffi::FfiContender {
                identity: identity.to_vec(),
                has_votes: votes.is_some(),
                votes: votes.unwrap_or(0),
            })
            .collect(),
        has_abstain: state.abstain_votes.is_some(),
        abstain_votes: state.abstain_votes.unwrap_or(0),
        has_lock: state.lock_votes.is_some(),
        lock_votes: state.lock_votes.unwrap_or(0),
        finished: state.finished,
        locked: state.locked,
        has_winner: state.winner.is_some(),
        winner: state.winner.map(|id| id.to_vec()).unwrap_or_default(),
        finished_at_time_ms: state.finished_at_time_ms,
    }
}

fn ffi_built(built: BuiltTransition) -> ffi::FfiBuiltTransition {
    ffi::FfiBuiltTransition {
        bytes: built.bytes,
        hash: built.hash.to_vec(),
    }
}

// ---------------------------------------------------------------------------
// Bridge implementations.
// ---------------------------------------------------------------------------

fn verify_identity_balance(proof: &[u8], identity_id: &[u8]) -> Result<ffi::FfiU64Result, String> {
    verify::verify_identity_balance(proof, identity_id).map(ffi_u64)
}

fn verify_identity_revision(proof: &[u8], identity_id: &[u8]) -> Result<ffi::FfiU64Result, String> {
    verify::verify_identity_revision(proof, identity_id).map(ffi_u64)
}

fn verify_identity_nonce(proof: &[u8], identity_id: &[u8]) -> Result<ffi::FfiU64Result, String> {
    verify::verify_identity_nonce(proof, identity_id).map(ffi_u64)
}

fn verify_identity_contract_nonce(
    proof: &[u8],
    identity_id: &[u8],
    contract_id: &[u8],
) -> Result<ffi::FfiU64Result, String> {
    verify::verify_identity_contract_nonce(proof, identity_id, contract_id).map(ffi_u64)
}

fn verify_identity_keys(proof: &[u8], identity_id: &[u8]) -> Result<ffi::FfiKeysResult, String> {
    let result = verify::verify_identity_keys(proof, identity_id)?;
    Ok(ffi::FfiKeysResult {
        root_hash: result.root_hash.to_vec(),
        present: result.keys.is_some(),
        keys: result
            .keys
            .unwrap_or_default()
            .iter()
            .map(ffi_key)
            .collect(),
    })
}

fn verify_identity_id_by_pubkey_hash(
    proof: &[u8],
    pubkey_hash: &[u8],
) -> Result<ffi::FfiIdResult, String> {
    verify::verify_identity_id_by_pubkey_hash(proof, pubkey_hash).map(ffi_id_result)
}

fn verify_dpns_name_exact(
    proof: &[u8],
    normalized_label: &str,
) -> Result<ffi::FfiDocsResult, String> {
    verify::verify_dpns_name_exact(proof, normalized_label).map(ffi_docs)
}

fn verify_dpns_name_prefix(
    proof: &[u8],
    normalized_prefix: &str,
    limit: u16,
) -> Result<ffi::FfiDocsResult, String> {
    verify::verify_dpns_name_prefix(proof, normalized_prefix, limit).map(ffi_docs)
}

fn verify_dpns_names_by_identity(
    proof: &[u8],
    identity_id: &[u8],
    limit: u16,
) -> Result<ffi::FfiDocsResult, String> {
    verify::verify_dpns_names_by_identity(proof, identity_id, limit).map(ffi_docs)
}

fn verify_dashpay_profile_by_owner(
    proof: &[u8],
    owner_id: &[u8],
) -> Result<ffi::FfiDocsResult, String> {
    verify::verify_dashpay_profile_by_owner(proof, owner_id).map(ffi_docs)
}

fn verify_dashpay_contact_requests(
    proof: &[u8],
    identity_id: &[u8],
    to_identity: bool,
    limit: u16,
) -> Result<ffi::FfiDocsResult, String> {
    verify::verify_dashpay_contact_requests(proof, identity_id, to_identity, limit).map(ffi_docs)
}

fn verify_contested_vote_state(
    proof: &[u8],
    contract_id: &[u8],
    document_type: &str,
    index_values: Vec<String>,
    count: u16,
) -> Result<ffi::FfiContestedResult, String> {
    verify::verify_contested_vote_state(proof, contract_id, document_type, index_values, count)
        .map(ffi_contested)
}

fn decode_identity(bytes: &[u8]) -> Result<ffi::FfiIdentity, String> {
    let identity = decode::decode_identity(bytes)?;
    Ok(ffi::FfiIdentity {
        id: identity.id.to_vec(),
        balance: identity.balance,
        revision: identity.revision,
        keys: identity.keys.iter().map(ffi_key).collect(),
    })
}

fn decode_identity_public_key(bytes: &[u8]) -> Result<ffi::FfiIdentityKey, String> {
    decode::decode_identity_public_key(bytes).map(|key| ffi_key(&key))
}

fn decode_dpns_domain(doc_bytes: &[u8]) -> Result<ffi::FfiDpnsName, String> {
    let name = decode::decode_dpns_domain(doc_bytes)?;
    Ok(ffi::FfiDpnsName {
        label: name.label,
        normalized_label: name.normalized_label,
        parent_domain: name.parent_domain,
        identity: name.identity.to_vec(),
        document_id: name.document_id.to_vec(),
        owner_id: name.owner_id.to_vec(),
    })
}

fn decode_dashpay_profile(doc_bytes: &[u8]) -> Result<ffi::FfiProfile, String> {
    let profile = decode::decode_dashpay_profile(doc_bytes)?;
    Ok(ffi::FfiProfile {
        document_id: profile.document_id.to_vec(),
        owner_id: profile.owner_id.to_vec(),
        display_name: profile.display_name,
        public_message: profile.public_message,
        avatar_url: profile.avatar_url,
        avatar_hash: profile.avatar_hash,
        avatar_fingerprint: profile.avatar_fingerprint,
        created_at: profile.created_at,
        updated_at: profile.updated_at,
        revision: profile.revision,
    })
}

fn decode_contact_request(doc_bytes: &[u8]) -> Result<ffi::FfiContactRequest, String> {
    let request = decode::decode_contact_request(doc_bytes)?;
    Ok(ffi::FfiContactRequest {
        owner_id: request.owner_id.to_vec(),
        to_user_id: request.to_user_id.to_vec(),
        encrypted_public_key: request.encrypted_public_key,
        sender_key_index: request.sender_key_index,
        recipient_key_index: request.recipient_key_index,
        account_reference: request.account_reference,
        encrypted_account_label: request.encrypted_account_label,
        core_height_created_at: request.core_height_created_at,
        created_at: request.created_at,
        document_id: request.document_id.to_vec(),
    })
}

/// Shareable handle to the C++ signer. The bridge functions run the async
/// dpp builders to completion on the calling thread with a local executor,
/// so the signer is never actually accessed from another thread; the
/// `Send + Sync` assertion only satisfies dpp's `Signer: Send + Sync`
/// bound.
struct SignerHandle<'a>(&'a ffi::WalletSigner);
unsafe impl Send for SignerHandle<'_> {}
unsafe impl Sync for SignerHandle<'_> {}

impl SignerHandle<'_> {
    fn sign(&self, key_id: u32, digest: [u8; 32]) -> Option<Vec<u8>> {
        let mut signature = Vec::new();
        self.0
            .SignDigestForKey(key_id, &digest, &mut signature)
            .then_some(signature)
    }
}

fn check_key_id(signature_public_key_id: u32, key: &ffi::FfiIdentityKey) -> Result<(), String> {
    if signature_public_key_id != key.id {
        return Err(format!(
            "signature public key id {signature_public_key_id} does not match key id {}",
            key.id
        ));
    }
    Ok(())
}

fn st_build_dpns_preorder(
    identity_id: &[u8],
    identity_contract_nonce: u64,
    salted_domain_hash: &[u8],
    signature_public_key_id: u32,
    key: ffi::FfiIdentityKey,
    entropy: &[u8],
    signer: &ffi::WalletSigner,
) -> Result<ffi::FfiBuiltTransition, String> {
    check_key_id(signature_public_key_id, &key)?;
    let handle = SignerHandle(signer);
    let sign_fn = |key_id: u32, digest: [u8; 32]| handle.sign(key_id, digest);
    st::build_dpns_preorder(
        identity_id,
        identity_contract_nonce,
        salted_domain_hash,
        &key_info(&key),
        entropy,
        &sign_fn,
    )
    .map(ffi_built)
}

#[allow(clippy::too_many_arguments)]
fn st_build_dpns_domain(
    identity_id: &[u8],
    identity_contract_nonce: u64,
    label: &str,
    normalized_label: &str,
    parent_domain: &str,
    preorder_salt: &[u8],
    entropy: &[u8],
    signature_public_key_id: u32,
    key: ffi::FfiIdentityKey,
    signer: &ffi::WalletSigner,
) -> Result<ffi::FfiBuiltTransition, String> {
    check_key_id(signature_public_key_id, &key)?;
    let handle = SignerHandle(signer);
    let sign_fn = |key_id: u32, digest: [u8; 32]| handle.sign(key_id, digest);
    st::build_dpns_domain(
        identity_id,
        identity_contract_nonce,
        label,
        normalized_label,
        parent_domain,
        preorder_salt,
        entropy,
        &key_info(&key),
        &sign_fn,
    )
    .map(ffi_built)
}

#[allow(clippy::too_many_arguments)]
fn st_build_profile(
    identity_id: &[u8],
    identity_contract_nonce: u64,
    display_name: &str,
    public_message: &str,
    avatar_url: &str,
    avatar_hash: &[u8],
    avatar_fingerprint: &[u8],
    revision: u64,
    has_existing_doc_id: bool,
    existing_document_id: &[u8],
    entropy: &[u8],
    signature_public_key_id: u32,
    key: ffi::FfiIdentityKey,
    signer: &ffi::WalletSigner,
) -> Result<ffi::FfiBuiltTransition, String> {
    check_key_id(signature_public_key_id, &key)?;
    let handle = SignerHandle(signer);
    let sign_fn = |key_id: u32, digest: [u8; 32]| handle.sign(key_id, digest);
    st::build_profile(
        identity_id,
        identity_contract_nonce,
        display_name,
        public_message,
        avatar_url,
        avatar_hash,
        avatar_fingerprint,
        revision,
        has_existing_doc_id.then_some(existing_document_id),
        entropy,
        &key_info(&key),
        &sign_fn,
    )
    .map(ffi_built)
}

#[allow(clippy::too_many_arguments)]
fn st_build_contact_request(
    identity_id: &[u8],
    identity_contract_nonce: u64,
    to_user_id: &[u8],
    encrypted_public_key: &[u8],
    sender_key_index: u32,
    recipient_key_index: u32,
    account_reference: u32,
    encrypted_account_label: &[u8],
    entropy: &[u8],
    signature_public_key_id: u32,
    key: ffi::FfiIdentityKey,
    signer: &ffi::WalletSigner,
) -> Result<ffi::FfiBuiltTransition, String> {
    check_key_id(signature_public_key_id, &key)?;
    let handle = SignerHandle(signer);
    let sign_fn = |key_id: u32, digest: [u8; 32]| handle.sign(key_id, digest);
    st::build_contact_request(
        identity_id,
        identity_contract_nonce,
        to_user_id,
        encrypted_public_key,
        sender_key_index,
        recipient_key_index,
        account_reference,
        encrypted_account_label,
        entropy,
        &key_info(&key),
        &sign_fn,
    )
    .map(ffi_built)
}

#[allow(clippy::too_many_arguments)]
fn st_build_identity_create(
    is_instant: bool,
    transaction: &[u8],
    instant_lock: &[u8],
    output_index: u32,
    core_chain_locked_height: u32,
    out_point: &[u8],
    keys: Vec<ffi::FfiNewIdentityKey>,
    signer: &ffi::WalletSigner,
) -> Result<ffi::FfiBuiltTransition, String> {
    let proof = if is_instant {
        st::AssetLockProofInput::Instant {
            transaction: transaction.to_vec(),
            instant_lock: instant_lock.to_vec(),
            output_index,
        }
    } else {
        st::AssetLockProofInput::Chain {
            core_chain_locked_height,
            out_point: out_point
                .try_into()
                .map_err(|_| format!("outpoint must be 36 bytes, got {}", out_point.len()))?,
        }
    };
    let keys: Vec<st::NewIdentityKey> = keys
        .into_iter()
        .map(|key| st::NewIdentityKey {
            id: key.id,
            purpose: key.purpose,
            security_level: key.security_level,
            pubkey: key.pubkey,
        })
        .collect();
    let handle = SignerHandle(signer);
    let sign_fn = |key_id: u32, digest: [u8; 32]| handle.sign(key_id, digest);
    st::build_identity_create(proof, &keys, &sign_fn).map(ffi_built)
}
