// Live-testnet E2E: asset lock -> IdentityCreate -> DPNS preorder -> domain ->
// resolve, exercising the real dash-qt platform statetransitions + transport.
#include <platform/params.h>
#include <platform/statetransitions.h>
#include <platform/transport/cbor.h>
#include <platform/transport/grpcweb.h>
#include <platform/transport/protobuf.h>
#include <platform/types.h>

#include <key.h>
#include <hash.h>
#include <random.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <functional>
const std::function<std::string(const char*)> G_TRANSLATION_FUN{nullptr};
using namespace platform;
static std::string EVONODE = "68.67.122.25";
static const uint16_t PORT = 1443;
static const char* SVC = "/org.dash.platform.dapi.v0.Platform/";

static pb::Writer VersionWrap(pb::Writer inner) { pb::Writer w; w.Message(1, inner.take()); return w; }
static transport::GrpcCallResult Call(const std::string& m, const std::vector<uint8_t>& r) {
    return transport::GrpcWebUnary(EVONODE, PORT, std::string(SVC) + m, r, 20000);
}
static std::vector<uint8_t> V(const Identifier& id) { return {id.begin(), id.end()}; }

static std::vector<uint8_t> GetIdentity(const Identifier& id) {
    pb::Writer v0; v0.Bytes(1, V(id));
    auto r = Call("getIdentity", VersionWrap(std::move(v0)).data());
    if (r.grpc_status != 0) return {};
    auto v0f = pb::GetLenField(r.message, 1); if (!v0f) return {};
    auto idf = pb::GetLenField(*v0f, 1); if (!idf) return {};
    return {idf->begin(), idf->end()};
}
static bool GetContractNonce(const Identifier& id, const Identifier& c, uint64_t& out) {
    pb::Writer v0; v0.Bytes(1, V(id)); v0.Bytes(2, V(c));
    auto r = Call("getIdentityContractNonce", VersionWrap(std::move(v0)).data());
    if (r.grpc_status != 0) { fprintf(stderr, "  nonce grpc=%d %s\n", r.grpc_status, r.grpc_message.c_str()); return false; }
    auto v0f = pb::GetLenField(r.message, 1); if (!v0f) return false;
    auto n = pb::GetVarintField(*v0f, 1); out = n ? *n : 0; return true;
}
static bool Broadcast(const std::vector<uint8_t>& st, std::string& err) {
    pb::Writer w; w.Bytes(1, st);
    auto r = Call("broadcastStateTransition", w.data());
    if (r.grpc_status == 0) return true;
    err = "grpc " + std::to_string(r.grpc_status) + ": " + r.grpc_message;
    return false;
}
static st::Signer KeySigner(const CKey& k) {
    return [k](const uint256& digest, std::vector<uint8_t>& sig) { return k.SignCompact(digest, sig); };
}
static void Sleep(int s) { std::this_thread::sleep_for(std::chrono::seconds(s)); }

