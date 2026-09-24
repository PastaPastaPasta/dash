P2P and network changes
-----------------------

* Startup now fails if any configured `-bind`, `-whitebind` or the implicit Tor
  onion-service bind (`127.0.0.1:9996` by default) cannot be set up. Dash Core
  v23 and earlier started as long as at least one bind succeeded. Use
  `-bind=<addr>:<port>=onion` to move the onion bind, or `-listenonion=0` to
  drop it, if the default address is not available on your system.
