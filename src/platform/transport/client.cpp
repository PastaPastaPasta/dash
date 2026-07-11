// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>

#include <platform/drive/queries.h>
#include <platform/drive/quorumsig.h>
#include <platform/drive/verify.h>
#include <platform/dpp/document.h>
#include <platform/transport/cbor.h>
#include <platform/transport/grpcweb.h>
#include <platform/transport/protobuf.h>

#include <logging.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace platform {

namespace {

constexpr char SVC[] = "/org.dash.platform.dapi.v0.Platform/";
constexpr int CALL_TIMEOUT_MS = 20000;

pb::Writer VersionWrap(pb::Writer inner)
{
    pb::Writer w;
    w.Message(1, inner.take());
    return w;
}

//! Extract a DAPI response's v0 body, its Proof (result field 2) and its
//! ResponseMetadata (field 3).
struct ParsedResponse {
    std::optional<Span<const uint8_t>> v0;
    std::optional<Span<const uint8_t>> non_proof; // result field 1 (value/documents)
    std::optional<Span<const uint8_t>> proof;     // result field 2
    std::optional<Span<const uint8_t>> metadata;  // field 3
};

ParsedResponse ParseResponse(Span<const uint8_t> msg)
{
    ParsedResponse r;
    r.v0 = pb::GetLenField(msg, 1);
    if (!r.v0) return r;
    r.non_proof = pb::GetLenField(*r.v0, 1);
    r.proof = pb::GetLenField(*r.v0, 2);
    r.metadata = pb::GetLenField(*r.v0, 3);
    return r;
}

//! Decode a DAPI Proof message into a drive::ProofEnvelope + its grovedb bytes.
bool DecodeProof(Span<const uint8_t> proof_msg, drive::ProofEnvelope& env,
                 std::vector<uint8_t>& grovedb_out)
{
    auto gdb = pb::GetLenField(proof_msg, 1);
    auto qh = pb::GetLenField(proof_msg, 2);
    auto sig = pb::GetLenField(proof_msg, 3);
    auto round = pb::GetVarintField(proof_msg, 4);
    auto bid = pb::GetLenField(proof_msg, 5);
    auto qt = pb::GetVarintField(proof_msg, 6);
    if (!gdb || !qh || !sig || !bid) return false;
    if (qh->size() != 32 || bid->size() != 32 || sig->size() != 96) return false;
    grovedb_out.assign(gdb->begin(), gdb->end());
    std::copy(qh->begin(), qh->end(), env.quorum_hash.begin());
    std::copy(bid->begin(), bid->end(), env.block_id_hash.begin());
    std::copy(sig->begin(), sig->end(), env.signature.begin());
    env.round = round ? static_cast<int32_t>(*round) : 0;
    env.quorum_type = qt ? static_cast<uint8_t>(*qt) : 0;
    return true;
}

drive::BlockContext DecodeMetadata(std::optional<Span<const uint8_t>> md, const std::string& chain_id)
{
    drive::BlockContext ctx;
    ctx.chain_id = chain_id;
    if (!md) return ctx;
    if (auto h = pb::GetVarintField(*md, 1)) ctx.height = *h;
    if (auto c = pb::GetVarintField(*md, 2)) ctx.core_chain_locked_height = static_cast<uint32_t>(*c);
    if (auto t = pb::GetVarintField(*md, 4)) ctx.time_ms = *t;
    if (auto p = pb::GetVarintField(*md, 5)) ctx.protocol_version = *p;
    return ctx;
}

class GrpcWebClient final : public PlatformClient
{
public:
    explicit GrpcWebClient(Params params) : m_params(std::move(params))
    {
        m_worker = std::thread([this] { Run(); });
    }
    ~GrpcWebClient() override { shutdown(); }

