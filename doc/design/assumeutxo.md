# Assumeutxo design

For operator instructions, see [Using Assumeutxo](/doc/assumeutxo.md).

Assumeutxo lets a node become useful from a recent, precomputed chainstate
while it validates the history from genesis in the background. The snapshot is
not a consensus shortcut: its base block and expected state hashes must be
authorized by chain parameters, and the node does not mark it fully validated
until background validation independently reaches the base.

Dash snapshots contain both the UTXO set and the block-derived state needed to
continue Dash validation above the base. This document describes the format,
trust model, and lifecycle implemented in this tree.

## File format and authorization

A snapshot starts with the version-2 `SnapshotMetadata` container defined in
`src/node/utxo_snapshot.h`:

1. the five bytes `utxo\xff`;
2. metadata version 2;
3. the network message-start bytes;
4. the snapshot base block hash; and
5. the number of serialized coins.

The body is the declared sequence of `(COutPoint, Coin)` records followed by
the `DASHEVO\0` marker and a `CEvoSnapshot`. The evo payload has its own format
version, currently version 3. Keeping its version separate permits the Dash
section to evolve without changing the outer, upstream-compatible metadata
container.

`AssumeutxoData` authorizes a base height and block hash together with
`nChainTx`, the serialized UTXO hash (`hash_serialized`), and the canonical evo
hash (`evo_hash`). The hashes commit to the decoded UTXO and evo bodies, not to
the metadata header or the whole file. Network magic prevents loading a file
for another network. Mainnet and testnet currently have no authorized entries;
regtest has built-in test entries and accepts additional exact entries through
the regtest-only debug option
`-assumeutxodata=<height>:<hash_serialized>:<evo_hash>:<nchaintx>:<blockhash>`.
Two legacy regtest fixtures use a null `evo_hash` as an intentional test-only
wildcard; null evo hashes are rejected on every other network.

Release builders generate and independently reproduce production parameter
entries using the procedure in [release-process.md](/doc/release-process.md).

## Dash evo snapshot version 3

The evo section carries the minimum block-derived state required to validate
and operate above a snapshot base without possessing every earlier block:

- the canonical deterministic masternode list at the base, including internal
  IDs and the total registration counter;
- a diff-encoded, apply-ordered reconstruction history for the older MN lists
  needed by quorum member selection;
- active and safety mined quorum commitments for every enabled LLMQ type,
  quorum score modifiers, and the four rotation-cycle snapshots needed to
  reconstruct the current and retained rotated quorum sets;
- credit-pool state, including the balance/limits and spent asset-unlock index
  ranges; and
- MNHF/EHF deployment signals.

Collections and nested allocations are bounded during decoding. Canonical
ordering covers MNs, historical diffs, quorum types and commitments, score
modifiers, credit-pool ranges, and MNHF signals. Historical MN lists are
reconstructed by applying each diff, rebuilding their indexes, and checking
their canonical hashes. See `src/evo/snapshot.h` and `src/evo/snapshot.cpp` for
the exact codec and bounds.

Internal MN IDs are part of the commitment deliberately. A node synced from
genesis assigns them in deterministic on-chain registration order and advances
the registration counter identically, so a dump can be reproduced from
genesis. Sorting by full `proTxHash` avoids dependence on in-memory container
iteration order.

Except for that regtest-only wildcard, the compiled `evo_hash` is SHA256 of the
canonical disk serialization. It has
the same developer-reviewed, hardcoded trust model as `hash_serialized` and is
reproducible by independently synced-from-genesis nodes. In addition, the evo
state is checked against commitments in the base block's coinbase special
transaction (CbTx): the deterministic-MN-list merkle root, quorum merkle root,
and, where present, credit-pool balance. These header-chain-anchored checks
reduce the state that rests only on the hardcoded hashes.

## Dual-chainstate lifecycle

Without a snapshot, `ChainstateManager` owns one normal, fully validated
chainstate. Snapshot use adds a second chainstate; both share the block index,
block files, Dash managers, and one `CEvoDB`, but have separate coins databases
and separate EvoDB identities.

### Activation

`loadtxoutset` first requires an empty mempool and an authorized base whose
header is known. It parses the metadata and coins, checks the coin count and
`hash_serialized`, parses and validates the evo v3 section, checks `evo_hash`,
and performs every CbTx check possible with the locally available base block.
It then seeds the base MN list, reconstruction history, score modifiers,
quorum commitments and rotation snapshots, credit pool, and MNHF signals.

