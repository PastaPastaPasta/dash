Updated RPCs
------------

- The `deprecatedrpc=fees` configuration option has been removed. The top-level
  fee fields `fee`, `modifiedfee`, `ancestorfees` and `descendantfees` are no
  longer returned by RPCs `getmempoolentry`, `getrawmempool(verbose=true)`,
  `getmempoolancestors(verbose=true)` and `getmempooldescendants(verbose=true)`.
  The same fee fields can be accessed through the `fees` object in the result.
  The top-level fee fields were previously deprecated in 23.0. (#25204)