int main(int argc, char** argv) {
    if (argc > 1) EVONODE = argv[1];
    const std::string label = argc > 2 ? argv[2] : ("qte2e" + HexStr(GetRandHash()).substr(0, 8));
    ECC_Start();

    CKey funding; funding.MakeNewKey(true);
    CKey auth0; auth0.MakeNewKey(true);
    CKey auth1; auth1.MakeNewKey(true);
    const CKeyID funding_hash = funding.GetPubKey().GetID();

    // 1. L1 asset lock funding the credit output to `funding` (0.01 DASH).
    const std::string cmd = "python3 /private/tmp/claude-502/-Users-pasta-workspace-dash--claude-worktrees-dash-platform-usernames-ui-bbe2b0/5802b8de-601e-4473-9ee9-2331b709cf48/scratchpad/make_assetlock.py "
        + HexStr(funding_hash) + " 1000000 > /tmp/e2e_al.json 2>/tmp/e2e_al.err";
    fprintf(stderr, "[1/6] creating asset lock (funding_hash=%s)...\n", HexStr(funding_hash).c_str());
    if (std::system(cmd.c_str()) != 0) { fprintf(stderr, "asset lock failed; /tmp/e2e_al.err\n"); return 1; }

    std::ifstream jf("/tmp/e2e_al.json"); std::stringstream ss; ss << jf.rdbuf();
    UniValue al; if (!al.read(ss.str())) { fprintf(stderr, "bad asset lock json\n"); return 1; }
    const auto tx_hex = al["tx_hex"].get_str();
    if (al["islock_hex"].isNull()) { fprintf(stderr, "no islock produced\n"); return 1; }
    const auto islock_hex = al["islock_hex"].get_str();
    fprintf(stderr, "  txid=%s islocked\n", al["txid"].get_str().c_str());

    st::InstantAssetLockProof proof;
    proof.transaction = ParseHex(tx_hex);
    proof.instant_lock = ParseHex(islock_hex);
    proof.output_index = 0;

    // 2. IdentityCreate: keys 0 (MASTER) and 1 (HIGH).
    std::vector<st::NewIdentityKey> keys;
    {
        st::NewIdentityKey k0;
        k0.id = 0; k0.purpose = IdentityPublicKey::Purpose::AUTHENTICATION;
        k0.security_level = IdentityPublicKey::SecurityLevel::MASTER;
        auto p0 = auth0.GetPubKey(); k0.pubkey.assign(p0.begin(), p0.end()); k0.signer = KeySigner(auth0);
        keys.push_back(std::move(k0));
        st::NewIdentityKey k1;
        k1.id = 1; k1.purpose = IdentityPublicKey::Purpose::AUTHENTICATION;
        k1.security_level = IdentityPublicKey::SecurityLevel::HIGH;
        auto p1 = auth1.GetPubKey(); k1.pubkey.assign(p1.begin(), p1.end()); k1.signer = KeySigner(auth1);
        keys.push_back(std::move(k1));
    }

    fprintf(stderr, "[2/6] building + broadcasting IdentityCreate...\n");
    auto ic = st::BuildIdentityCreate(proof, keys, KeySigner(funding));
    if (!ic.ok()) { fprintf(stderr, "  build failed: %s\n", ic.error.c_str()); return 1; }
    std::string err;
    if (!Broadcast(ic.value->bytes, err)) { fprintf(stderr, "  broadcast failed: %s\n", err.c_str()); return 1; }
    fprintf(stderr, "  broadcast OK; st hash=%s\n", ic.value->hash.ToString().c_str());

    // Recompute identity id from the funding outpoint (txid || 0).
    // (BuildIdentityCreate uses DSHA256(tx_bytes) as the special-tx txid.)
    uint256 tx_dsha = Hash(proof.transaction);
    std::array<uint8_t, 36> outpoint{}; std::copy(tx_dsha.begin(), tx_dsha.end(), outpoint.begin());
    const Identifier real_id = st::IdentityIdFromOutpoint(outpoint);
    fprintf(stderr, "  identity_id=%s\n", HexStr(real_id).c_str());

    // 3. Confirm identity exists.
    fprintf(stderr, "[3/6] waiting for identity confirmation...\n");
    bool confirmed = false;
    for (int i = 0; i < 30; ++i) {
        if (!GetIdentity(real_id).empty()) { confirmed = true; break; }
        Sleep(5);
    }
    if (!confirmed) { fprintf(stderr, "  identity did not confirm\n"); return 1; }
    fprintf(stderr, "  IDENTITY CONFIRMED\n");

    // 4. DPNS preorder.
    const std::string normalized = st::NormalizeLabel(label);
    fprintf(stderr, "[4/6] DPNS preorder for '%s' (normalized '%s')...\n", label.c_str(), normalized.c_str());
    std::array<uint8_t, 32> salt; GetStrongRandBytes(salt);
    const auto salted = st::SaltedDomainHash(salt, normalized, "dash");
    uint64_t nonce = 0; GetContractNonce(real_id, DPNS_CONTRACT_ID, nonce);
    auto pre = st::BuildDpnsPreorder(real_id, nonce + 1, salted, 1, KeySigner(auth1));
    if (!pre.ok()) { fprintf(stderr, "  preorder build failed: %s\n", pre.error.c_str()); return 1; }
    if (!Broadcast(pre.value->bytes, err)) { fprintf(stderr, "  preorder broadcast failed: %s\n", err.c_str()); return 1; }
    fprintf(stderr, "  preorder broadcast OK\n");
    Sleep(15);

    // 5. DPNS domain.
    fprintf(stderr, "[5/6] DPNS domain...\n");
    GetContractNonce(real_id, DPNS_CONTRACT_ID, nonce);
    auto dom = st::BuildDpnsDomain(real_id, nonce + 1, label, normalized, "dash", salt, 1, KeySigner(auth1));
    if (!dom.ok()) { fprintf(stderr, "  domain build failed: %s\n", dom.error.c_str()); return 1; }
    if (!Broadcast(dom.value->bytes, err)) { fprintf(stderr, "  domain broadcast failed: %s\n", err.c_str()); return 1; }
    fprintf(stderr, "  domain broadcast OK\n");

    // 6. Resolve the name back.
    fprintf(stderr, "[6/6] resolving '%s' back to the identity...\n", normalized.c_str());
    bool resolved = false;
    for (int i = 0; i < 20; ++i) {
        Sleep(6);
        // getDocuments on dpns domain: where [[normalizedParentDomainName,==,dash],[normalizedLabel,==,label]]
        transport::cbor::Writer w;
        w.Array(2);
        w.Array(3); w.Text("normalizedParentDomainName"); w.Text("=="); w.Text("dash");
        w.Array(3); w.Text("normalizedLabel"); w.Text("=="); w.Text(normalized);
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(DPNS_CONTRACT_ID.begin(), DPNS_CONTRACT_ID.end()));
        v0.Str(2, "domain");
        v0.Bytes(3, w.data());
        v0.Varint(5, 1); // limit
        auto r = Call("getDocuments", VersionWrap(std::move(v0)).data());
        if (r.grpc_status != 0) { fprintf(stderr, "  getDocuments grpc=%d %s\n", r.grpc_status, r.grpc_message.c_str()); continue; }
        auto v0f = pb::GetLenField(r.message, 1); if (!v0f) continue;
        auto docs = pb::GetLenField(*v0f, 1); if (!docs) continue;
        auto doc = pb::GetLenField(*docs, 1);
        if (doc && !doc->empty()) { resolved = true; break; }
    }
    if (!resolved) { fprintf(stderr, "  name did not resolve\n"); return 1; }

    printf("E2E SUCCESS: identity %s registered username '%s' on testnet\n", HexStr(real_id).c_str(), label.c_str());
    ECC_Stop();
    return 0;
}
