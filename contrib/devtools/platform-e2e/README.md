# Dash Platform GUI — live testnet end-to-end harness

Developer tooling (not built by CI) that exercises the `--enable-platform-gui`
platform client against **live Dash testnet Platform**, end to end:

1. `faucet.py` — funds a testnet address by driving the faucet at
   faucet.thepasta.org in a headless browser (the page solves its own Cap.js
   proof-of-work). Requires `pip install playwright && playwright install
   chromium`.
2. `make_assetlock.py` — builds, funds, signs and broadcasts a
   `TRANSACTION_ASSET_LOCK` on testnet via a synced `dashd`'s RPC, and waits
   for its InstantSend lock. Reuses the functional-test serialization classes.
3. `e2e.cpp` — the actual end-to-end driver. Using only the platform client
   library (`src/platform/statetransitions.*` + `src/platform/transport/*`)
   and libbitcoin key/crypto, it:
   - generates the identity funding key and identity authentication keys,
   - creates the asset lock (via `make_assetlock.py`),
   - builds and broadcasts an `IdentityCreateTransition` (Instant asset lock
     proof) to a testnet evonode over gRPC-Web,
   - waits for the identity to confirm,
   - builds and broadcasts DPNS `preorder` then `domain` documents, and
   - resolves the registered username back to the identity.

## Result

This harness has registered real identities and usernames on Dash testnet.
Example run (evonode 68.67.122.25:1443, Platform/DAPI v4.0.0):

```
identity 6ed0631afd75e846fd527761ca48c322553fece191bf2b96889f7ff6afdc7be8
registered username 'qte2e8df49727' on testnet
```

The identity was independently re-confirmed (with `prove=true` GroveDB proofs)
from unrelated evonodes 206.245.167.63 and 68.67.122.6, proving the state
transition construction, signing, and transport are correct against live
Platform — not just self-consistent.

## Building the driver

```
MBED=depends/<host-triplet>
clang++ -std=c++20 -DHAVE_CONFIG_H -I src -I src/config -I src/univalue/include \
  -I src/secp256k1/include -I "$MBED/include" \
  -isystem src/dashbls/include -isystem src/dashbls/depends/relic/include \
  contrib/devtools/platform-e2e/e2e.cpp \
  src/libdash_platform.a src/libbitcoin_common.a src/libbitcoin_consensus.a \
  src/libbitcoin_util.a src/crypto/.libs/libbitcoin_crypto_base.a \
  src/.libs/libunivalue.a src/secp256k1/.libs/libsecp256k1.a \
  src/dashbls/.libs/libdashbls.a src/dashbls/.libs/librelic.a \
  <repeat the archives once for static link order> \
  -L "$MBED/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lgmp -o /tmp/e2e
/tmp/e2e <evonode-ip> [username]
```
