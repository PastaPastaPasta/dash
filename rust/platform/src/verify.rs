// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! Drive proof verification for the DAPI queries the dash-qt Platform GUI
//! makes, mirroring src/platform/drive/queries.{h,cpp} but implemented on
//! top of the real rs-drive verifiers. Each function verifies a GroveDB
//! proof against the exact query shape the server executed and returns the
//! proven root hash plus typed results; `None` means proven absent.

use std::collections::BTreeMap;

use dpp::data_contract::accessors::v0::DataContractV0Getters;
use dpp::data_contract::DataContract;
use dpp::identity::identity_public_key::accessors::v0::IdentityPublicKeyGettersV0;
use dpp::identity::IdentityPublicKey;
use dpp::platform_value::Value;
use dpp::system_data_contracts::{load_system_data_contract, SystemDataContract};
use dpp::voting::vote_info_storage::contested_document_vote_poll_winner_info::ContestedDocumentVotePollWinnerInfo;
use dpp::voting::vote_polls::contested_document_resource_vote_poll::ContestedDocumentResourceVotePoll;
use drive::drive::identity::key::fetch::IdentityKeysRequest;
use drive::drive::Drive;
use drive::query::vote_poll_vote_state_query::{
    ContestedDocumentVotePollDriveQuery, ContestedDocumentVotePollDriveQueryResultType,
};
use drive::query::{DriveDocumentQuery, InternalClauses, OrderClause, WhereClause, WhereOperator};
use indexmap::IndexMap;
use platform_version::version::PlatformVersion;

use crate::types::{
    id32, ContestedResult, ContestedVoteState, DocsResult, IdResult, KeyInfo, KeysResult, U64Result,
};

/// The contested unique index of the DPNS `domain` document type.
pub const CONTESTED_INDEX_NAME: &str = "parentNameAndLabel";

fn key_info(key: &IdentityPublicKey) -> KeyInfo {
    KeyInfo {
        id: key.id(),
        purpose: key.purpose() as u8,
        security_level: key.security_level() as u8,
        key_type: key.key_type() as u8,
        read_only: key.read_only(),
        data: key.data().to_vec(),
        disabled_at: key.disabled_at(),
    }
}

pub fn verify_identity_balance(proof: &[u8], identity_id: &[u8]) -> Result<U64Result, String> {
    let id = id32(identity_id, "identity id")?;
    let (root_hash, value) = Drive::verify_identity_balance_for_identity_id(
        proof,
        id,
        false,
        PlatformVersion::latest(),
    )
    .map_err(|e| format!("identity balance proof verification failed: {e}"))?;
    Ok(U64Result { root_hash, value })
}

pub fn verify_identity_revision(proof: &[u8], identity_id: &[u8]) -> Result<U64Result, String> {
    let id = id32(identity_id, "identity id")?;
    let (root_hash, value) = Drive::verify_identity_revision_for_identity_id(
        proof,
        id,
        false,
        PlatformVersion::latest(),
    )
    .map_err(|e| format!("identity revision proof verification failed: {e}"))?;
    Ok(U64Result { root_hash, value })
}

pub fn verify_identity_nonce(proof: &[u8], identity_id: &[u8]) -> Result<U64Result, String> {
    let id = id32(identity_id, "identity id")?;
    let (root_hash, value) =
        Drive::verify_identity_nonce(proof, id, false, PlatformVersion::latest())
            .map_err(|e| format!("identity nonce proof verification failed: {e}"))?;
    Ok(U64Result { root_hash, value })
}

pub fn verify_identity_contract_nonce(
    proof: &[u8],
    identity_id: &[u8],
    contract_id: &[u8],
) -> Result<U64Result, String> {
    let id = id32(identity_id, "identity id")?;
    let contract = id32(contract_id, "contract id")?;
    let (root_hash, value) = Drive::verify_identity_contract_nonce(
        proof,
        id,
        contract,
        false,
        PlatformVersion::latest(),
    )
    .map_err(|e| format!("identity contract nonce proof verification failed: {e}"))?;
    Ok(U64Result { root_hash, value })
}

