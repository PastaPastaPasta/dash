# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

enable_language(C)
string(APPEND CMAKE_C_COMPILE_OBJECT " ${APPEND_CPPFLAGS} ${APPEND_CFLAGS}")

add_library(bitcoin_x11crypto STATIC EXCLUDE_FROM_ALL
    aes_helper.c
    blake.c
    bmw.c
    cubehash.c
    echo.c
    groestl.c
    jh.c
    keccak.c
    luffa.c
    shavite.c
    simd.c
    skein.c

    ../../support/cleanse.cpp
)

target_link_libraries(bitcoin_x11crypto
    PRIVATE
    core_interface
)
