#!/usr/bin/env bash
# Copyright (c) 2021-2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# This script is executed inside the builder image

export LC_ALL=C.UTF-8

set -e

source ./ci/dash/matrix.sh

unset CC CXX DISPLAY;

# Set sccache as compiler wrapper when GHA backend is enabled
# For depends-based builds, derive compiler from HOST; for NO_DEPENDS builds, compiler is set in BITCOIN_CONFIG
if command -v sccache &> /dev/null && [ "${SCCACHE_GHA_ENABLED:-}" = "true" ]; then
    sccache --zero-stats 2>/dev/null || true

    # Only set CC/CXX for depends-based builds
    # NO_DEPENDS builds (asan, fuzz, etc.) set compiler in BITCOIN_CONFIG which overrides env vars
    if [ "${NO_DEPENDS}" != "1" ] && [ -n "$HOST" ]; then
        case "$HOST" in
            x86_64-pc-linux-gnu)
                export CC="sccache gcc"
                export CXX="sccache g++"
                ;;
            arm-linux-gnueabihf)
                export CC="sccache arm-linux-gnueabihf-gcc"
                export CXX="sccache arm-linux-gnueabihf-g++"
                ;;
            x86_64-w64-mingw32)
                export CC="sccache x86_64-w64-mingw32-gcc"
                export CXX="sccache x86_64-w64-mingw32-g++"
                ;;
            *-apple-darwin*)
                export CC="sccache clang"
                export CXX="sccache clang++"
                ;;
            s390x-linux-gnu)
                export CC="sccache s390x-linux-gnu-gcc"
                export CXX="sccache s390x-linux-gnu-g++"
                ;;
            *)
                # Unknown host, try HOST-prefixed compiler or fall back to gcc
                if command -v "${HOST}-gcc" &> /dev/null; then
                    export CC="sccache ${HOST}-gcc"
                    export CXX="sccache ${HOST}-g++"
                else
                    export CC="sccache gcc"
                    export CXX="sccache g++"
                fi
                ;;
        esac
    fi
fi

if [ -n "$CONFIG_SHELL" ]; then
  export CONFIG_SHELL="$CONFIG_SHELL"
fi

BITCOIN_CONFIG_ALL="--enable-external-signer --disable-dependency-tracking --prefix=$DEPENDS_DIR/$HOST --bindir=$BASE_OUTDIR/bin --libdir=$BASE_OUTDIR/lib"
if [ -z "$NO_WERROR" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} --enable-werror"
fi

( test -n "$CONFIG_SHELL" && eval '"$CONFIG_SHELL" -c "./autogen.sh"' ) || ./autogen.sh

rm -rf build-ci
mkdir build-ci
cd build-ci

bash -c "../configure $BITCOIN_CONFIG_ALL $BITCOIN_CONFIG" || ( cat config.log && false)
make distdir VERSION="$BUILD_TARGET"

cd "dashcore-$BUILD_TARGET"
bash -c "./configure $BITCOIN_CONFIG_ALL $BITCOIN_CONFIG" || ( cat config.log && false)

# This step influences compilation and therefore will always be a part of the
# compile step
if [ "${RUN_TIDY}" = "true" ]; then
  MAYBE_BEAR="bear --config src/.bear-tidy-config"
  MAYBE_TOKEN="--"
fi

bash -c "${MAYBE_BEAR} ${MAYBE_TOKEN} make ${MAKEJOBS} ${GOAL}" || ( echo "Build failure. Verbose build follows." && make "$GOAL" V=1 ; false )

# Show sccache statistics if enabled
if [ "${SCCACHE_GHA_ENABLED:-}" = "true" ]; then
    sccache --show-stats 2>/dev/null || true
fi

if [ -n "$USE_VALGRIND" ]; then
    echo "valgrind in USE!"
    "${BASE_ROOT_DIR}/ci/test/wrap-valgrind.sh"
fi

# GitHub Actions can segment a job into steps, linting is a separate step
# so Actions runners will perform this step separately.
if [ "${RUN_TIDY}" = "true" ] && [ "${GITHUB_ACTIONS}" != "true" ]; then
  "${BASE_ROOT_DIR}/ci/dash/lint-tidy.sh"
fi

if [ "$RUN_SECURITY_TESTS" = "true" ]; then
  make test-security-check
fi
