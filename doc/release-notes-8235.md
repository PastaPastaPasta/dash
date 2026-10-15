Updated RPCs
------------

- `testmempoolaccept` with more than one transaction reports the
  `package-error` `evo-local-unavailable`, and `submitpackage` fails with that
  reason, when this node cannot read the masternode, quorum or other Evo state
  it needs to check the package, for example because the block data it is
  derived from was pruned. The request was not at fault. (#8235)

Other changes
-------------

- A block this node has stored but cannot read back while deriving the credit
  pool or the EHF signals no longer makes the block being validated invalid.
  The node shuts down with a "Failed to read block" error instead, as it does
  when it cannot read a block it is connecting. (#8235)

- On a pruned node, a block whose masternode, quorum or other Evo state
  depends on block data that was pruned is no longer marked invalid, which
  left the node on a local fork until `reconsiderblock`. The node now stops
  advancing at that block, logs an error and shows a warning that block data
  was pruned; restart with `-reindex` to download it again. Startup still
  fails when the EHF signals at the tip cannot be rebuilt. (#8235)
