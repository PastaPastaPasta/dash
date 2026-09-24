New settings
------------

* The dust attack protection introduced in v23.1.0 as a GUI option is now
  available to `dashd` as well through `-dustprotectionthreshold=<n>`: UTXOs
  received from external transactions whose value is at or below `<n>` duffs are
  locked automatically by the wallet, so they are not spent together with other
  coins. It is disabled by default (`0`). The GUI option and the command-line
  option control the same setting. Locks persist across restarts and are not
  released when the threshold is lowered or the option is removed; use
  `lockunspent` to unlock such outputs manually.
