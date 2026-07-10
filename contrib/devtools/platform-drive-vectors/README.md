# platform-drive-vectors

Developer tool that generates the Drive per-query proof vectors and the
Tenderdash quorum-signature vectors in
`src/test/data/platform/drive_query_vectors.json` and
`src/test/data/platform/quorum_sig_vectors.json`, consumed by
`src/test/platform_drive_tests.cpp` to validate the pure-C++ verifier in
`src/platform/drive/`.

The vectors are ground truth:

- **drive_query_vectors.json** — a synthetic GroveDB (grovedb
  [dashpay/grovedb](https://github.com/dashpay/grovedb) tag **v5.0.0**) is built
  with the exact Drive tree layout (RootTree children + identity
  sub-structure), identity public keys are serialized by the real `rs-dpp`
  ([dashpay/platform](https://github.com/dashpay/platform) tag **v4.0.0**,
  protocol version 12), and each query is proved with `prove_query` and
  re-verified with `verify_query_raw` so the recorded root hash and result set
  are authoritative.
- **quorum_sig_vectors.json** — the sign digest is built exactly as
  `rs-drive-proof-verifier` v4.0.0 `verify_tenderdash_proof` and
  `rs-tenderdash-abci` [v1.5.1](https://github.com/dashpay/rs-tenderdash-abci)
  `signatures.rs` do, and self-checked against that crate's own unit-test
  vectors. `StateId` is reproduced with a `prost` message declared with the
  field tags of tenderdash **v1.5.3**
  `proto/tendermint/types/types.proto`, so its bytes match a real node without
  needing `protoc`. The signature is a real BLS12-381 **basic-scheme** signature
  (blsful, re-exported by `dpp::bls_signatures`); blsful "Modern" serialization
  equals the IETF format that `src/bls` produces for the non-legacy scheme, so
  the 48-byte public key / 96-byte signature interoperate with
  `CBLSPublicKey`/`CBLSSignature`.

This crate is not part of the Dash Core build or CI. Regenerate the vectors
only when bumping the pinned tags:

```bash
cd contrib/devtools/platform-drive-vectors
cargo run --release -- ../../../src/test/data/platform
```

Then rerun `./src/test/test_dash --run_test=platform_drive_tests` and fix the
C++ port for any protocol changes.

## Coverage and limitations

Covered queries (all single-path, so the C++ client builds the same
`PathQuery` and can verify real evonode proofs): getIdentityBalance,
getIdentity revision, getIdentityNonce, getIdentityKeys (all keys),
getIdentityByPublicKeyHash (present + absent), and getIdentity (full),
assembled client-side from the balance + revision + all-keys sub-proofs.

Not yet covered (documented for a follow-up): getIdentityContractNonce,
getDocuments (DPNS / DashPay secondary-index queries), getDataContract, and
getContestedResourceVoteState. Their Drive path layouts involve secondary
indexes / contested-resource trees whose queries are harder to reproduce
synthetically and are best validated against captured testnet responses.
