FROM ubuntu:jammy

# Prevent tzdata from prompting for input
ENV DEBIAN_FRONTEND="noninteractive" TZ="Europe/London"

# Common APT arguments
ENV APT_ARGS="-y --no-install-recommends --no-upgrade"

#
# -----------------------------------------------------------------------------
# Install base packages
# -----------------------------------------------------------------------------
RUN set -ex; \
    apt-get update && \
    apt-get install $APT_ARGS \
      apt-transport-https \
      ca-certificates \
      gnupg \
      software-properties-common \
      wget \
      curl \
      xz-utils \
    && rm -rf /var/lib/apt/lists/*

#
# -----------------------------------------------------------------------------
# Detect host architecture and add a foreign architecture if needed.
#
# On an amd64 host, add armhf (with its dedicated sources) so that we can install
# the ARM cross‑compiler. On ARM hosts we assume that the x86_64 cross‑compiler is
# provided natively.
# -----------------------------------------------------------------------------
RUN set -ex; \
    ARCH="$(dpkg --print-architecture)"; \
    echo "Host architecture is: ${ARCH}"; \
    if [ "${ARCH}" = "amd64" ]; then \
      echo "Running on amd64; adding armhf as a foreign architecture"; \
      dpkg --add-architecture armhf; \
      echo "deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse" \
        > /etc/apt/sources.list.d/armhf.list; \
    fi; \
    apt-get update

#
# -----------------------------------------------------------------------------
# Install common development packages (including Python via PyEnv)
# -----------------------------------------------------------------------------
ARG PYTHON_VERSION=3.9.18
RUN set -ex; \
    apt-get install $APT_ARGS \
      build-essential \
      autotools-dev \
      automake \
      autoconf \
      bison \
      bsdmainutils \
      ccache \
      cmake \
      gawk \
      gettext \
      git \
      libtool \
      lsb-release \
      m4 \
      pkg-config \
      unzip \
      valgrind \
      zlib1g-dev \
    \
    # Python build dependencies
      libbz2-dev \
      libffi-dev \
      liblzma-dev \
      libncurses5-dev \
      libncursesw5-dev \
      libreadline-dev \
      libsqlite3-dev \
      libssl-dev \
      make \
      tk-dev \
    && rm -rf /var/lib/apt/lists/*

# Install PyEnv and the requested Python version
ENV PYENV_ROOT="/usr/local/pyenv"
ENV PATH="${PYENV_ROOT}/shims:${PYENV_ROOT}/bin:${PATH}"
RUN set -ex; \
    curl https://pyenv.run | bash; \
    pyenv update; \
    pyenv install "${PYTHON_VERSION}"; \
    pyenv global "${PYTHON_VERSION}"; \
    pyenv rehash; \
    pip3 install --no-cache-dir \
      codespell==1.17.1 \
      flake8==3.8.3 \
      jinja2 \
      lief==0.13.2 \
      multiprocess \
      mypy==0.910 \
      pyzmq==22.3.0 \
      vulture==2.3

#
# -----------------------------------------------------------------------------
# Install LLVM/Clang (example: version 18) and set it as default
# -----------------------------------------------------------------------------
ARG LLVM_VERSION=18
RUN set -ex; \
    curl -sL https://apt.llvm.org/llvm.sh | bash -s -- "${LLVM_VERSION}"; \
    apt-get update && apt-get install $APT_ARGS \
      "clang-format-${LLVM_VERSION}" \
      "clang-tidy-${LLVM_VERSION}" \
      "libc++-${LLVM_VERSION}-dev" \
      "libc++abi-${LLVM_VERSION}-dev" \
      "libclang-rt-${LLVM_VERSION}-dev" \
      "lld-${LLVM_VERSION}" \
    && rm -rf /var/lib/apt/lists/*; \
    # Set up update-alternatives for LLVM tools
    ALT_CMD="update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-${LLVM_VERSION} 100"; \
    for binName in clang clang++ clang-format clang-tidy clangd dsymutil lld lldb lldb-server llvm-ar llvm-cov llvm-nm llvm-objdump llvm-ranlib llvm-strip; do \
        ALT_CMD="$ALT_CMD --slave /usr/bin/${binName} ${binName} /usr/bin/${binName}-${LLVM_VERSION}"; \
    done; \
    for binName in ld64.lld ld.lld lld-link wasm-ld; do \
        ALT_CMD="$ALT_CMD --slave /usr/bin/${binName} ${binName} /usr/bin/lld-${LLVM_VERSION}"; \
    done; \
    sh -c "$ALT_CMD"

# Add LLVM libraries to LD_LIBRARY_PATH
ENV LD_LIBRARY_PATH="/usr/lib/llvm-${LLVM_VERSION}/lib"

#
# -----------------------------------------------------------------------------
# Install cross compilers depending on host architecture.
#
# - On amd64: install the ARM cross-compiler packages and create symlinks so that
#           "arm-linux-gnueabihf-gcc" and "arm-linux-gnueabihf-g++" are available.
# - On ARM: install the x86_64 cross-compiler packages and create symlinks so that
#           "x86_64-pc-linux-gnu-gcc" and "x86_64-pc-linux-gnu-g++" are available.
# -----------------------------------------------------------------------------
RUN set -ex; \
    ARCH="$(dpkg --print-architecture)"; \
    echo "Installing cross-compiler for ARM on amd64"; \
    apt-get update && apt-get install $APT_ARGS \
    gcc-arm-linux-gnueabihf \
    g++-arm-linux-gnueabihf; \
    \
    # Create symlinks for the ARM cross-compiler.
    # (Ubuntu Jammy typically installs versioned binaries—adjust the version if needed.)
    ln -sf /usr/bin/arm-linux-gnueabihf-gcc-11 /usr/bin/arm-linux-gnueabihf-gcc; \
    ln -sf /usr/bin/arm-linux-gnueabihf-g++-11 /usr/bin/arm-linux-gnueabihf-g++; \
    if [ "${ARCH}" = "arm64" ]; then \
      echo "Installing cross-compiler for x86_64 on arm64"; \
      apt-get update && apt-get install $APT_ARGS \
        gcc-x86-64-linux-gnu \
        g++-x86-64-linux-gnu; \
      \
      # Create symlinks so that a build expecting x86_64-pc-linux-gnu-gcc/g++ will find the compiler.
      ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/bin/x86_64-pc-linux-gnu-gcc; \
      ln -sf /usr/bin/x86_64-linux-gnu-g++ /usr/bin/x86_64-pc-linux-gnu-g++; \
    fi && \
    rm -rf /var/lib/apt/lists/*

#
# -----------------------------------------------------------------------------
# Additional Tools (ShellCheck, dash_hash, Cppcheck, etc.)
# -----------------------------------------------------------------------------
ARG SHELLCHECK_VERSION=v0.9.0
RUN set -ex; \
    # Example: download ShellCheck (note: the x86_64 binary won't run natively on ARM)
    curl -fL \
      "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" \
      -o /tmp/shellcheck.tar.xz || true; \
    rm -f /tmp/shellcheck.tar.xz

# dash_hash
ARG DASH_HASH_VERSION=1.4.0
RUN set -ex; \
    cd /tmp; \
    git clone --depth 1 --no-tags --branch="${DASH_HASH_VERSION}" \
      https://github.com/dashpay/dash_hash; \
    cd dash_hash && pip3 install -r requirements.txt .; \
    cd .. && rm -rf dash_hash

# Cppcheck
ARG CPPCHECK_VERSION=2.13.0
RUN set -ex; \
    curl -fL "https://github.com/danmar/cppcheck/archive/${CPPCHECK_VERSION}.tar.gz" -o /tmp/cppcheck.tar.gz; \
    mkdir -p /opt/cppcheck && tar -xzf /tmp/cppcheck.tar.gz -C /opt/cppcheck --strip-components=1; \
    cd /opt/cppcheck; \
    mkdir build && cd build && cmake .. && cmake --build . -j"$(nproc)"; \
    mkdir /usr/local/share/Cppcheck && ln -s /opt/cppcheck/cfg/ /usr/local/share/Cppcheck/cfg; \
    rm -rf /tmp/cppcheck*

ENV PATH="/opt/cppcheck/build/bin:${PATH}"

#
# -----------------------------------------------------------------------------
# Add a non-root user
# -----------------------------------------------------------------------------
ARG USER_ID=1000
ARG GROUP_ID=1000
RUN set -ex; \
    groupadd -g "${GROUP_ID}" dash; \
    useradd -u "${USER_ID}" -g dash -s /bin/bash -m -d /home/dash dash; \
    chown "${USER_ID}:${GROUP_ID}" -R /home/dash

# Prepare a work directory for building
RUN mkdir -p /src/dash && chown dash:dash /src/dash
WORKDIR /src/dash

USER dash