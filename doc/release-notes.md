# Dash Core version v24.0.3

This is a new patch version release. It fixes governance proposals created in
the Qt wallet that could pass their vote and never be paid, a database
inconsistency that could follow a reindex, a wallet rescan that could miss
transactions, mnemonic checksum validation, CoinJoin signature and queue
validation, and several other bugs.

Like v24.0.0, v24.0.1 and v24.0.2, this release carries the `v24` network
upgrade (hard fork), which is **mandatory** for all nodes: every node must run
v24.0.0 or later before the hard fork activates. Upgrading to v24.0.3 is
recommended for all nodes. The upgrade itself and everything else introduced
in v24 are described in the
[v24.0.0 release notes](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.0.md),
the fixes for `v24` activation in the
[v24.0.1 release notes](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.1.md),
and the memory leak fix in the
[v24.0.2 release notes](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.2.md).

Please report bugs using the issue tracker at GitHub:

  <https://github.com/dashpay/dash/issues>

# Upgrading and downgrading

## How to Upgrade

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over /Applications/Dash-Qt (on Mac) or
dashd/dash-qt (on Linux).

Upgrading from v24.0.0, v24.0.1 or v24.0.2 needs no further steps. If you are
upgrading from v23.1.x or older, read "How to Upgrade" in the
[v24.0.0 release notes](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.0.md)
first: the first start migrates the address, spent and timestamp indexes, and
some settings changed.

## Downgrade warning

### Downgrade to v24.0.0, v24.0.1 or v24.0.2

Downgrading to v24.0.0, v24.0.1 or v24.0.2 is possible and needs no special
steps.

### Downgrade to a version < v24.0.0

There is no supported downgrade after `v24` activation, and downgrading before
activation has caveats. See "Downgrade warning" in the
[v24.0.0 release notes](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.0.md).

# Release Notes

## Governance proposals

