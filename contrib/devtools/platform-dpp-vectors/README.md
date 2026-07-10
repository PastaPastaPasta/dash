# platform-dpp-vectors

Developer tool that generates the DPP (Dash Platform Protocol) test vectors in
`src/test/data/platform/dpp_identity_vectors.json` and
`src/test/data/platform/dpp_st_vectors.json`, consumed by
`src/test/platform_dpp_tests.cpp` to validate the pure-C++ DPP port in
`src/platform/dpp/`.

The vectors are ground truth: every state transition is constructed, signed
and serialized by the real `rs-dpp` crate from
[dashpay/platform](https://github.com/dashpay/platform), pinned to tag
**v4.0.0** (Platform protocol version **12**). Signing uses
`dashcore::signer` (double SHA256 digest, 65-byte compact recoverable ECDSA,
header byte `27 + recovery_id + 4`).

This crate is not part of the Dash Core build or CI. Regenerate the vectors
only when bumping the pinned platform tag:

```bash
cd contrib/devtools/platform-dpp-vectors
cargo run --release -- ../../../src/test/data/platform
```

Then rerun `./src/test/test_dash --run_test=platform_dpp_tests` and fix the
C++ port for any protocol changes.