pub fn verify_identity_keys(proof: &[u8], identity_id: &[u8]) -> Result<KeysResult, String> {
    let id = id32(identity_id, "identity id")?;
    let request = IdentityKeysRequest::new_all_keys_query(&id, None);
    let (root_hash, partial) = Drive::verify_identity_keys_by_identity_id(
        proof,
        request,
        false,
        false,
        false,
        PlatformVersion::latest(),
    )
    .map_err(|e| format!("identity keys proof verification failed: {e}"))?;
    let keys = partial.map(|identity| {
        identity
            .loaded_public_keys
            .values()
            .map(key_info)
            .collect::<Vec<_>>()
    });
    Ok(KeysResult { root_hash, keys })
}

pub fn verify_identity_id_by_pubkey_hash(
    proof: &[u8],
    pubkey_hash: &[u8],
) -> Result<IdResult, String> {
    let hash: [u8; 20] = pubkey_hash
        .try_into()
        .map_err(|_| format!("public key hash must be 20 bytes, got {}", pubkey_hash.len()))?;
    let (root_hash, id) = Drive::verify_identity_id_by_unique_public_key_hash(
        proof,
        false,
        hash,
        PlatformVersion::latest(),
    )
    .map_err(|e| format!("identity-by-public-key-hash proof verification failed: {e}"))?;
    Ok(IdResult { root_hash, id })
}

// ---------------------------------------------------------------------------
// Document queries. The constructed DriveDocumentQuery must match the query
// the server executed for the proof to verify; the shapes below mirror the
// C++ transport requests (src/platform/transport/client.cpp) and are pinned
// by the drive_query_vectors.json acceptance vectors.
// ---------------------------------------------------------------------------

fn load_contract(contract: SystemDataContract) -> Result<DataContract, String> {
    load_system_data_contract(contract, PlatformVersion::latest())
        .map_err(|e| format!("unable to load system data contract: {e}"))
}

fn equal_clause(field: &str, value: Value) -> (String, WhereClause) {
    (
        field.to_string(),
        WhereClause {
            field: field.to_string(),
            operator: WhereOperator::Equal,
            value,
        },
    )
}

fn asc_order(field: &str) -> IndexMap<String, OrderClause> {
    let mut order_by = IndexMap::new();
    order_by.insert(
        field.to_string(),
        OrderClause {
            field: field.to_string(),
            ascending: true,
        },
    );
    order_by
}

/// Runs a document query proof through drive and keeps the documents
/// serialized.
fn run_document_query(
    proof: &[u8],
    contract: &DataContract,
    document_type_name: &str,
    internal_clauses: InternalClauses,
    order_by: IndexMap<String, OrderClause>,
    limit: Option<u16>,
) -> Result<DocsResult, String> {
    let document_type = contract
        .document_type_for_name(document_type_name)
        .map_err(|e| format!("unknown document type {document_type_name}: {e}"))?;
    let query = DriveDocumentQuery {
        contract,
        document_type,
        internal_clauses,
        offset: None,
        limit,
        order_by,
        start_at: None,
        start_at_included: false,
        block_time_ms: None,
    };
    let (root_hash, documents) = query
        .verify_proof_keep_serialized(proof, PlatformVersion::latest())
        .map_err(|e| format!("document proof verification failed: {e}"))?;
    Ok(DocsResult {
        root_hash,
        documents,
    })
}

pub fn verify_dpns_name_exact(proof: &[u8], normalized_label: &str) -> Result<DocsResult, String> {
    let contract = load_contract(SystemDataContract::DPNS)?;
    let internal_clauses = InternalClauses {
        equal_clauses: BTreeMap::from([
            equal_clause("normalizedParentDomainName", Value::Text("dash".to_string())),
            equal_clause("normalizedLabel", Value::Text(normalized_label.to_string())),
        ]),
        ..Default::default()
    };
    run_document_query(
        proof,
        &contract,
        "domain",
        internal_clauses,
        IndexMap::new(),
        Some(1),
    )
}

