// Proves the verified READ path against live testnet: transport ->
// drive::VerifyFullIdentity (GroveDB proof) -> quorum threshold-signature
// binding, for the identity the E2E registered.
#include <platform/drive/queries.h>
#include <platform/drive/quorumsig.h>
#include <platform/drive/verify.h>
#include <platform/transport/grpcweb.h>
#include <platform/transport/protobuf.h>
#include <util/strencodings.h>
#include <functional>
#include <util/translation.h>
#include <bls/bls.h>
#include <fstream>
#include <map>
#include <sstream>
#include <cstdio>
const std::function<std::string(const char*)> G_TRANSLATION_FUN{nullptr};
using namespace platform;

static const char* EVO = "68.67.122.25";
static pb::Writer Wrap(pb::Writer in){ pb::Writer w; w.Message(1,in.take()); return w; }

static bool Fetch(const std::string& method, const std::vector<uint8_t>& id,
                  std::vector<uint8_t>& proof_out, drive::ProofEnvelope& env, drive::BlockContext& ctx){
  pb::Writer v0; v0.Bytes(1,id);
  if(method=="getIdentityKeys"){ pb::Writer ak; pb::Writer rt; rt.Message(1,ak.take()); v0.Message(2,rt.take()); v0.Bool(5,true); }
  else v0.Bool(2,true);
  auto r=transport::GrpcWebUnary(EVO,1443,std::string("/org.dash.platform.dapi.v0.Platform/")+method,Wrap(std::move(v0)).data(),15000);
  if(r.grpc_status!=0){printf("  %s grpc=%d %s\n",method.c_str(),r.grpc_status,r.grpc_message.c_str());return false;}
  auto v0f=pb::GetLenField(r.message,1); if(!v0f)return false;
  auto pr=pb::GetLenField(*v0f,2); if(!pr){printf("  %s: no proof\n",method.c_str());return false;}
  auto md=pb::GetLenField(*v0f,3);
  auto gdb=pb::GetLenField(*pr,1); auto qh=pb::GetLenField(*pr,2); auto sig=pb::GetLenField(*pr,3);
  auto round=pb::GetVarintField(*pr,4); auto bid=pb::GetLenField(*pr,5); auto qt=pb::GetVarintField(*pr,6);
  if(!gdb||!qh||!sig||!bid)return false;
  proof_out.assign(gdb->begin(),gdb->end());
  std::copy(qh->begin(),qh->end(),env.quorum_hash.begin());
  std::copy(bid->begin(),bid->end(),env.block_id_hash.begin());
  std::copy(sig->begin(),sig->end(),env.signature.begin());
  env.round=round?(int32_t)*round:0; env.quorum_type=qt?(uint8_t)*qt:0;
  if(md){ if(auto h=pb::GetVarintField(*md,1))ctx.height=*h;
    if(auto c=pb::GetVarintField(*md,2))ctx.core_chain_locked_height=(uint32_t)*c;
    if(auto t=pb::GetVarintField(*md,4))ctx.time_ms=*t;
    if(auto p=pb::GetVarintField(*md,5))ctx.protocol_version=*p; }
  ctx.chain_id="dash-testnet-51";
  return true;
}

int main(){
  bls::bls_legacy_scheme.store(false);
  // Load quorum keys.
  std::map<std::string,std::vector<uint8_t>> qkeys;
  std::ifstream f("/private/tmp/claude-502/-Users-pasta-workspace-dash--claude-worktrees-dash-platform-usernames-ui-bbe2b0/5802b8de-601e-4473-9ee9-2331b709cf48/scratchpad/qkeys.txt");
  std::string line;
  while(std::getline(f,line)){ auto sp=line.find(' '); if(sp==std::string::npos)continue;
    qkeys[line.substr(0,sp)]=ParseHex(line.substr(sp+1)); }
  printf("loaded %zu quorum keys\n",qkeys.size());

  auto lookup=[&](uint8_t,const drive::Hash256& qh)->std::optional<std::vector<uint8_t>>{
    // quorum_hash in the proof is little-endian (internal); RPC hashes are
    // big-endian display. Try both orders.
    std::string be=HexStr(qh); std::vector<uint8_t> rev(qh.rbegin(),qh.rend()); std::string le=HexStr(rev);
    auto it=qkeys.find(be); if(it!=qkeys.end())return it->second;
    it=qkeys.find(le); if(it!=qkeys.end())return it->second;
    return std::nullopt;
  };

  auto id=ParseHex("6ed0631afd75e846fd527761ca48c322553fece191bf2b96889f7ff6afdc7be8");
  Identifier idf; std::copy(id.begin(),id.end(),idf.begin());

  std::vector<uint8_t> bal,rev,keys; drive::ProofEnvelope env; drive::BlockContext ctx;
  printf("fetching proved sub-queries...\n");
  printf("  -> getIdentityBalance\n");if(!Fetch("getIdentityBalance",id,bal,env,ctx))return 1;
  printf("  -> getIdentityBalanceAndRevision\n");if(!Fetch("getIdentityBalanceAndRevision",id,rev,env,ctx))return 1;
  printf("  -> getIdentityKeys\n");if(!Fetch("getIdentityKeys",id,keys,env,ctx))return 1;
  printf("  got proofs at height=%llu quorum_type=%u\n",(unsigned long long)ctx.height,env.quorum_type);
  std::optional<Identity> identity; drive::Hash256 root; std::string err;
  if(!drive::VerifyFullIdentity(bal,rev,keys,idf,identity,root,err)){printf("VerifyFullIdentity FAILED: %s\n",err.c_str());return 1;}
  if(!identity){printf("identity ABSENT\n");return 1;}
  printf("GroveDB proof verified: balance=%llu revision=%llu keys=%zu root=%s\n",(unsigned long long)identity->balance,(unsigned long long)identity->revision,identity->public_keys.size(),HexStr(root).c_str());

  if(!drive::VerifyRootBinding(root,env,ctx,lookup,err)){printf("quorum signature FAILED: %s\n",err.c_str());return 2;}
  printf("QUORUM SIGNATURE VERIFIED — root bound to a signed testnet Platform block\n");
  printf("READ-PATH E2E SUCCESS: fully proof-verified identity from live testnet\n");
  return 0;
}
