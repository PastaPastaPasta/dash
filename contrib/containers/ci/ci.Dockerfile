# syntax = devthefuture/dockerfile-x

FROM ./ci-slim.Dockerfile

# The inherited Dockerfile switches to non-privileged context and we've
# just started configuring this image, give us root access
USER root

# Install packages
RUN set -ex; \
    apt-get update && apt-get install ${APT_ARGS} \
    autoconf \
    automake \
    autotools-dev \
    bc \
    bear \
    bison \
    ccache \
    cmake \
    g++-11 \
    g++-14 \
    g++-arm-linux-gnueabihf \
    g++-mingw-w64-x86-64 \
    gawk \
    gettext \
    libtool \
    m4 \
    pkg-config \
    wine-stable \
    wine64 \
    zip \
    && rm -rf /var/lib/apt/lists/*

# Configure MinGW to use POSIX threading model (required for std::condition_variable)
# RocksDB (used by GroveDB) needs POSIX threads for condition variables.
# The win32 threading model lacks __gthread_cond_t support.
RUN set -ex; \
    update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix; \
    update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

# Install Clang + LLVM and set it as default
RUN set -ex; \
    apt-get update && apt-get install ${APT_ARGS} \
    "clang-${LLVM_VERSION}" \
    "clangd-${LLVM_VERSION}" \
    "clang-format-${LLVM_VERSION}" \
    "clang-tidy-${LLVM_VERSION}" \
    "libc++-${LLVM_VERSION}-dev" \
    "libc++abi-${LLVM_VERSION}-dev" \
    "libclang-${LLVM_VERSION}-dev" \
    "libclang-rt-${LLVM_VERSION}-dev" \
    "lld-${LLVM_VERSION}" \
    "lldb-${LLVM_VERSION}"; \
    rm -rf /var/lib/apt/lists/*; \
    echo "Setting defaults..."; \
    llvmUpdAltArgs="update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-${LLVM_VERSION} 100"; \
    for binName in clang clang++ clang-apply-replacements clang-format clang-tidy clangd dsymutil lld lldb lldb-server llvm-ar llvm-cov llvm-nm llvm-objdump llvm-ranlib llvm-strip run-clang-tidy; do \
        llvmUpdAltArgs="${llvmUpdAltArgs} --slave /usr/bin/${binName} ${binName} /usr/bin/${binName}-${LLVM_VERSION}"; \
    done; \
    for binName in ld64.lld ld.lld lld-link wasm-ld; do \
        llvmUpdAltArgs="${llvmUpdAltArgs} --slave /usr/bin/${binName} ${binName} /usr/bin/lld-${LLVM_VERSION}"; \
    done; \
    sh -c "${llvmUpdAltArgs}";
# LD_LIBRARY_PATH is empty by default, this is the first entry
ENV LD_LIBRARY_PATH="/usr/lib/llvm-${LLVM_VERSION}/lib"

RUN set -ex; \
    git clone --depth=1 "https://github.com/include-what-you-use/include-what-you-use" -b "clang_${LLVM_VERSION}" /opt/iwyu; \
    cd /opt/iwyu; \
    mkdir build && cd build; \
    cmake -G 'Unix Makefiles' -DCMAKE_PREFIX_PATH=/usr/lib/llvm-${LLVM_VERSION} ..; \
    make install -j "$(( $(nproc) - 1 ))"; \
    cd /opt && rm -rf /opt/iwyu;

# ===========================================================================
# Rust toolchain for GroveDB (requires Rust 1.82+ for dependencies)
# ===========================================================================

# Install Rust to a shared location accessible by all users
ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV PATH="/opt/cargo/bin:${PATH}"

# Install Rust via rustup (stable toolchain, minimal profile)
RUN set -ex; \
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y \
        --default-toolchain stable \
        --profile minimal; \
    chmod -R a+rX /opt/rustup /opt/cargo; \
    rm -rf /opt/cargo/registry/cache

# Add cross-compilation targets for all supported platforms
# Note: macOS cross-compilation requires passing CC/linker from the depends system
# via environment variables at build time (handled in Makefile.grovedb.include)
RUN set -ex; \
    rustup target add \
        x86_64-unknown-linux-gnu \
        aarch64-unknown-linux-gnu \
        armv7-unknown-linux-gnueabihf \
        x86_64-pc-windows-gnu \
        i686-pc-windows-gnu \
        x86_64-apple-darwin \
        aarch64-apple-darwin

# Configure cargo for cross-compilation linkers (Linux/Windows only)
# macOS linkers are configured dynamically via Makefile.grovedb.include
# since they come from the depends/ build system
RUN set -ex; \
    printf '%s\n' \
        '[target.x86_64-pc-windows-gnu]' \
        'linker = "x86_64-w64-mingw32-gcc"' \
        '' \
        '[target.aarch64-unknown-linux-gnu]' \
        'linker = "aarch64-linux-gnu-gcc"' \
        '' \
        '[target.armv7-unknown-linux-gnueabihf]' \
        'linker = "arm-linux-gnueabihf-gcc"' \
        > /opt/cargo/config.toml

# Verify Rust installation
RUN cargo --version && rustc --version

RUN \
  mkdir -p /cache/ccache && \
  mkdir /cache/depends && \
  mkdir /cache/sdk-sources && \
  chown ${USER_ID}:${GROUP_ID} /cache && \
  chown ${USER_ID}:${GROUP_ID} -R /cache

# We're done, switch back to non-privileged user
USER dash