pub fn verify_dpns_name_prefix(
    proof: &[u8],
    normalized_prefix: &str,
    limit: u16,
) -> Result<DocsResult, String> {
    if normalized_prefix.is_empty() || normalized_prefix.as_bytes().last() == Some(&0xff) {
        return Err("invalid DPNS prefix".to_string());
    }
    let contract = load_contract(SystemDataContract::DPNS)?;
    let internal_clauses = InternalClauses {
        equal_clauses: BTreeMap::from([equal_clause(
            "normalizedParentDomainName",
            Value::Text("dash".to_string()),
        )]),
        range_clause: Some(WhereClause {
            field: "normalizedLabel".to_string(),
            operator: WhereOperator::StartsWith,
            value: Value::Text(normalized_prefix.to_string()),
        }),
        ..Default::default()
    };
    run_document_query(
        proof,
        &contract,
        "domain",
        internal_clauses,
        asc_order("normalizedLabel"),
        Some(limit),
    )
}

pub fn verify_dpns_names_by_identity(
    proof: &[u8],
    identity_id: &[u8],
    limit: u16,
) -> Result<DocsResult, String> {
    let id = id32(identity_id, "identity id")?;
    let contract = load_contract(SystemDataContract::DPNS)?;
    let internal_clauses = InternalClauses {
        equal_clauses: BTreeMap::from([equal_clause("records.identity", Value::Identifier(id))]),
        ..Default::default()
    };
    run_document_query(
        proof,
        &contract,
        "domain",
        internal_clauses,
        IndexMap::new(),
        Some(limit),
    )
}

pub fn verify_dashpay_profile_by_owner(
    proof: &[u8],
    owner_id: &[u8],
) -> Result<DocsResult, String> {
    let id = id32(owner_id, "owner id")?;
    let contract = load_contract(SystemDataContract::Dashpay)?;
    let internal_clauses = InternalClauses {
        equal_clauses: BTreeMap::from([equal_clause("$ownerId", Value::Identifier(id))]),
        ..Default::default()
    };
    run_document_query(
        proof,
        &contract,
        "profile",
        internal_clauses,
        IndexMap::new(),
        Some(1),
    )
}

pub fn verify_dashpay_contact_requests(
    proof: &[u8],
    identity_id: &[u8],
    to_identity: bool,
    limit: u16,
) -> Result<DocsResult, String> {
    let id = id32(identity_id, "identity id")?;
    let contract = load_contract(SystemDataContract::Dashpay)?;
    let field = if to_identity { "toUserId" } else { "$ownerId" };
    let internal_clauses = InternalClauses {
        equal_clauses: BTreeMap::from([equal_clause(field, Value::Identifier(id))]),
        ..Default::default()
    };
    run_document_query(
        proof,
        &contract,
        "contactRequest",
        internal_clauses,
        asc_order("$createdAt"),
        Some(limit),
    )
}

// ---------------------------------------------------------------------------
// FromProof-driven verification over (request bytes, response bytes).
//
// The C++ transport hands over the exact protobuf request it sent and the
// full protobuf response it received; drive-proof-verifier reconstructs the
// query from the request, replays the GroveDB proof, and verifies the
// Tenderdash quorum threshold signature against the quorum keys served by
// crate::provider. The returned Meta fields are authenticated by that
// signature.
// ---------------------------------------------------------------------------

use dapi_grpc::platform::v0::get_documents_request::Version as GetDocumentsVersion;
use dapi_grpc::platform::v0::{
    GetContestedResourceVoteStateRequest, GetContestedResourceVoteStateResponse,
    GetDocumentsRequest, GetDocumentsResponse, GetIdentityBalanceRequest,
    GetIdentityBalanceResponse, GetIdentityByPublicKeyHashRequest,
    GetIdentityByPublicKeyHashResponse, GetIdentityContractNonceRequest,
    GetIdentityContractNonceResponse, GetIdentityKeysRequest, GetIdentityKeysResponse,
    GetIdentityNonceRequest, GetIdentityNonceResponse, GetIdentityRequest, GetIdentityResponse,
    ResponseMetadata,
};
use dapi_grpc::platform::VersionedGrpcResponse;
use dash_context_provider::ContextProvider as _;
use dash_platform_queries::documents::document_query::verify_documents_response;
use dpp::document::serialization_traits::DocumentPlatformConversionMethodsV0;
use dpp::identity::Identity;
use dpp::voting::vote_info_storage::contested_document_vote_poll_winner_info::ContestedDocumentVotePollWinnerInfo as WinnerInfo;
use drive_proof_verifier::types::{
    Contenders, IdentityBalance, IdentityContractNonceFetcher, IdentityNonceFetcher,
    IdentityPublicKeys,
};
use drive_proof_verifier::FromProof;
use prost::Message;

