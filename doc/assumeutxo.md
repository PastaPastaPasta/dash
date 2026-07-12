# Using Assumeutxo

Assumeutxo can make a new Dash Core node usable quickly by loading a recent
chainstate snapshot. The node syncs forward from the snapshot immediately and,
at the same time, validates every block from genesis in the background. For
implementation details, see the [design document](/doc/design/assumeutxo.md).

## Availability and trust

There is currently no canonical snapshot distribution service and this tree
has no mainnet or testnet snapshot parameters. Built-in snapshots and the
`-assumeutxodata` option are for regtest only until release builders generate
and review production parameters.

A snapshot may be transported by an untrusted third party. For production
parameters, Dash Core accepts it only when its network and base block match an
authorized entry and both its UTXO hash and Dash evo-state hash match the
hardcoded values. (Two built-in legacy regtest fixtures retain a test-only null
evo-hash wildcard.) Dash Core also
cross-checks the deterministic masternode list, quorum commitments, and credit
pool against commitments in the base block's CbTx. Background validation then
re-derives the UTXO set and canonical masternode state from genesis before the
snapshot is considered fully validated.

## Obtain or create a snapshot

Obtain a snapshot for an authorized base height from a source you choose, or
create one on a normally synced node with `dumptxoutset`. A current-tip dump is:

```text
dash-cli -rpcclienttimeout=0 dumptxoutset /path/to/utxo.dat latest
```

To dump a particular recent height (or block hash), temporarily roll the node
back while writing the snapshot:

```text
dash-cli -rpcclienttimeout=0 -named dumptxoutset /path/to/utxo.dat rollback=HEIGHT
```

The positional form `dumptxoutset /path/to/utxo.dat rollback` selects the
latest base already authorized in chain parameters. A rollback dump requires
all intervening block and undo data, so it can fail on a pruned node. Network
activity is suspended and peers are disconnected during rollback; avoid other
block-storage RPCs until the command finishes. The original tip is reconsidered
after the dump. Use an unlimited or long RPC timeout because hashing and evo
snapshot construction can take several minutes.

The result includes `base_hash`, `base_height`, `txoutset_hash`, `evo_hash`,
`nchaintx`, and the written path. Operators can reproduce an authorized dump at
the same base and compare both hashes. Creating a dump at an arbitrary height
does not authorize it for loading; authorization is supplied by chain
parameters (or, on regtest, an exact `-assumeutxodata` entry).

## Load and monitor a snapshot

Wait for the destination node to learn the base block header, ensure its
mempool is empty, and load the file:

```text
dash-cli loadtxoutset /path/to/utxo.dat
```

Relative paths are resolved under the network data directory. On success the
result reports `coins_loaded`, `tip_hash`, `base_height`, and `path`. The input
file can then be removed; Dash Core has copied its state into
`chainstate_snapshot` and EvoDB.

`loadtxoutset` is unavailable when Dash Core is running in masternode mode.
Stop using `-masternodeblsprivkey` and restart as a regular node before loading
a snapshot. Quorum signing is also refused whenever an unvalidated snapshot is
active.

Monitor progress with:

```text
dash-cli getchainstates
```

During background validation, `chainstates` normally contains two entries. The
active snapshot entry has `snapshot_blockhash` and `validated: false`; the
historical entry advances from genesis. The snapshot chain syncs to the network
tip first, so normal wallet and mempool use can begin before background
validation finishes.

When the historical entry reaches the snapshot base, Dash Core recomputes the
UTXO hash, compares the canonical masternode list and required reconstruction
history, and completes any CbTx/evo checks deferred until the base block was
available. The active entry then reports `validated: true`. Cleanup and
promotion are crash-safe and may finish on the next restart, after which only
one normal chainstate remains.

If any required hash or derived-state comparison fails, Dash Core invalidates
the snapshot and shuts down rather than continuing on it. Follow the reported
recovery instruction; a reindex may be required for an EvoDB inconsistency.

## Disk use, pruning, and indexes

A pruned node can use Assumeutxo, but dual-chainstate operation can temporarily
exceed the configured prune budget. The minimum block-file allowance is needed
for each chainstate, the snapshot base is retained until deferred checks
complete, and index builders retain blocks they have not processed.

Indexes still build sequentially from genesis; they do not begin at the
snapshot base. Expect extra download time and disk use until background
validation and indexes catch up. Two chainstate databases also remain on disk
until validation succeeds and cleanup completes.
