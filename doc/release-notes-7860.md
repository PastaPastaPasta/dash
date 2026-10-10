Governance
----------

- Pruned nodes can now run governance. A new `-govcollateralindex` stores
  only the transactions that could be a proposal's collateral, and unlike
  `-txindex` it works with `-prune`. It is turned on automatically whenever
  `-txindex` is off and governance validation is on, so pruned nodes, and
  nodes started with `-txindex=0`, now sync proposals and votes, check
  superblock payments in full, and can create and submit proposals.

- A node that pruned blocks before it had this index can't build it, because
  the index has to start from the genesis block. It now starts with governance
  turned off and a warning instead of failing; restarting it once with
  `-reindex` downloads the blockchain again and enables governance, and the
  node stays pruned.

- Pruned nodes now check superblock payments against the governance
  triggers they know, like full nodes do, instead of only checking that a
  superblock stays within the budget. A node that turns on `-prune` keeps
  its blocks until `-govcollateralindex` has read them, and a node's
  governance sync waits for the index instead of timing out.

- `-govcollateralindex` is not turned on automatically together with
  `-reindex-chainstate`, which it is incompatible with, as is `-txindex`.

- Starting with `-prune` and `-disablegovernance=0` is no longer an error.
  Starting with neither `-txindex` nor `-govcollateralindex` while governance
  validation is on still is, outside regtest.