use crate::decode::identity_info;
use crate::provider::{network, provider};
use crate::types::{IdentityInfo, Meta};

fn decode_message<T: Message + Default>(bytes: &[u8], what: &str) -> Result<T, String> {
    T::decode(bytes).map_err(|e| format!("unable to decode {what}: {e}"))
}

fn meta_from(mtd: &ResponseMetadata) -> Meta {
    Meta {
        height: mtd.height,
        core_chain_locked_height: mtd.core_chain_locked_height,
        time_ms: mtd.time_ms,
        protocol_version: mtd.protocol_version,
        chain_id: mtd.chain_id.clone(),
    }
}

/// The platform version the response claims to be produced under, falling
/// back to the crate's latest known version for responses from a newer
/// server. The claimed version is authenticated after the fact: it enters
/// the signed StateId, so a lie fails the quorum signature check.
fn response_version<R: VersionedGrpcResponse>(response: &R) -> &'static PlatformVersion
where
    <R as VersionedGrpcResponse>::Error: std::fmt::Display,
{
    response
        .metadata()
        .ok()
        .and_then(|mtd| PlatformVersion::get(mtd.protocol_version).ok())
        .unwrap_or_else(PlatformVersion::latest)
}

pub fn verify_get_identity_nonce(
    request: &[u8],
    response: &[u8],
) -> Result<(Option<u64>, Meta), String> {
    let request: GetIdentityNonceRequest = decode_message(request, "GetIdentityNonceRequest")?;
    let response: GetIdentityNonceResponse = decode_message(response, "GetIdentityNonceResponse")?;
    let version = response_version(&response);
    let (nonce, mtd, _) = IdentityNonceFetcher::maybe_from_proof_with_metadata(
        request,
        response,
        network()?,
        version,
        provider(),
    )
    .map_err(|e| format!("identity nonce proof verification failed: {e}"))?;
    Ok((nonce.map(|fetcher| fetcher.0), meta_from(&mtd)))
}

pub fn verify_get_identity_contract_nonce(
    request: &[u8],
    response: &[u8],
) -> Result<(Option<u64>, Meta), String> {
    let request: GetIdentityContractNonceRequest =
        decode_message(request, "GetIdentityContractNonceRequest")?;
    let response: GetIdentityContractNonceResponse =
        decode_message(response, "GetIdentityContractNonceResponse")?;
    let version = response_version(&response);
    let (nonce, mtd, _) = IdentityContractNonceFetcher::maybe_from_proof_with_metadata(
        request,
        response,
        network()?,
        version,
        provider(),
    )
    .map_err(|e| format!("identity contract nonce proof verification failed: {e}"))?;
    Ok((nonce.map(|fetcher| fetcher.0), meta_from(&mtd)))
}

/// getIdentity: one proof covering balance, revision and keys
/// (Drive::verify_full_identity_by_identity_id). `None` = proven absent.
pub fn verify_get_identity(
    request: &[u8],
    response: &[u8],
) -> Result<(Option<IdentityInfo>, Meta), String> {
    let request: GetIdentityRequest = decode_message(request, "GetIdentityRequest")?;
    let response: GetIdentityResponse = decode_message(response, "GetIdentityResponse")?;
    let version = response_version(&response);
    let (identity, mtd, _) = <Identity as FromProof<GetIdentityRequest>>::maybe_from_proof_with_metadata(
        request,
        response,
        network()?,
        version,
        provider(),
    )
    .map_err(|e| format!("identity proof verification failed: {e}"))?;
    Ok((identity.as_ref().map(identity_info), meta_from(&mtd)))
}

/// getIdentityByPublicKeyHash: one proof resolving the unique key hash to
/// the full identity. `None` = no identity registered that key hash.
pub fn verify_get_identity_by_pubkey_hash(
    request: &[u8],
    response: &[u8],
) -> Result<(Option<IdentityInfo>, Meta), String> {
    let request: GetIdentityByPublicKeyHashRequest =
        decode_message(request, "GetIdentityByPublicKeyHashRequest")?;
    let response: GetIdentityByPublicKeyHashResponse =
        decode_message(response, "GetIdentityByPublicKeyHashResponse")?;
    let version = response_version(&response);
    let (identity, mtd, _) =
        <Identity as FromProof<GetIdentityByPublicKeyHashRequest>>::maybe_from_proof_with_metadata(
            request,
            response,
            network()?,
            version,
            provider(),
        )
        .map_err(|e| format!("identity-by-public-key-hash proof verification failed: {e}"))?;
    Ok((identity.as_ref().map(identity_info), meta_from(&mtd)))
}

