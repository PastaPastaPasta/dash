# Masternode management in the GUI

The Masternodes tab in dash-qt gains a full set of management flows, so a
masternode, evonode or shared masternode can be created and maintained from the
wallet without hand-assembling `protx` commands in the debug console.

## Recognition

- Shared masternodes appear in the list with the type "Shared", have their own
  entry in the type filter (the "Regular" filter now shows only single-owner
  masternodes), and match the text filter by their share owner, refund and
  reward addresses. A masternode is recognized as owned when the wallet holds
  any of its share owner keys.
- The details dialog shows the collateral share table with per-share amounts and
  addresses, the shares belonging to this wallet, the early-exit penalty terms
  and the remaining early period.

## Registration

- A wizard registers a masternode (1,000 DASH) or evonode (4,000 DASH) using one
  of three collateral paths: funding the collateral from the wallet, using an
  existing collateral output already held by the wallet, or external collateral
  held in another wallet or on a hardware device (prepare, sign the message
  out-of-band, then submit).
- Owner and voting addresses are generated from the wallet, so its backup covers
  them. The operator BLS key is generated and saved in the wallet (new default),
  generated without saving (its secret shown once, behind a confirmation), or
  supplied as a hosting provider's public key. Either way the matching
  `masternodeblsprivkey` line for the masternode server's `dash.conf` is shown.

## Transaction history

- Dash special transactions now have their own types in the transaction list:
  Masternode Registration, Masternode Update and Masternode Dissolution, with a
  matching entry in the type filter. A registration previously appeared as a
  "Payment to yourself" whose amount was only the network fee.

## Maintenance

- Update Service, Update Registrar and Revoke are available from the list's
  context menu, with the evonode platform fields where applicable. Update
  Registrar is not offered for shared masternodes, which rotate their keys
  unanimously instead.

## Shared masternodes (requires v24)

- Creating a shared masternode is a resumable session: participants pass a
  session file between their wallets to agree the share table and terms, each
  contributes one funding output equal to their share, and each signs. Every
  signature and every imported copy is verified against the transaction actually
  being registered before it is accepted, so the terms shown always match the
  terms signed.
- Shared masternodes can change a share's reward address, rotate their operator
  and voting keys unanimously, and dissolve either unilaterally (with a live
  payout and penalty preview) or unanimously (penalty-free). Each participant can
  also generate standby dissolution transactions that never expire, to recover
  their principal without cooperation if a key is lost.

Shared-masternode features are shown only once the v24 hard fork is active on the
connected network.