Only after these checks and durable writes does the new
`chainstate_snapshot` become active. The former normal chainstate becomes the
historical/background chainstate and continues from genesis. The snapshot
chainstate is explicitly `UNVALIDATED` even though it can sync from its base to
the network tip. `getchainstates` exposes both chainstates and their
`validated` fields.

### Background validation and completion

Both chainstates execute normal block and Dash special-transaction validation.
The active snapshot chainstate is prioritized until it reaches the network tip;
cache is then rebalanced toward background validation. When the background
chainstate reaches the snapshot base, completion:

1. flushes both coins/EvoDB identities;
2. recomputes its serialized UTXO hash and compares it with
   `hash_serialized`;
3. reconstructs the retained historical MN lists and compares them with the
   independently captured background lists;
4. performs deferred CbTx checks using the now-available base block;
5. compares the snapshot's canonical base MN-list hash with the list derived
   by background validation; and
6. verifies both EvoDB best-block markers.

Any mismatch invalidates the snapshot and shuts the node down with recovery
information. Success changes the snapshot chainstate to `VALIDATED`, records
the computed UTXO hash on the completed background chainstate, and releases
the snapshot-base pruning lock.

### Crash-safe promotion and cleanup

Shutdown can occur during loading, background validation, completion, or
cleanup. Persistent base-block and EvoDB lifecycle markers let startup resume
the two-chainstate run. After successful completion, startup promotes the
snapshot coins directory to the normal `chainstate` directory and retires the
background directory through durable rename steps (`chainstate_todelete` is
used as the deletion staging name). EvoDB snapshot markers are promoted only
after both directory renames are durable.

Load-time recovery recognizes interrupted promotion states and either finishes
cleanup or reports an inconsistency requiring reindex; it does not silently
select one side. After cleanup there is one normal, fully validated chainstate.
The retained `base_blockhash` records its snapshot origin but does not make it
assumed-valid again. Reindex paths remove stale snapshot/invalid/to-delete
directories and rebuild EvoDB.

## Shared EvoDB and union convergence

Dash uses one `CEvoDB` rather than a second `evodb_snapshot` directory. Most
derived data is keyed by block hash: snapshot-chainstate writes above the base
and background-chainstate writes through the base are naturally disjoint. Their
union converges to the database produced by validation from genesis, so no
lossy merge is needed when the background coins database is discarded.

The `NORMAL` and `SNAPSHOT` identities have independent best-block markers and
transaction contexts. Flushes are serialized and committed per identity.
Writes of already seeded block-hash data are verify-or-skip: disagreement
between seeded and independently derived state is validation failure, not an
overwrite. A dual-chainstate marker makes incomplete shared-DB work detectable
at startup. This is the D2 union-convergence design implemented in
`src/evo/evodb.*`, `src/evo/snapshot_load.cpp`, and `src/validation.cpp`.

## Subsystem behavior before validation

Validation callbacks identify the chainstate role so background connections do
not drive active-tip side effects. Wallets, indexes, P2P processing, and Dash
subsystems continue to follow the active snapshot tip where appropriate while
the background chainstate performs consensus validation.

Safety boundaries are stricter for quorum signing:

- signature-share creation and quorum-signing RPCs refuse to sign while a
  snapshot is unvalidated;
- `masternode status` reports quorum participation as disabled in that state;
  and
- runtime `loadtxoutset` is refused in masternode mode because the already
  constructed active signing contexts cannot be rebound safely.

Verification and non-signing use of the seeded quorum state remain available.
When requested historical block or quorum data is unavailable because it lies
below an unvalidated snapshot base, serving code treats that as local data
unavailability and does not punish the requesting or serving peer for the
snapshot limitation. The node also advertises limited network service until
background validation restores full historical service.

## Pruning, indexes, and the mempool

Pruned nodes may load snapshots. During dual-chainstate operation the prune
budget is split between chainstates, with the per-chainstate minimum enforced;
the effective requirement can therefore exceed the configured budget. A prune
lock retains the base block until deferred CbTx/evo completion checks finish,
and blocks needed by indexes cannot be pruned before those indexes process
them.

Indexes do not skip history: they build in order from genesis using background
validation and continue through the snapshot base to the active tip. This may
temporarily retain substantial block data. The active snapshot can otherwise
serve normal wallet and mempool operation, but `loadtxoutset` requires the
mempool to be empty so transactions validated against the old active coins view
cannot cross the activation boundary.

The input snapshot file is no longer needed after `loadtxoutset` returns
successfully. The two coins databases and retained block/index data, however,
remain until validation and cleanup complete.
