# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Wallet functionality used by bitcoind and bitcoin-wallet executables.
add_library(bitcoin_wallet STATIC EXCLUDE_FROM_ALL
  ../coinjoin/client.cpp
  ../coinjoin/interfaces.cpp
  ../coinjoin/util.cpp
  bip39.cpp
  coincontrol.cpp
  context.cpp
  crypter.cpp
  db.cpp
  dump.cpp
  fees.cpp
  hdchain.cpp
  interfaces.cpp
  load.cpp
  rpcdump.cpp
  rpcwallet.cpp
  scriptpubkeyman.cpp
  wallet.cpp
  walletdb.cpp
  walletutil.cpp
  coinselection.cpp
)
target_link_libraries(bitcoin_wallet
  PRIVATE
    core_interface
    bitcoin_common
    univalue
    Boost::headers
    dashbls
    immer
    immer-dev
        leveldb
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)

if(NOT USE_SQLITE AND NOT USE_BDB)
  message(FATAL_ERROR "Wallet functionality requested but no BDB or SQLite support available.")
endif()
if(USE_SQLITE)
  target_sources(bitcoin_wallet PRIVATE sqlite.cpp)
  target_link_libraries(bitcoin_wallet
    PRIVATE
      $<TARGET_NAME_IF_EXISTS:unofficial::sqlite3::sqlite3>
      $<TARGET_NAME_IF_EXISTS:SQLite::SQLite3>
  )
endif()
if(USE_BDB)
  target_sources(bitcoin_wallet PRIVATE bdb.cpp salvage.cpp)
  target_link_libraries(bitcoin_wallet PUBLIC BerkeleyDB::BerkeleyDB)
endif()