    void shutdown() override
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_stop) return;
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    void updateEndpoints(std::vector<Endpoint> endpoints) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_endpoints = std::move(endpoints);
        m_ep_index = 0;
    }
    void updateQuorumKeys(uint8_t llmq_type, std::vector<QuorumKey> keys) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_quorum_type = llmq_type;
        m_quorum_keys = std::move(keys);
    }

    // Queries — each enqueues a task on the worker thread.
    void resolveName(const std::string& normalized_label, Callback<std::optional<DpnsName>> cb) override
    {
        Enqueue([=, this] { DoResolveName(normalized_label, cb); });
    }
    void searchNames(const std::string& prefix, uint32_t limit, Callback<std::vector<DpnsName>> cb) override
    {
        Enqueue([=, this] { DoSearchNames(prefix, limit, cb); });
    }
    void namesOfIdentity(const Identifier& identity, Callback<std::vector<DpnsName>> cb) override
    {
        Enqueue([=, this] { DoNamesOfIdentity(identity, cb); });
    }
    void getIdentity(const Identifier& id, Callback<std::optional<Identity>> cb) override
    {
        Enqueue([=, this] { DoGetIdentity(id, cb); });
    }
    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& h, Callback<std::optional<Identity>> cb) override
    {
        Enqueue([=, this] { DoGetIdentityByPubKeyHash(h, cb); });
    }
    void getIdentityNonce(const Identifier& id, Callback<uint64_t> cb) override
    {
        Enqueue([=, this] { DoGetNonce(id, std::nullopt, cb); });
    }
    void getIdentityContractNonce(const Identifier& id, const Identifier& contract, Callback<uint64_t> cb) override
    {
        Enqueue([=, this] { DoGetNonce(id, contract, cb); });
    }
    void getProfile(const Identifier& owner_id, Callback<std::optional<Profile>> cb) override
    {
        Enqueue([=, this] { DoGetProfile(owner_id, cb); });
    }
    void getContactRequests(const Identifier& identity, bool to_me, uint64_t since_ms,
                            Callback<std::vector<ContactRequest>> cb) override
    {
        Enqueue([=, this] { DoGetContactRequests(identity, to_me, since_ms, cb); });
    }
    void getContestedNameState(const std::string& normalized_label, Callback<ContestedNameState> cb) override
    {
        Enqueue([=, this] { DoGetContestedNameState(normalized_label, cb); });
    }
    void broadcastStateTransition(const std::vector<uint8_t>& st, Callback<BroadcastResult> cb) override
    {
        Enqueue([=, this] { DoBroadcast(st, cb); });
    }

