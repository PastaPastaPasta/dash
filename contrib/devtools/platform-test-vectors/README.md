# platform-test-vectors

Developer tool that generates GroveDB proof test vectors for the pure-C++
GroveDB proof verifier in `src/platform/proof/` (unit tests:
`src/test/platform_proof_tests.cpp`, suite `platform_proof_tests`).

This crate is **not** built by CI or by the normal Dash Core build. It exists
so the committed JSON vectors in `src/test/data/platform/` can be regenerated
from the real Rust GroveDB implementation (github.com/dashpay/grovedb, pinned
to tag `v5.0.0`) whenever the C++ port needs new coverage.

## Usage

Requires a recent stable Rust toolchain (edition 2024 support, Rust >= 1.85)
and the usual native build dependencies for RocksDB (clang, cmake).

```bash
cd contrib/devtools/platform-test-vectors
cargo run --release -- ../../../src/test/data/platform
```

This rewrites:

- `element_vectors.json` — serialized `Element` bytes (bincode
  standard/big-endian config) for Item, SumItem, Tree, SumTree and Reference
  variants.
- `merk_proof_vectors.json` — single-subtree merk proof op streams (extracted
  from full GroveDB proofs) with query descriptions, expected root hashes and
  expected result sets.
- `grovedb_proof_vectors.json` — full layered `GroveDBProof` envelopes (V0 and
  V1) with path-query descriptions, expected root hashes and expected result
  sets (as returned by `GroveDb::verify_query_raw`).

All fixture contents are deterministic (fixed keys/values, no randomness or
timestamps), so regenerating without changing this crate must produce
byte-identical JSON.
