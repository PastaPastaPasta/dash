Updated RPCs
------------

* `quorum dkginfo` accepts an optional `proTxHash` argument and reports, in a
  new `upcoming_dkgs` array, the DKG sessions that masternode will take part in
  whose work block is already mined. Without the argument the local active
  masternode is used, if any.