private:
    // ---- worker plumbing ----
    void Enqueue(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_stop) return;
            m_queue.push_back(std::move(task));
        }
        m_cv.notify_one();
    }
    void Run()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty()) return;
                task = std::move(m_queue.front());
                m_queue.pop_front();
            }
            task();
        }
    }

    // Pick an endpoint round-robin; empty host if none configured.
    std::string NextEndpoint(uint16_t& port)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_endpoints.empty()) return {};
        const auto& ep = m_endpoints[m_ep_index % m_endpoints.size()];
        ++m_ep_index;
        port = ep.service.GetPort();
        return ep.service.ToStringAddr();
    }

    drive::QuorumKeyLookup MakeLookup()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto keys = m_quorum_keys;
        return [keys](uint8_t, const drive::Hash256& qh) -> std::optional<std::vector<uint8_t>> {
            for (const auto& k : keys) {
                if (k.matchesProofHash(qh)) return k.pubkey;
            }
            return std::nullopt;
        };
    }

    transport::GrpcCallResult Call(const std::string& method, const std::vector<uint8_t>& req, std::string& transport_err)
    {
        // Try each endpoint until one answers at transport level.
        const size_t n = [&] { std::lock_guard<std::mutex> lk(m_mtx); return std::max<size_t>(1, m_endpoints.size()); }();
        transport::GrpcCallResult last;
        for (size_t i = 0; i < n; ++i) {
            uint16_t port = 1443;
            const std::string host = NextEndpoint(port);
            if (host.empty()) { transport_err = "no evonode endpoints available"; return {}; }
            last = transport::GrpcWebUnary(host, port, std::string(SVC) + method, req, CALL_TIMEOUT_MS);
            if (last.transport_ok) return last;
            transport_err = last.transport_error;
        }
        return last;
    }

    // ---- operations ----
    void DoBroadcast(const std::vector<uint8_t>& st, const Callback<BroadcastResult>& cb)
    {
        pb::Writer w;
        w.Bytes(1, st);
        std::string terr;
        auto r = Call("broadcastStateTransition", w.data(), terr);
        Result<BroadcastResult> out;
        if (!r.transport_ok) { out.error = terr; cb(out); return; }
        BroadcastResult br;
        br.accepted = (r.grpc_status == 0);
        if (!br.accepted) { br.error = r.grpc_message; br.error_code = static_cast<uint32_t>(r.grpc_status); }
        out.value = br;
        cb(out);
    }

    void DoGetNonce(const Identifier& id, std::optional<Identifier> contract, const Callback<uint64_t>& cb)
    {
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(id.begin(), id.end()));
        std::string method;
        if (contract) {
            v0.Bytes(2, std::vector<uint8_t>(contract->begin(), contract->end()));
            v0.Bool(3, true);
            method = "getIdentityContractNonce";
        } else {
            v0.Bool(2, true);
            method = "getIdentityNonce";
        }
        std::string terr;
        auto r = Call(method, VersionWrap(std::move(v0)).data(), terr);
        Result<uint64_t> out;
        if (!r.transport_ok || r.grpc_status != 0) { out.error = r.transport_ok ? r.grpc_message : terr; cb(out); return; }
        auto p = ParseResponse(r.message);
        if (!p.proof) { out.error = method + ": no proof in response"; cb(out); return; }
        drive::ProofEnvelope env;
        std::vector<uint8_t> proof;
        if (!DecodeProof(*p.proof, env, proof)) { out.error = method + ": bad proof envelope"; cb(out); return; }
        drive::Hash256 root;
        std::optional<uint64_t> nonce;
        std::string err;
        const bool verified = contract
            ? drive::VerifyIdentityContractNonce(proof, id, *contract, nonce, root, err)
            : drive::VerifyIdentityNonce(proof, id, nonce, root, err);
        if (!verified || !drive::VerifyRootBinding(root, env,
                DecodeMetadata(p.metadata, m_params.tenderdash_chain_id), MakeLookup(), err)) {
            out.error = method + ": proof verification failed: " + err; cb(out); return;
        }
        out.value = nonce.value_or(0);
        cb(out);
    }

    void DoGetIdentity(const Identifier& id, const Callback<std::optional<Identity>>& cb)
    {
        // Proof-verified full identity: three simple proved sub-queries sharing
        // one signed root (drive::VerifyFullIdentity).
        auto lookup = MakeLookup();
        Result<std::optional<Identity>> out;

        struct ProvedResponse {
            std::vector<uint8_t> proof;
            drive::ProofEnvelope envelope;
            drive::BlockContext context;
        } balance, revision, keys;

        // getIdentityBalance / getIdentityBalanceAndRevision take {id=1,
        // prove=2}; getIdentityKeys takes {id=1, request_type=2 (an AllKeys
        // sub-message), prove=5} — see platform.proto.
        auto fetch = [&](const std::string& method, bool keys_request, ProvedResponse& proved) -> bool {
            pb::Writer v0;
            v0.Bytes(1, std::vector<uint8_t>(id.begin(), id.end()));
            if (keys_request) {
                pb::Writer all_keys;             // AllKeys {}
                pb::Writer request_type;         // KeyRequestType { all_keys = 1 }
                request_type.Message(1, all_keys.take());
                v0.Message(2, request_type.take());
                v0.Bool(5, true);                // prove
            } else {
                v0.Bool(2, true);                // prove
            }
            std::string terr;
            auto r = Call(method, VersionWrap(std::move(v0)).data(), terr);
            if (!r.transport_ok || r.grpc_status != 0) { out.error = r.transport_ok ? r.grpc_message : terr; return false; }
            auto p = ParseResponse(r.message);
            if (!p.proof) { out.error = method + ": no proof in response"; return false; }
            if (!DecodeProof(*p.proof, proved.envelope, proved.proof)) { out.error = method + ": bad proof envelope"; return false; }
            proved.context = DecodeMetadata(p.metadata, m_params.tenderdash_chain_id);
            return true;
        };

        if (!fetch("getIdentityBalance", false, balance)) { cb(out); return; }
        if (!fetch("getIdentityBalanceAndRevision", false, revision)) { cb(out); return; }
        if (!fetch("getIdentityKeys", true, keys)) { cb(out); return; }

        std::optional<uint64_t> balance_value, revision_value;
        std::optional<std::vector<IdentityPublicKey>> key_values;
        drive::Hash256 balance_root, revision_root, keys_root;
        std::string err;
        const bool proofs_ok =
            drive::VerifyIdentityBalance(balance.proof, id, balance_value, balance_root, err) &&
            drive::VerifyRootBinding(balance_root, balance.envelope, balance.context, lookup, err) &&
            drive::VerifyIdentityRevision(revision.proof, id, revision_value, revision_root, err) &&
            drive::VerifyRootBinding(revision_root, revision.envelope, revision.context, lookup, err) &&
            drive::VerifyIdentityKeys(keys.proof, id, key_values, keys_root, err) &&
            drive::VerifyRootBinding(keys_root, keys.envelope, keys.context, lookup, err);
        if (!proofs_ok) {
            out.error = "identity proof verification failed: " + err;
            cb(out);
            return;
        }
        if (!balance_value && !revision_value && !key_values) {
            out.value = std::optional<Identity>{};
        } else if (!balance_value || !revision_value || !key_values) {
            out.error = "identity proof components were inconsistent";
            cb(out);
            return;
        } else {
            Identity identity;
            identity.id = id;
            identity.balance = *balance_value;
            identity.revision = *revision_value;
            identity.public_keys = std::move(*key_values);
            out.value = std::move(identity);
        }
        cb(out);
    }

    void DoGetIdentityByPubKeyHash(const std::array<uint8_t, 20>& h, const Callback<std::optional<Identity>>& cb)
    {
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(h.begin(), h.end()));
        v0.Bool(2, true);
        std::string terr;
        auto r = Call("getIdentityByPublicKeyHash", VersionWrap(std::move(v0)).data(), terr);
        Result<std::optional<Identity>> out;
        if (!r.transport_ok || r.grpc_status != 0) { out.error = r.transport_ok ? r.grpc_message : terr; cb(out); return; }
        auto p = ParseResponse(r.message);
        if (!p.proof) { out.error = "no proof"; cb(out); return; }
        drive::ProofEnvelope env; std::vector<uint8_t> gp;
        if (!DecodeProof(*p.proof, env, gp)) { out.error = "bad proof"; cb(out); return; }
        auto ctx = DecodeMetadata(p.metadata, m_params.tenderdash_chain_id);
        std::optional<Identifier> id_out; std::string err;
        if (!drive::VerifyAndDecodeIdentityIdByPublicKeyHash(gp, env, ctx, h, MakeLookup(), id_out, err)) {
            out.error = err; cb(out); return;
        }
        if (!id_out) { out.value = std::optional<Identity>{}; cb(out); return; }
        // Follow the id to the full identity.
        DoGetIdentity(*id_out, cb);
    }

    using DocumentVerifier = std::function<bool(Span<const uint8_t>, std::vector<Bytes>&,
                                                drive::Hash256&, std::string&)>;

    // Document queries (DPNS domain / DashPay profile & contactRequest). The
    // server returns only a GroveDB proof; the requested Drive index path is
    // reconstructed locally, including cryptographic absence, and the root is
    // bound to a locally-known Platform LLMQ key before document bytes escape.
    std::vector<std::vector<uint8_t>> GetDocuments(const Identifier& contract, const std::string& doc_type,
                                                   const std::vector<uint8_t>& where_cbor, uint32_t limit,
                                                   const DocumentVerifier& verifier, std::string& err,
                                                   const std::vector<uint8_t>& order_by_cbor = {})
    {
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(contract.begin(), contract.end()));
        v0.Str(2, doc_type);
        if (!where_cbor.empty()) v0.Bytes(3, where_cbor);
        if (!order_by_cbor.empty()) v0.Bytes(4, order_by_cbor);
        if (limit) v0.Varint(5, limit);
        v0.Bool(8, true); // prove
        std::string terr;
        auto r = Call("getDocuments", VersionWrap(std::move(v0)).data(), terr);
        std::vector<std::vector<uint8_t>> docs;
        if (!r.transport_ok || r.grpc_status != 0) {
            err = r.transport_ok ? r.grpc_message : terr;
            LogPrintf("Platform getDocuments(%s) failed: %s\n", doc_type, err);
            return docs;
        }
        auto p = ParseResponse(r.message);
        if (!p.proof) { err = "getDocuments response did not contain a proof"; return docs; }
        drive::ProofEnvelope env;
        std::vector<uint8_t> grovedb_proof;
        if (!DecodeProof(*p.proof, env, grovedb_proof)) { err = "bad document proof envelope"; return docs; }
        drive::Hash256 root;
        if (!verifier(grovedb_proof, docs, root, err)) {
            LogPrintf("Platform getDocuments(%s) proof verification failed: %s\n", doc_type, err);
            return {};
        }
        const auto ctx = DecodeMetadata(p.metadata, m_params.tenderdash_chain_id);
        if (!drive::VerifyRootBinding(root, env, ctx, MakeLookup(), err)) {
            LogPrintf("Platform getDocuments(%s) root binding failed: %s\n", doc_type, err);
            return {};
        }
        return docs;
    }

    //! drive-abci decodes each getContestedResourceVoteState index value as a
    //! bincode (standard, big-endian) platform Value; strings are
    //! Value::Text = declaration-order discriminant 18 + length + utf8
    //! (rs-platform-value src/lib.rs, rs-drive-abci
    //! src/query/voting/contested_resource_vote_state/v0/mod.rs).
    static std::vector<uint8_t> BincodeTextValue(const std::string& text)
    {
        std::vector<uint8_t> out;
        out.push_back(18);
        // bincode varint: single byte below 251; DPNS labels are <= 63 chars.
        if (text.size() >= 251) return {};
        out.push_back(static_cast<uint8_t>(text.size()));
        out.insert(out.end(), text.begin(), text.end());
        return out;
    }

    //! Mirrors drive-abci's default_query_limit for requests that pin `count`
    //! so the locally reconstructed PathQuery matches the prover's exactly.
    static constexpr uint16_t CONTESTED_VOTE_COUNT{100};

    void DoGetContestedNameState(const std::string& normalized_label, const Callback<ContestedNameState>& cb)
    {
        // getContestedResourceVoteState on the DPNS contested
        // parentNameAndLabel index, VoteTally result type with locked and
        // abstaining tallies (platform.proto
        // GetContestedResourceVoteStateRequestV0).
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(DPNS_CONTRACT_ID.begin(), DPNS_CONTRACT_ID.end()));
        v0.Str(2, "domain");
        v0.Str(3, "parentNameAndLabel");
        v0.Bytes(4, BincodeTextValue("dash"));
        v0.Bytes(4, BincodeTextValue(normalized_label));
        v0.Varint(5, 1); // ResultType::VOTE_TALLY
        v0.Bool(6, true); // allow_include_locked_and_abstaining_vote_tally
        v0.Varint(8, CONTESTED_VOTE_COUNT);
        v0.Bool(9, true); // prove
        std::string terr;
        auto r = Call("getContestedResourceVoteState", VersionWrap(std::move(v0)).data(), terr);
        Result<ContestedNameState> out;
        if (!r.transport_ok || r.grpc_status != 0) {
            out.error = r.transport_ok ? r.grpc_message : terr;
            cb(out);
            return;
        }
        auto p = ParseResponse(r.message);
        if (!p.proof) { out.error = "getContestedResourceVoteState: no proof in response"; cb(out); return; }
        drive::ProofEnvelope env;
        std::vector<uint8_t> grovedb_proof;
        if (!DecodeProof(*p.proof, env, grovedb_proof)) { out.error = "bad contested vote proof envelope"; cb(out); return; }

        // Index values as raw tree keys (strings encode to their utf8 bytes,
        // DocumentPropertyType::encode_value_for_tree_keys).
        const std::vector<Bytes> index_values{Bytes{'d', 'a', 's', 'h'},
                                              Bytes(normalized_label.begin(), normalized_label.end())};
        drive::ContestedVoteState state;
        drive::Hash256 root;
        std::string err;
        if (!drive::VerifyContestedVoteState(grovedb_proof, DPNS_CONTRACT_ID, "domain", index_values,
                                             CONTESTED_VOTE_COUNT, state, root, err) ||
            !drive::VerifyRootBinding(root, env, DecodeMetadata(p.metadata, m_params.tenderdash_chain_id),
                                      MakeLookup(), err)) {
            out.error = "contested vote state proof verification failed: " + err;
            cb(out);
            return;
        }

        ContestedNameState result;
        result.normalized_label = normalized_label;
        result.contenders = std::move(state.contenders);
        result.abstain_votes = state.abstain_votes.value_or(0);
        result.lock_votes = state.lock_votes.value_or(0);
        if (!state.contest_found) {
            result.status = ContestedNameState::Status::UNKNOWN;
        } else if (!state.finished) {
            result.status = ContestedNameState::Status::CONTEST_IN_PROGRESS;
        } else if (state.locked) {
            result.status = ContestedNameState::Status::LOCKED;
            result.ends_at = state.finished_at_time_ms;
        } else {
            result.status = ContestedNameState::Status::WON;
            result.winner = state.winner;
            result.ends_at = state.finished_at_time_ms;
        }
        out.value = std::move(result);
        cb(out);
    }

    static std::vector<uint8_t> DpnsWhere(const std::string& normalized_label, bool starts_with)
    {
        transport::cbor::Writer w;
        w.Array(2);
        w.Array(3); w.Text("normalizedParentDomainName"); w.Text("=="); w.Text("dash");
        w.Array(3); w.Text("normalizedLabel"); w.Text(starts_with ? "startsWith" : "=="); w.Text(normalized_label);
        return w.take();
    }

    static std::vector<uint8_t> DpnsPrefixOrderBy()
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(2); w.Text("normalizedLabel"); w.Text("asc");
        return w.take();
    }

    void DoResolveName(const std::string& normalized_label, const Callback<std::optional<DpnsName>>& cb)
    {
        std::string err;
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", DpnsWhere(normalized_label, false), 1,
            [&normalized_label](Span<const uint8_t> proof, std::vector<Bytes>& out,
                                drive::Hash256& root, std::string& verify_error) {
                return drive::VerifyDpnsNameExact(proof, normalized_label, out, root, verify_error);
            }, err);
        Result<std::optional<DpnsName>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        if (docs.empty()) { out.value = std::optional<DpnsName>{}; cb(out); return; } // available / absent
        DpnsName name;
        if (dpp::DecodeDpnsDomain(docs.front(), name)) { out.value = name; }
        else {
            out.error = "DPNS domain document decoding failed";
            LogPrintf("Platform resolveName(%s): %s (%u bytes)\n", normalized_label, out.error,
                      docs.front().size());
        }
        cb(out);
    }

    void DoSearchNames(const std::string& prefix, uint32_t limit, const Callback<std::vector<DpnsName>>& cb)
    {
        std::string err;
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", DpnsWhere(prefix, true), limit,
            [&prefix, limit](Span<const uint8_t> proof, std::vector<Bytes>& out,
                             drive::Hash256& root, std::string& verify_error) {
                return drive::VerifyDpnsNamePrefix(proof, prefix, static_cast<uint16_t>(limit),
                                                   out, root, verify_error);
            }, err, DpnsPrefixOrderBy());
        Result<std::vector<DpnsName>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        std::vector<DpnsName> names;
        for (auto& d : docs) { DpnsName n; if (dpp::DecodeDpnsDomain(d, n)) names.push_back(std::move(n)); }
        out.value = std::move(names);
        cb(out);
    }

    void DoNamesOfIdentity(const Identifier& identity, const Callback<std::vector<DpnsName>>& cb)
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(3); w.Text("records.identity"); w.Text("==");
        w.Bytes(Span<const uint8_t>{identity.data(), identity.size()});
        std::string err;
        constexpr uint32_t limit{100};
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", w.take(), limit,
            [&identity](Span<const uint8_t> proof, std::vector<Bytes>& out,
                        drive::Hash256& root, std::string& verify_error) {
                return drive::VerifyDpnsNamesByIdentity(proof, identity, 100, out, root, verify_error);
            }, err);
        Result<std::vector<DpnsName>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        std::vector<DpnsName> names;
        for (auto& d : docs) { DpnsName n; if (dpp::DecodeDpnsDomain(d, n)) names.push_back(std::move(n)); }
        out.value = std::move(names);
        cb(out);
    }

    void DoGetProfile(const Identifier& owner_id, const Callback<std::optional<Profile>>& cb)
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(3); w.Text("$ownerId"); w.Text("=="); w.Bytes(Span<const uint8_t>{owner_id.data(), owner_id.size()});
        std::string err;
        auto docs = GetDocuments(DASHPAY_CONTRACT_ID, "profile", w.take(), 1,
            [&owner_id](Span<const uint8_t> proof, std::vector<Bytes>& out,
                        drive::Hash256& root, std::string& verify_error) {
                return drive::VerifyDashPayProfileByOwner(proof, owner_id, out, root, verify_error);
            }, err);
        Result<std::optional<Profile>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        if (docs.empty()) { out.value = std::optional<Profile>{}; cb(out); return; }
        Profile prof;
        if (dpp::DecodeDashPayProfile(docs.front(), prof)) out.value = prof;
        else out.value = std::optional<Profile>{};
        cb(out);
    }

    void DoGetContactRequests(const Identifier& identity, bool to_me, uint64_t /*since_ms*/,
                              const Callback<std::vector<ContactRequest>>& cb)
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(3);
        w.Text(to_me ? "toUserId" : "$ownerId");
        w.Text("==");
        w.Bytes(Span<const uint8_t>{identity.data(), identity.size()});
        transport::cbor::Writer order_by;
        order_by.Array(1);
        order_by.Array(2);
        order_by.Text("$createdAt");
        order_by.Text("asc");
        std::string err;
        auto docs = GetDocuments(DASHPAY_CONTRACT_ID, "contactRequest", w.take(), 100,
            [&identity, to_me](Span<const uint8_t> proof, std::vector<Bytes>& docs_out,
                               drive::Hash256& root, std::string& verify_error) {
                return drive::VerifyDashPayContactRequests(proof, identity, to_me, 100,
                                                           docs_out, root, verify_error);
            }, err, order_by.take());
        Result<std::vector<ContactRequest>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        std::vector<ContactRequest> reqs;
        for (auto& d : docs) { ContactRequest cr; if (dpp::DecodeDashPayContactRequest(d, cr)) reqs.push_back(std::move(cr)); }
        out.value = std::move(reqs);
        cb(out);
    }

    Params m_params;
    std::thread m_worker;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    bool m_stop{false};
    std::vector<Endpoint> m_endpoints;
    size_t m_ep_index{0};
    uint8_t m_quorum_type{0};
    std::vector<QuorumKey> m_quorum_keys;
};

} // namespace

std::unique_ptr<PlatformClient> MakeGrpcWebPlatformClient(const Params& params)
{
    return std::make_unique<GrpcWebClient>(params);
}

} // namespace platform
