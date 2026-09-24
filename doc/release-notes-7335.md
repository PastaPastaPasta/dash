Updated RPCs
------------

* `gettxoutsetinfo` no longer returns `block_info.unspendables.bip30`. Dash
  never had transactions overridden by duplicates, so the value was always
  zero.
