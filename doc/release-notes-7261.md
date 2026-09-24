Wallet
------

* `fundrawtransaction`, `send`, `sendall` and `walletcreatefundedpsbt` accept a
  `use_cj` option, previously available only on `sendtoaddress` and `sendmany`,
  that restricts coin selection to fully mixed CoinJoin outputs. `send` and
  `sendall` record the resulting transaction as a CoinJoin spend (`DS` in
  `gettransaction`), as `sendtoaddress` and `sendmany` already do.