- The Create Proposal dialog built a proposal's payment window around the time
  the dialog was used rather than around the superblocks chosen in it. Such a
  proposal could pass its vote and still not be paid: a one-payment proposal
  for any superblock after the next one was never paid, and a multi-payment
  proposal lost its last payment(s). The window now runs from half a cycle
  before the first chosen superblock to half a cycle after the last one, and
  the dates in the "Payment date" list are estimated the same way. The list no
  longer offers a superblock whose voting deadline has passed, and the deadline
  is checked again before the proposal fee is paid. Proposals created with the
  old window can't be changed, because the window is part of the proposal
  data. (#8154)
- Fixed a use-after-free and missing size limits in the cache that holds
  governance votes received before their governance object. (#8122)

## Reindex

- After `-reindex` or `-reindex-chainstate`, and after the one-time repair of
  masternode list diffs that runs on the first start after upgrading from a
  version older than v23.0.2, EvoDB could be written to disk ahead of the
  coins database. If the node was then stopped uncleanly before its next
  flush, the next start failed with "Error initializing block database" or
  "Found EvoDB inconsistency" and needed a reindex. The node now flushes all of
  its databases, in the same order as a clean shutdown, as soon as the reindex
  or repair completes. A node that already fails to start with one of these
  errors still needs to be reindexed once with v24.0.3. (#8103)

## Wallet

- Mnemonic validation now checks every BIP39 checksum bit. Previously only
  some of them were checked, so some mistyped mnemonics were accepted and
  silently created or restored a different wallet. Such a mnemonic is now
  refused. It was never a valid BIP39 mnemonic, and other wallets already
  refuse it. Existing wallets are not affected. (#8111)
- A rescan that uses block filters (`-blockfilterindex`) could skip blocks that
  pay the wallet and still report success, when the wallet's descriptors
  changed during the rescan. The rescan now reads the wallet's descriptors under
  their locks and picks up descriptors added while it runs. (#8139)

## CoinJoin

- A masternode coordinating a mixing session now checks each participant's
  signature against the output it spends and the final transaction that all
  participants approved. Previously the check could accept a signature that
  did not authenticate the input, which let one participant break a session.
  (#7815)
- A participant who leaves several inputs unsigned is now counted once when
  the masternode chooses whose collateral to charge. Previously such a
  participant could avoid the charge. (#8116)
- Mixing clients now reject queue announcements with an invalid denomination,
  as masternodes already do. (#7955)

## Masternodes, quorums and sporks

- A node now relays and mines the final commitment of a quorum with the most
  signers. Previously the first commitment it saw was kept even when one with
  more signers arrived, and a replaced commitment was never freed. (#8085)
- A signature share that a masternode requested from a peer is requested again
  if it was dropped because the queue of pending shares was full. Previously it
  was never requested from that peer again. (#7994)
- Two spork updates signed within the same second, or after the signing node's
  clock went back, left the signing node on the new value while other nodes
  kept the old one. A new spork message is now signed with a later time than
  the active one, or refused if that time would be too far in the future for
  other nodes to accept. (#8127)

## GUI

- Editing a contact's address in the address book no longer replaces its label
  with the new address. (#8123)
- Coin Control no longer unselects the wrong input when one of the coins it
  shows is no longer available. (#8123)
- With `-disablewallet`, clicking the proxy icon now opens the Network page of
  the options dialog instead of Appearance. (#8124)
- Saving a PSBT now reports an error if the file could not be written, and file
  names with non-ASCII characters are handled correctly. The PSBT dialog no
  longer reads an unknown fee, and a zero fee is no longer shown as unknown.
  (#8124)
- The CoinJoin send tab no longer loads, and on shutdown overwrites, the
  custom change address saved by the regular send tab. (#8124)
- The service update and revoke dialogs of a shared masternode keep OK
  disabled until an address with funds to pay the fee is selected. (#8124)
- Cancelling a wallet migration, including at the passphrase prompt, now
  stops it and leaves the wallet locked or unlocked for mixing only, as it
  was before. Wallets unlocked only for mixing are asked for their passphrase
  too. (#8124)
- The proposal list retries when masternode data is not ready yet, and its
  background updates stop when the application shuts down. (#8124)
- Clearing the network traffic graph now also clears the samples of the longer
  time ranges. (#8123)
- With several wallets loaded, the overview page of one wallet no longer
  suppresses CoinJoin updates on another's. Other display fixes: the balance
  labels when watch-only balances are hidden, the out-of-sync label in
  discreet mode, the DASH unit description, the proposal details after a
  display unit change or a wallet unload, and the credit pool amounts shown
  before the first result arrives. (#8123)

## RPC

- `quorum info`, `sign`, `verify`, `hasrecsig`, `getrecsig`, `isconflicting`,
  `selectquorum` and `getdata` now reject a quorum type outside 0..255 with
  "JSON integer out of range". Previously such a value was truncated, so `356`
  was used as quorum type `100`. `quorum sign` now checks the quorum type
  before reporting that it is only available on masternodes. (#7954)
- `masternode payments` treats an explicit `null` count as the default of 1,
  and `masternode payments` and `getspecialtxes` no longer overflow on very
  large or negative counts. `masternode winners` rejects a negative count.
  (#8118, #8125)
- `verifyislock` with a `maxHeight` outside the chain now fails with
  "Block height out of range" instead of an internal error. (#8125)
- `getaddressdeltas` accepts a single address given as a string, like the
  other address index RPCs. (#8118)
- `evodb verify` and `evodb repair` require both blocks to be in the active
  chain and otherwise fail with "Start block not found in active chain" or
  "Stop block not found in active chain". (#8118)
- `getblockheaders` reads the requested headers while holding the chain lock.
  The help and errors of `operatorReward` now give its range as 0 to 100.
  (#8118)

## Contrib

- Fixed the OpenRC init script, `contrib/zmq/zmq_sub.py` on Python 3.14 and
  IPv6 handling in `contrib/qos/tc.sh`. (#8109)

# v24.0.3 Change log

See detailed [set of changes][set-of-changes].

# Credits

Thanks to everyone who directly contributed to this release:

- PastaPastaPasta

As well as everyone that submitted issues and reviewed pull requests.

# Older releases

These releases are considered obsolete. Old release notes can be found here:

- [v24.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.2.md) released Nov/14/2026
- [v24.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.1.md) released Nov/13/2026
- [v24.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-24.0.0.md) released Nov/5/2026
- [v23.1.10](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.10.md) released Nov/2/2026
- [v23.1.9](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.9.md) released Oct/29/2026
- [v23.1.8](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.8.md) released Jul/29/2026
- [v23.1.7](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.7.md) released Jun/30/2026
- [v23.1.5](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.5.md) released Jun/19/2026
- [v23.1.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.4.md) released Jun/18/2026
- [v23.1.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.3.md) released May/28/2026
- [v23.1.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.2.md) released Mar/12/2026
- [v23.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.0.md) released Feb/15/2026
- [v23.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.0.2.md) released Dec/4/2025
- [v23.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.0.0.md) released Nov/10/2025
- [v22.1.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.3.md) released Jul/15/2025
- [v22.1.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.2.md) released Apr/15/2025
- [v22.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.1.md) released Feb/17/2025
- [v22.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.0.md) released Feb/10/2025
- [v22.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.0.0.md) released Dec/12/2024
- [v21.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.1.1.md) released Oct/22/2024
- [v21.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.1.0.md) released Aug/8/2024
- [v21.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.0.2.md) released Aug/1/2024
- [v21.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.0.0.md) released Jul/25/2024
- [v20.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.1.1.md) released April/3/2024
- [v20.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.1.0.md) released March/5/2024
- [v20.0.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.4.md) released Jan/13/2024
- [v20.0.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.3.md) released December/26/2023
- [v20.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.2.md) released December/06/2023
- [v20.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.1.md) released November/18/2023
- [v20.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.0.md) released November/15/2023
- [v19.3.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.3.0.md) released July/31/2023
- [v19.2.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.2.0.md) released June/19/2023
- [v19.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.1.0.md) released May/22/2023
- [v19.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.0.0.md) released Apr/14/2023
- [v18.2.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.2.2.md) released Mar/21/2023
- [v18.2.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.2.1.md) released Jan/17/2023
- [v18.2.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.2.0.md) released Jan/01/2023
- [v18.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.1.1.md) released January/08/2023
- [v18.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.1.0.md) released October/09/2022
- [v18.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.0.2.md) released October/09/2022
- [v18.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.0.1.md) released August/17/2022
- [v0.17.0.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.17.0.3.md) released June/07/2021
- [v0.17.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.17.0.2.md) released May/19/2021
- [v0.16.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.16.1.1.md) released November/17/2020
- [v0.16.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.16.1.0.md) released November/14/2020
- [v0.16.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.16.0.1.md) released September/30/2020
- [v0.15.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.15.0.md) released February/18/2020
- [v0.14.0.5](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.5.md) released December/08/2019
- [v0.14.0.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.4.md) released November/22/2019
- [v0.14.0.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.3.md) released August/15/2019
- [v0.14.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.2.md) released July/4/2019
- [v0.14.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.1.md) released May/31/2019
- [v0.14.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.md) released May/22/2019
- [v0.13.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.3.md) released Apr/04/2019
- [v0.13.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.2.md) released Mar/15/2019
- [v0.13.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.1.md) released Feb/9/2019
- [v0.13.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.0.md) released Jan/14/2019
- [v0.12.3.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.4.md) released Dec/14/2018
- [v0.12.3.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.3.md) released Sep/19/2018
- [v0.12.3.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.2.md) released Jul/09/2018
- [v0.12.3.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.1.md) released Jul/03/2018
- [v0.12.2.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.2.3.md) released Jan/12/2018
- [v0.12.2.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.2.2.md) released Dec/17/2017
- [v0.12.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.2.md) released Nov/08/2017
- [v0.12.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.0.md) released Aug/15/2015
- [v0.11.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.11.2.md) released Mar/04/2015
- [v0.11.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.11.1.md) released Feb/10/2015
- [v0.11.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.11.0.md) released Jan/15/2015
- [v0.10.x](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.10.0.md) released Sep/25/2014
- [v0.9.x](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.9.0.md) released Mar/13/2014

[set-of-changes]: https://github.com/dashpay/dash/compare/v24.0.2...dashpay:v24.0.3
