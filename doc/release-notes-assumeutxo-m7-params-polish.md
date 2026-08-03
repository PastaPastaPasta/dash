Assumeutxo
----------

Dash Core can now bootstrap from an authorized UTXO and Dash evo-state
snapshot while validating the chain from genesis in the background. The new
`loadtxoutset` RPC loads snapshots created by `dumptxoutset`, and the new
`getchainstates` RPC reports active and background validation progress.
`dumptxoutset` now includes the canonical deterministic-masternode, quorum,
credit-pool, and MNHF state needed to continue Dash validation, and supports
creating a snapshot at a specified rollback height or block hash.

Production snapshot contents are checked against hardcoded UTXO and evo hashes
and against the base block's CbTx commitments. Masternode-mode nodes cannot call
`loadtxoutset`, and quorum signing remains disabled while a loaded snapshot is
not yet validated.

This release does not yet include mainnet or testnet snapshot parameters.
Assumeutxo loading is available for built-in regtest entries and exact regtest
entries supplied with the debug-only `-assumeutxodata` option until production
parameters are generated through the release process.
