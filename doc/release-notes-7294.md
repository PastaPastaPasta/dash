Consensus changes
-----------------

* Asset Lock special transactions gain a payload version 2, enabled by the `v24`
  hard fork. Before activation only version 1 is valid; after activation both
  version 1 and version 2 are. A version 2 payload signals that Platform assigns
  each credit output to the matching Identity automatically, and it extends the
  allowed `creditOutputs[].scriptPubKey` forms from P2PKH only to P2PKH and P2SH.
  All other Asset Lock rules are unchanged. See DIP-27. (dash#7294)

Updated RPCs
------------

* `validateaddress` and `getaddressinfo` now accept DIP-18 Dash Platform
  addresses (`dash1k…`/`dash1s…` on mainnet, `tdash1k…`/`tdash1s…` on the test
  chains) and report them with a new `isplatform` field. `getaddressinfo`
  describes such an address against the credit output script an Asset Lock would
  carry for it, so `ismine` and `solvable` answer whether this wallet holds the
  key or script behind the address. (dash#7294)

* `sendtoaddress` and `sendmany` accept Platform addresses as recipients. Doing
  so turns the transaction into an Asset Lock with a version 2 payload whose
  credit outputs are the Platform recipients, plus a single empty `OP_RETURN`
  output carrying their total. `subtractfeefromamount` is not supported for
  Platform recipients, because the locked amount has to match the `OP_RETURN`
  value exactly. (dash#7294)

* `getrawtransaction`, `decoderawtransaction` and the other transaction-decoding
  RPCs report `assetLockTx.creditOutputs[].address` for version 2 payloads, the
  DIP-18 Platform address the credit output pays. Version 1 payloads carry no
  such address and the field is omitted. (dash#7294)

Notes
-----

* This release carries the consensus side of version 2 Asset Locks: after the
  `v24` hard fork they are valid in a block. They remain non-standard for now,
  so under default policy they are not accepted into the mempool or relayed,
  and sending to a Platform address fails with
  `Transaction is non-standard (assetlocktx-version-2) …`. A future release
  will make them standard once Dash Platform can process them; that is a
  policy change only and needs no further consensus change. On testnet,
  devnets and regtest the node can be started with `-acceptnonstdtxn=1` to
  relay them already; mainnet does not accept that option. (dash#7294)
