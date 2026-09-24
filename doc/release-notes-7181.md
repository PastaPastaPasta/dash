Updated RPCs
------------

* The deprecated keys `service`, `platformP2PPort` and `platformHTTPPort` will now require the runtime
  flag `-deprecatedrpc=service`.
  * The following RPCs are affected `decoderawtransaction`, `decodepsbt`, `getblock`,
    `getrawtransaction`, `gettransaction`, `masternode status` (only the `dmnState` key), `protx diff`,
    `protx listdiff`.
  * This runtime gate also extends to the functionally identical key, `address` in `masternode list`
    (and its alias, `masternodelist`)
  * The same gate applies to `getcoinjoininfo`, `protx info`, `protx list detailed`
    and `quorum info`.
  * `dash-tx -json` output omits these keys as well and offers no way to restore
    them, since `dash-tx` does not accept `-deprecatedrpc`.
