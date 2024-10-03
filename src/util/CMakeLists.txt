# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(bitcoin_util STATIC EXCLUDE_FROM_ALL
#  ../bls/bls_batchverifier.h
  ../bls/bls_ies.cpp
#  ../bls/bls_ies.h
  ../bls/bls_worker.cpp
#  ../bls/bls_worker.h
  ../coinjoin/common.cpp
  ../coinjoin/options.cpp
  ../support/lockedpool.cpp
  ../chainparamsbase.cpp
  ../clientversion.cpp
  ../compat/strnlen.cpp
  ../fs.cpp
  ../interfaces/echo.cpp
  ../interfaces/handler.cpp
  ../interfaces/init.cpp
  ../logging.cpp
  ../messagesigner.cpp
  ../random.cpp
  ../randomenv.cpp
  ../rpc/request.cpp
  ../stacktraces.cpp
  ../support/cleanse.cpp
  ../sync.cpp
  ../threadinterrupt.cpp
  asmap.cpp
  bip32.cpp
  bytevectorhash.cpp
  check.cpp
  edge.cpp
  error.cpp
  fees.cpp
  hasher.cpp
  getuniquepath.cpp
  sock.cpp
  system.cpp
  message.cpp
  moneystr.cpp
  readwritefile.cpp
  settings.cpp
  ranges_set.cpp
  spanparsing.cpp
  strencodings.cpp
  time.cpp
  serfloat.cpp
  string.cpp
  thread.cpp
  threadnames.cpp
  tokenpipe.cpp
  wpipe.cpp
)

target_link_libraries(bitcoin_util
  PRIVATE
    core_interface
    bitcoin_clientversion
    bitcoin_crypto
    dashbls
    $<$<PLATFORM_ID:Windows>:ws2_32>
)
