# Dash Platform client library (GUI-only)

This directory contains a Qt-free C++ client for Dash Platform (Evolution),
used exclusively by the dash-qt GUI when configured with
`--enable-platform-gui`. It provides:

- per-network parameters and the well-known system data contract IDs
  (`params.*`);
- codecs for the wire formats Platform uses: a bincode-v2 subset, a protobuf
  wire-format subset for the DAPI gRPC messages, and DPP (Dash Platform
  Protocol) object serialization;
- a GroveDB/merk proof verifier (blake3-based) mirroring the upstream Rust
  `verify` feature slice, plus the Tenderdash quorum-signature check that
  binds a proof's root hash to a Platform block signed by an LLMQ quorum;
- a DAPI client speaking gRPC-Web over HTTP/1.1 + TLS (mbedtls) to evonodes.

## Isolation rules

- Nothing in this directory may be linked into `dashd`, `dash-cli`,
  `dash-tx`, `dash-wallet` or any consensus/wallet library. It is linked into
  `dash-qt` and `test_dash` only, and only under `--enable-platform-gui`.
- Consensus, wallet and node code must not include headers from here. The GUI
  (`src/qt/platform/`) is the only consumer.
- Code here may depend on `src/crypto`, `src/util`, `src/bls` (dashbls),
  `src/secp256k1` and the standard library. It must not depend on Qt.

Upstream references are pinned in code comments (dashpay/platform, grovedb).
All protocol logic is pinned to a single Platform protocol version and must be
re-validated against Rust-generated test vectors when Platform upgrades.