/// getIdentityBalance. Not bridged to C++ (the client fetches balances via
/// the full identity); exercised by the vector tests to pin the shared
/// grovedb + quorum-signature pipeline against the fixture corpus.
pub fn verify_get_identity_balance(
    request: &[u8],
    response: &[u8],
) -> Result<(Option<u64>, Meta), String> {
    let request: GetIdentityBalanceRequest = decode_message(request, "GetIdentityBalanceRequest")?;
    let response: GetIdentityBalanceResponse =
        decode_message(response, "GetIdentityBalanceResponse")?;
    let version = response_version(&response);
    let (balance, mtd, _) = IdentityBalance::maybe_from_proof_with_metadata(
        request,
        response,
        network()?,
        version,
        provider(),
    )
    .map_err(|e| format!("identity balance proof verification failed: {e}"))?;
    Ok((balance, meta_from(&mtd)))
}

/// getIdentityKeys (all keys). Not bridged to C++ (see
/// `verify_get_identity_balance`).
pub fn verify_get_identity_keys(
    request: &[u8],
    response: &[u8],
) -> Result<(Option<Vec<KeyInfo>>, Meta), String> {
    let request: GetIdentityKeysRequest = decode_message(request, "GetIdentityKeysRequest")?;
    let response: GetIdentityKeysResponse = decode_message(response, "GetIdentityKeysResponse")?;
    let version = response_version(&response);
    let (keys, mtd, _) = IdentityPublicKeys::maybe_from_proof_with_metadata(
        request,
        response,
        network()?,
        version,
        provider(),
    )
    .map_err(|e| format!("identity keys proof verification failed: {e}"))?;
    let keys = keys.map(|keys| {
        keys.values()
            .flatten()
            .map(crate::decode::key_info)
            .collect::<Vec<_>>()
    });
    Ok((keys, meta_from(&mtd)))
}

/// getDocuments: reconstructs the query from the request
/// (DocumentQuery::try_from_request), verifies the proof, and returns the
/// matched documents re-serialized in platform form (the input to the
/// decode_* functions). Absence (no matching documents) is an empty vector.
pub fn verify_get_documents(
    request: &[u8],
    response: &[u8],
) -> Result<(Vec<Vec<u8>>, Meta), String> {
    let request: GetDocumentsRequest = decode_message(request, "GetDocumentsRequest")?;
    let response: GetDocumentsResponse = decode_message(response, "GetDocumentsResponse")?;
    let version = response_version(&response);

    let (contract_id_bytes, document_type_name) = match &request.version {
        Some(GetDocumentsVersion::V0(v0)) => (v0.data_contract_id.clone(), v0.document_type.clone()),
        Some(GetDocumentsVersion::V1(v1)) => (v1.data_contract_id.clone(), v1.document_type.clone()),
        None => return Err("GetDocumentsRequest has no version set".to_string()),
    };
    let contract_id = crate::types::id32(&contract_id_bytes, "contract id")?;
    let contract = provider()
        .get_data_contract(&contract_id.into(), version)
        .map_err(|e| format!("unable to resolve data contract: {e}"))?
        .ok_or("document queries support only the pinned system contracts")?;

    let (documents, mtd, _) = verify_documents_response(
        request,
        std::sync::Arc::clone(&contract),
        response,
        network()?,
        version,
        provider(),
    )
    .map_err(|e| format!("document proof verification failed: {e}"))?;

    let document_type = contract
        .document_type_for_name(&document_type_name)
        .map_err(|e| format!("unknown document type {document_type_name}: {e}"))?;
    let mut serialized = Vec::new();
    for document in documents.into_iter().flatten().filter_map(|(_, doc)| doc) {
        serialized.push(
            document
                .serialize(document_type, &contract, version)
                .map_err(|e| format!("unable to re-serialize verified document: {e}"))?,
        );
    }
    Ok((serialized, meta_from(&mtd)))
}

