# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(GNUInstallDirs)
include(AddWindowsResources)

configure_file(${PROJECT_SOURCE_DIR}/cmake/bitcoin-config.h.in config/bitcoin-config.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

# TODO: After the transition from Autotools to CMake, the obj/ subdirectory
#       could be dropped as its only purpose was to separate a generated header
#       from source files.
add_custom_target(generate_build_info
  BYPRODUCTS ${PROJECT_BINARY_DIR}/src/obj/build.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/src/obj
  COMMAND ${CMAKE_COMMAND} -DBUILD_INFO_HEADER_PATH=${PROJECT_BINARY_DIR}/src/obj/build.h -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/script/GenerateBuildInfo.cmake
  COMMENT "Generating obj/build.h"
  VERBATIM
)
add_library(bitcoin_clientversion OBJECT EXCLUDE_FROM_ALL
  clientversion.cpp
)
target_compile_definitions(bitcoin_clientversion
  PRIVATE
    HAVE_BUILD_INFO
)
target_link_libraries(bitcoin_clientversion
  PRIVATE
    core_interface
)
add_dependencies(bitcoin_clientversion generate_build_info)

add_subdirectory(crypto)
add_subdirectory(univalue)
add_subdirectory(util)
if(WITH_MULTIPROCESS)
  add_subdirectory(ipc)
endif()

#=============================
# secp256k1 subtree
#=============================
message("")
message("Configuring secp256k1 subtree...")
set(SECP256K1_DISABLE_SHARED ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
include(GetTargetInterface)
# -fsanitize and related flags apply to both C++ and C,
# so we can pass them down to libsecp256k1 as CFLAGS.
get_target_interface(core_sanitizer_cxx_flags "" sanitize_interface COMPILE_OPTIONS)
set(SECP256K1_LATE_CFLAGS ${core_sanitizer_cxx_flags} CACHE STRING "" FORCE)
unset(core_sanitizer_cxx_flags)
# We want to build libsecp256k1 with the most tested RelWithDebInfo configuration.
enable_language(C)
foreach(config IN LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
  if(config STREQUAL "")
    continue()
  endif()
  string(TOUPPER "${config}" config)
  set(CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endforeach()
# If the CFLAGS environment variable is defined during building depends
# and configuring this build system, its content might be duplicated.
if(DEFINED ENV{CFLAGS})
  deduplicate_flags(CMAKE_C_FLAGS)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)
add_subdirectory(secp256k1)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
string(APPEND CMAKE_C_COMPILE_OBJECT " ${APPEND_CPPFLAGS} ${APPEND_CFLAGS}")

# Stable, backwards-compatible consensus functionality.
add_library(bitcoin_consensus STATIC EXCLUDE_FROM_ALL
  arith_uint256.cpp
  consensus/merkle.cpp
  consensus/tx_check.cpp
  hash.cpp
  primitives/block.cpp
  primitives/transaction.cpp
  pubkey.cpp
  script/interpreter.cpp
  script/script.cpp
  script/script_error.cpp
  uint256.cpp
)
target_link_libraries(bitcoin_consensus
  PRIVATE
    core_interface
    bitcoin_crypto
    secp256k1
)

if(WITH_ZMQ)
  add_subdirectory(zmq)
endif()

# Home for common functionality shared by different executables and libraries.
# Similar to `bitcoin_util` library, but higher-level.
add_library(bitcoin_common STATIC EXCLUDE_FROM_ALL 
  base58.cpp
  bech32.cpp
  bloom.cpp
  chainparams.cpp
  coins.cpp
  compressor.cpp
  core_read.cpp
  core_write.cpp
  deploymentinfo.cpp
  governance/common.cpp
  init/common.cpp
  key.cpp
  key_io.cpp
  merkleblock.cpp
  net_types.cpp
  netaddress.cpp
  netbase.cpp
  net_permissions.cpp
  outputtype.cpp
  policy/feerate.cpp
  policy/policy.cpp
  protocol.cpp
  psbt.cpp
  rpc/rawtransaction_util.cpp
  rpc/util.cpp
  saltedhasher.cpp
  scheduler.cpp
  script/descriptor.cpp
  script/sign.cpp
  script/signingprovider.cpp
  script/standard.cpp
  warnings.cpp
)
target_link_libraries(bitcoin_common
  PRIVATE
    core_interface
    bitcoin_consensus
    bitcoin_util
    univalue
    secp256k1
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
    $<$<PLATFORM_ID:Windows>:ws2_32>
)


set(installable_targets)
if(ENABLE_WALLET)
  add_subdirectory(wallet)

  if(BUILD_WALLET_TOOL)
    add_executable(bitcoin-wallet
      bitcoin-wallet.cpp
#      init/bitcoin-wallet.cpp
      wallet/wallettool.cpp
    )
    add_windows_resources(bitcoin-wallet bitcoin-wallet-res.rc)
    target_link_libraries(bitcoin-wallet
      core_interface
      bitcoin_wallet
      bitcoin_common
      bitcoin_util
      Boost::headers
    )
    list(APPEND installable_targets bitcoin-wallet)
  endif()
endif()


# P2P and RPC server functionality used by `bitcoind` and `bitcoin-qt` executables.
add_library(bitcoin_node STATIC EXCLUDE_FROM_ALL
        addrdb.cpp
  addressindex.cpp
  addrman.cpp
  banman.cpp
  batchedlogger.cpp
  bip324.cpp
  blockencodings.cpp
  blockfilter.cpp
  chain.cpp
  coinjoin/coinjoin.cpp
  coinjoin/context.cpp
  coinjoin/server.cpp
  consensus/tx_verify.cpp
  dbwrapper.cpp
  deploymentstatus.cpp
  dsnotificationinterface.cpp
  evo/assetlocktx.cpp
  evo/cbtx.cpp
  evo/creditpool.cpp
  evo/chainhelper.cpp
  evo/deterministicmns.cpp
  evo/dmnstate.cpp
  evo/evodb.cpp
  evo/mnauth.cpp
  evo/mnhftx.cpp
  evo/providertx.cpp
  evo/simplifiedmns.cpp
  evo/specialtx.cpp
  evo/specialtxman.cpp
  flatfile.cpp
  governance/classes.cpp
  governance/exceptions.cpp
  governance/governance.cpp
  governance/object.cpp
  governance/validators.cpp
  governance/vote.cpp
  governance/votedb.cpp
  gsl/assert.cpp
  httprpc.cpp
  httpserver.cpp
  i2p.cpp
  index/base.cpp
  index/blockfilterindex.cpp
  index/coinstatsindex.cpp
  index/txindex.cpp
  init.cpp
  llmq/blockprocessor.cpp
  llmq/chainlocks.cpp
  llmq/clsig.cpp
  llmq/commitment.cpp
  llmq/context.cpp
  llmq/debug.cpp
  llmq/dkgsession.cpp
  llmq/dkgsessionhandler.cpp
  llmq/dkgsessionmgr.cpp
  llmq/ehf_signals.cpp
  llmq/instantsend.cpp
  llmq/options.cpp
  llmq/quorums.cpp
  llmq/signing.cpp
  llmq/signing_shares.cpp
  llmq/snapshot.cpp
  llmq/utils.cpp
  mapport.cpp
  masternode/node.cpp
  masternode/meta.cpp
  masternode/payments.cpp
  masternode/sync.cpp
  masternode/utils.cpp
  miner.cpp
  net.cpp
  netfulfilledman.cpp
  netgroup.cpp
  net_processing.cpp
  node/blockstorage.cpp
  node/coin.cpp
  node/coinstats.cpp
  node/connection_types.cpp
  node/context.cpp
  node/eviction.cpp
  node/interfaces.cpp
  node/psbt.cpp
  node/transaction.cpp
  node/ui_interface.cpp
  noui.cpp
  policy/fees.cpp
  policy/packages.cpp
  policy/policy.cpp
  policy/settings.cpp
  pow.cpp
  rest.cpp
  rpc/blockchain.cpp
  rpc/coinjoin.cpp
  rpc/evo.cpp
  rpc/index_util.cpp
  rpc/masternode.cpp
  rpc/governance.cpp
  rpc/mining.cpp
  rpc/misc.cpp
  rpc/net.cpp
  rpc/quorums.cpp
  rpc/rawtransaction.cpp
  rpc/server.cpp
  rpc/server_util.cpp
  script/sigcache.cpp
  shutdown.cpp
  spork.cpp
  statsd_client.cpp
  timedata.cpp
  torcontrol.cpp
  txdb.cpp
  txmempool.cpp
  txorphanage.cpp
  validation.cpp
  validationinterface.cpp
  versionbits.cpp
  $<$<TARGET_EXISTS:bitcoin_wallet>:wallet/init.cpp>
  $<$<NOT:$<TARGET_EXISTS:bitcoin_wallet>>:dummywallet.cpp>
)
target_link_libraries(bitcoin_node
  PRIVATE
    core_interface
    bitcoin_common
    bitcoin_util
    leveldb
    minisketch
    univalue
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:libevent::libevent>
    $<TARGET_NAME_IF_EXISTS:libevent::pthreads>
    $<TARGET_NAME_IF_EXISTS:NATPMP::NATPMP>
    $<TARGET_NAME_IF_EXISTS:MiniUPnPc::MiniUPnPc>
    $<TARGET_NAME_IF_EXISTS:bitcoin_zmq>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)


# Bitcoin Core bitcoind.
if(BUILD_DAEMON)
  add_executable(bitcoind
    bitcoind.cpp
    init/bitcoind.cpp
  )
  add_windows_resources(bitcoind bitcoind-res.rc)
  target_link_libraries(bitcoind
    core_interface
    bitcoin_node
    $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  )
  list(APPEND installable_targets bitcoind)
endif()
if(WITH_MULTIPROCESS)
  add_executable(bitcoin-node
    bitcoind.cpp
    init/bitcoin-node.cpp
  )
  target_link_libraries(bitcoin-node
    core_interface
    bitcoin_node
    bitcoin_ipc
    $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  )
  list(APPEND installable_targets bitcoin-node)
endif()


add_library(bitcoin_cli STATIC EXCLUDE_FROM_ALL
  compat/stdin.cpp
  rpc/client.cpp
)
target_link_libraries(bitcoin_cli
  PUBLIC
    core_interface
    univalue
)


# Bitcoin Core RPC client
if(BUILD_CLI)
  add_executable(bitcoin-cli bitcoin-cli.cpp)
  add_windows_resources(bitcoin-cli bitcoin-cli-res.rc)
  target_link_libraries(bitcoin-cli
    core_interface
    bitcoin_cli
    bitcoin_common
    bitcoin_util
    $<TARGET_NAME_IF_EXISTS:libevent::libevent>
  )
  list(APPEND installable_targets bitcoin-cli)
endif()


if(BUILD_TX)
  add_executable(bitcoin-tx bitcoin-tx.cpp)
  add_windows_resources(bitcoin-tx bitcoin-tx-res.rc)
  target_link_libraries(bitcoin-tx
    core_interface
    bitcoin_common
    bitcoin_util
    univalue
  )
  list(APPEND installable_targets bitcoin-tx)
endif()


if(BUILD_UTIL)
  add_executable(bitcoin-util bitcoin-util.cpp)
  add_windows_resources(bitcoin-util bitcoin-util-res.rc)
  target_link_libraries(bitcoin-util
    core_interface
    bitcoin_common
    bitcoin_util
  )
  list(APPEND installable_targets bitcoin-util)
endif()


if(BUILD_GUI)
  add_subdirectory(qt)
endif()


if(BUILD_KERNEL_LIB)
  add_subdirectory(kernel)
endif()

if(BUILD_UTIL_CHAINSTATE)
  add_executable(bitcoin-chainstate
    bitcoin-chainstate.cpp
  )
  # TODO: The `SKIP_BUILD_RPATH` property setting can be deleted
  #       in the future after reordering Guix script commands to
  #       perform binary checks after the installation step.
  # Relevant discussions:
  # - https://github.com/hebasto/bitcoin/pull/236#issuecomment-2183120953
  # - https://github.com/bitcoin/bitcoin/pull/30312#issuecomment-2191235833
  set_target_properties(bitcoin-chainstate PROPERTIES
    SKIP_BUILD_RPATH OFF
  )
  target_link_libraries(bitcoin-chainstate
    PRIVATE
      core_interface
      bitcoinkernel
  )
endif()


add_subdirectory(test/util)
if(BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(BUILD_TESTS)
  add_subdirectory(test)
endif()

if(BUILD_FUZZ_BINARY)
  add_subdirectory(test/fuzz)
endif()


install(TARGETS ${installable_targets}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
unset(installable_targets)

if(INSTALL_MAN)
  # TODO: these stubs are no longer needed. man pages should be generated at install time.
  install(DIRECTORY ../doc/man/
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    FILES_MATCHING PATTERN *.1
  )
endif()
