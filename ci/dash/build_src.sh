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

# Reset compiler cache stats at start of build
if command -v buildcache &> /dev/null; then
    buildcache -z 2>/dev/null || true
fi
if command -v sccache &> /dev/null; then
    sccache --zero-stats 2>/dev/null || true
fi

if [ -n "$CONFIG_SHELL" ]; then
  export CONFIG_SHELL="$CONFIG_SHELL"
fi

BITCOIN_CONFIG_ALL="--enable-external-signer --disable-dependency-tracking --prefix=$DEPENDS_DIR/$HOST --bindir=$BASE_OUTDIR/bin --libdir=$BASE_OUTDIR/lib"
if [ -z "$NO_WERROR" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} --enable-werror"
fi

# Use buildcache (preferred) or sccache for CI builds when a remote backend is configured
if [ -n "${BUILDCACHE_REMOTE:-}" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} --disable-ccache --disable-sccache --enable-buildcache"
elif [ "${SCCACHE_GHA_ENABLED:-}" = "true" ] || [ -n "${SCCACHE_BUCKET:-}" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} --disable-ccache --enable-sccache"
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

# Show compiler cache statistics
if command -v buildcache &> /dev/null; then
    buildcache -s 2>/dev/null || true
fi
if command -v sccache &> /dev/null; then
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
