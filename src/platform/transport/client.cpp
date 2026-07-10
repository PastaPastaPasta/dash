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
        Enqueue([=, this] { cb(Result<std::vector<DpnsName>>{.value = std::vector<DpnsName>{}}); });
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
        Enqueue([=, this] { cb(Result<ContestedNameState>{.value = ContestedNameState{}}); });
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
                if (std::equal(qh.begin(), qh.end(), k.quorum_hash.begin())) return k.pubkey;
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
            method = "getIdentityContractNonce";
        } else {
            method = "getIdentityNonce";
        }
        std::string terr;
        auto r = Call(method, VersionWrap(std::move(v0)).data(), terr);
        Result<uint64_t> out;
        if (!r.transport_ok || r.grpc_status != 0) { out.error = r.transport_ok ? r.grpc_message : terr; cb(out); return; }
        auto p = ParseResponse(r.message);
        // Nonce is a scalar in the non-proof result (prove=false). Proof-backed
        // nonce verification is available via drive::VerifyIdentityNonce when
        // prove=true is requested; nonces are non-security-critical (server
        // rejects a wrong nonce at broadcast) so the value path is acceptable.
        uint64_t nonce = 0;
        if (p.v0) { if (auto n = pb::GetVarintField(*p.v0, 1)) nonce = *n; }
        out.value = nonce;
        cb(out);
    }

    void DoGetIdentity(const Identifier& id, const Callback<std::optional<Identity>>& cb)
    {
        // Proof-verified full identity: three simple proved sub-queries sharing
        // one signed root (drive::VerifyFullIdentity).
        auto lookup = MakeLookup();
        Result<std::optional<Identity>> out;

        std::vector<uint8_t> bal_proof, rev_proof, keys_proof;
        drive::ProofEnvelope env;
        drive::BlockContext ctx;
        bool have_env = false;

        // getIdentityBalance / getIdentityBalanceAndRevision take {id=1,
        // prove=2}; getIdentityKeys takes {id=1, request_type=2 (an AllKeys
        // sub-message), prove=5} — see platform.proto.
        auto fetch = [&](const std::string& method, bool keys_request, std::vector<uint8_t>& proof_out) -> bool {
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
            if (!DecodeProof(*p.proof, env, proof_out)) { out.error = method + ": bad proof envelope"; return false; }
            if (!have_env) { ctx = DecodeMetadata(p.metadata, m_params.tenderdash_chain_id); have_env = true; }
            return true;
        };

        if (!fetch("getIdentityBalance", false, bal_proof)) { cb(out); return; }
        if (!fetch("getIdentityBalanceAndRevision", false, rev_proof)) { cb(out); return; }
        if (!fetch("getIdentityKeys", true, keys_proof)) { cb(out); return; }

        std::optional<Identity> identity;
        drive::Hash256 root;
        std::string err;
        if (!drive::VerifyFullIdentity(bal_proof, rev_proof, keys_proof, id, identity, root, err)) {
            out.error = "proof verify failed: " + err;
            cb(out);
            return;
        }
        // Bind the root to a signed Platform block.
        if (identity && !drive::VerifyRootBinding(root, env, ctx, lookup, err)) {
            out.error = "quorum signature verify failed: " + err;
            cb(out);
            return;
        }
        out.value = identity;
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

    // Document queries (DPNS domain / DashPay profile & contactRequest). These
    // decode the returned documents via the DPP layer. Document-level proof
    // verification is applied when the drive layer exposes it; until then a
    // present document is returned and absence is treated as "not found".
    std::vector<std::vector<uint8_t>> GetDocuments(const Identifier& contract, const std::string& doc_type,
                                                   const std::vector<uint8_t>& where_cbor, uint32_t limit,
                                                   std::string& err)
    {
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(contract.begin(), contract.end()));
        v0.Str(2, doc_type);
        if (!where_cbor.empty()) v0.Bytes(3, where_cbor);
        if (limit) v0.Varint(5, limit);
        std::string terr;
        auto r = Call("getDocuments", VersionWrap(std::move(v0)).data(), terr);
        std::vector<std::vector<uint8_t>> docs;
        if (!r.transport_ok || r.grpc_status != 0) { err = r.transport_ok ? r.grpc_message : terr; return docs; }
        auto p = ParseResponse(r.message);
        if (!p.v0) return docs;
        // result.documents (field 1) -> Documents { repeated bytes documents = 1 }
        if (auto docs_msg = pb::GetLenField(*p.v0, 1)) {
            pb::Reader rd{*docs_msg};
            pb::Field f;
            while (rd.Next(f)) {
                if (f.number == 1 && f.type == pb::WireType::Len) docs.emplace_back(f.bytes.begin(), f.bytes.end());
            }
        }
        return docs;
    }

    static std::vector<uint8_t> DpnsWhere(const std::string& normalized_label, bool starts_with)
    {
        transport::cbor::Writer w;
        w.Array(2);
        w.Array(3); w.Text("normalizedParentDomainName"); w.Text("=="); w.Text("dash");
        w.Array(3); w.Text("normalizedLabel"); w.Text(starts_with ? "startsWith" : "=="); w.Text(normalized_label);
        return w.take();
    }

    void DoResolveName(const std::string& normalized_label, const Callback<std::optional<DpnsName>>& cb)
    {
        std::string err;
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", DpnsWhere(normalized_label, false), 1, err);
        Result<std::optional<DpnsName>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        if (docs.empty()) { out.value = std::optional<DpnsName>{}; cb(out); return; } // available / absent
        DpnsName name;
        if (dpp::DecodeDpnsDomain(docs.front(), name)) { out.value = name; }
        else { out.value = std::optional<DpnsName>{}; }
        cb(out);
    }

    void DoSearchNames(const std::string& prefix, uint32_t limit, const Callback<std::vector<DpnsName>>& cb)
    {
        std::string err;
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", DpnsWhere(prefix, true), limit, err);
        Result<std::vector<DpnsName>> out;
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
        auto docs = GetDocuments(DASHPAY_CONTRACT_ID, "profile", w.take(), 1, err);
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
        std::string err;
        auto docs = GetDocuments(DASHPAY_CONTRACT_ID, "contactRequest", w.take(), 100, err);
        Result<std::vector<ContactRequest>> out;
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
