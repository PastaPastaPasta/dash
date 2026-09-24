Updated RPCs
------------

* The `payoutAddress` argument of `protx register`, `protx register_legacy`,
  `protx register_evo`, `protx register_fund`, `protx register_fund_legacy`,
  `protx register_fund_evo` and `protx update_registrar` accepts, in addition to
  a single address, an array of `{"address": ..., "reward": ...}` objects that
  splits the owner reward between several payees, with `reward` given in basis
  points. Multi-party payouts are only valid once the v24 hard fork activates.

* For ExtAddr (version 3) masternode states and ProTx payloads the payout list
  is reported as a `payouts` array in place of the single `payoutAddress`
  field, which legacy versions keep. Shared masternodes report `shares`
  instead. The fields appear in `protx info`, `protx list detailed`,
  `protx listdiff`, `masternode status` (`dmnState`) and in decoded
  `ProRegTx`/`ProUpRegTx` payloads of the transaction-decoding RPCs;
  `protx diff` includes them only with `extended=true`.