/// getContestedResourceVoteState (VoteTally result type). The query shape —
/// contract, document type, index values, tally options, count — is
/// reconstructed from the request itself.
pub fn verify_get_contested_vote_state(
    request: &[u8],
    response: &[u8],
) -> Result<(ContestedVoteState, Meta), String> {
    let request: GetContestedResourceVoteStateRequest =
        decode_message(request, "GetContestedResourceVoteStateRequest")?;
    let response: GetContestedResourceVoteStateResponse =
        decode_message(response, "GetContestedResourceVoteStateResponse")?;
    let version = response_version(&response);
    let (contenders, mtd, _) = Contenders::maybe_from_proof_with_metadata(
        request,
        response,
        network()?,
        version,
        provider(),
    )
    .map_err(|e| format!("contested vote state proof verification failed: {e}"))?;

    let mut state = ContestedVoteState::default();
    if let Some(contenders) = contenders {
        state.contest_found = true;
        state.contenders = contenders
            .contenders
            .iter()
            .map(|(id, contender)| (id.to_buffer(), contender.vote_tally()))
            .collect();
        state.abstain_votes = contenders.abstain_vote_tally;
        state.lock_votes = contenders.lock_vote_tally;
        if let Some((winner_info, finalization_block)) = contenders.winner {
            state.finished = true;
            state.finished_at_time_ms = finalization_block.time_ms;
            match winner_info {
                WinnerInfo::WonByIdentity(id) => state.winner = Some(id.to_buffer()),
                WinnerInfo::Locked => state.locked = true,
                WinnerInfo::NoWinner => {}
            }
        }
    }
    Ok((state, meta_from(&mtd)))
}

pub fn verify_contested_vote_state(
    proof: &[u8],
    contract_id: &[u8],
    document_type: &str,
    index_values: Vec<String>,
    count: u16,
) -> Result<ContestedResult, String> {
    let contract_id = id32(contract_id, "contract id")?;
    // Only the DPNS contested-name poll exists today; the contract is needed
    // to resolve the index-value tree keys.
    let contract = load_contract(SystemDataContract::DPNS)?;
    if contract.id().to_buffer() != contract_id {
        return Err("contested vote state queries support only the DPNS contract".to_string());
    }
    let query = ContestedDocumentVotePollDriveQuery {
        vote_poll: ContestedDocumentResourceVotePoll {
            contract_id: contract_id.into(),
            document_type_name: document_type.to_string(),
            index_name: CONTESTED_INDEX_NAME.to_string(),
            index_values: index_values.into_iter().map(Value::Text).collect(),
        },
        result_type: ContestedDocumentVotePollDriveQueryResultType::VoteTally,
        offset: None,
        limit: Some(count),
        start_at: None,
        allow_include_locked_and_abstaining_vote_tally: true,
    };
    let resolved = query
        .resolve_with_provided_borrowed_contract(&contract)
        .map_err(|e| format!("unable to resolve contested vote query: {e}"))?;
    let (root_hash, result) = resolved
        .verify_vote_poll_vote_state_proof(proof, PlatformVersion::latest())
        .map_err(|e| format!("contested vote state proof verification failed: {e}"))?;

    let mut state = ContestedVoteState {
        contest_found: !(result.contenders.is_empty()
            && result.locked_vote_tally.is_none()
            && result.abstaining_vote_tally.is_none()
            && result.winner.is_none()),
        contenders: result
            .contenders
            .iter()
            .map(|contender| (contender.identity_id().to_buffer(), contender.vote_tally()))
            .collect(),
        abstain_votes: result.abstaining_vote_tally,
        lock_votes: result.locked_vote_tally,
        ..Default::default()
    };
    if let Some((winner_info, finalization_block)) = result.winner {
        state.finished = true;
        state.finished_at_time_ms = finalization_block.time_ms;
        match winner_info {
            ContestedDocumentVotePollWinnerInfo::WonByIdentity(id) => {
                state.winner = Some(id.to_buffer())
            }
            ContestedDocumentVotePollWinnerInfo::Locked => state.locked = true,
            ContestedDocumentVotePollWinnerInfo::NoWinner => {}
        }
    }
    Ok(ContestedResult { root_hash, state })
}
