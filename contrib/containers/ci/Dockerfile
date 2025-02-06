FROM ubuntu:jammy

# Needed to prevent tzdata hanging while expecting user input
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


# -----------------------------------------------------------------------------
# Detect host architecture, add foreign architecture, and add a proper source
# list for the foreign architecture
# -----------------------------------------------------------------------------
RUN set -ex; \
    ARCH="$(dpkg --print-architecture)"; \
    echo "Host architecture is: ${ARCH}"; \
    if [ "${ARCH}" = "amd64" ]; then \
      # We're running on amd64 and want to cross-compile for armhf.
      dpkg --add-architecture armhf; \
      echo "deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse" \
        > /etc/apt/sources.list.d/armhf.list; \
    elif [ "${ARCH}" = "arm64" ]; then \
      # We're running on arm64 and want to cross-compile for amd64.
      dpkg --add-architecture amd64; \
      echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy main restricted universe multiverse" \
        > /etc/apt/sources.list.d/amd64.list; \
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
    # Python build deps
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
    \
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
# Install LLVM/Clang (example: version 18), set it as default
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
    # update-alternatives
    ALT_CMD="update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-${LLVM_VERSION} 100"; \
    for binName in clang clang++ clang-format clang-tidy clangd dsymutil lld lldb lldb-server llvm-ar llvm-cov llvm-nm llvm-objdump llvm-ranlib llvm-strip; do \
        ALT_CMD="$ALT_CMD --slave /usr/bin/${binName} ${binName} /usr/bin/${binName}-${LLVM_VERSION}"; \
    done; \
    for binName in ld64.lld ld.lld lld-link wasm-ld; do \
        ALT_CMD="$ALT_CMD --slave /usr/bin/${binName} ${binName} /usr/bin/lld-${LLVM_VERSION}"; \
    done; \
    sh -c "$ALT_CMD"

# LD_LIBRARY_PATH is empty by default, so let's add the LLVM libs
ENV LD_LIBRARY_PATH="/usr/lib/llvm-${LLVM_VERSION}/lib"

#
# -----------------------------------------------------------------------------
# Install cross compilers *depending on host architecture*
# -----------------------------------------------------------------------------
RUN set -ex; \
    ARCH="$(dpkg --print-architecture)"; \
    if [ "${ARCH}" = "amd64" ]; then \
      # Cross-compiler for 32-bit ARM
      apt-get update && apt-get install $APT_ARGS \
        g++-arm-linux-gnueabihf; \
    elif [ "${ARCH}" = "arm64" ]; then \
      # Cross-compiler for x86_64
      apt-get update && apt-get install $APT_ARGS \
        g++-x86-64-linux-gnu; \
    else \
      echo "Warning: no cross-compiler installed for architecture: ${ARCH}"; \
    fi && \
    rm -rf /var/lib/apt/lists/*

#
# -----------------------------------------------------------------------------
# Example: Additional Tools (ShellCheck, Cppcheck, dash_hash, etc.)
# -----------------------------------------------------------------------------
ARG SHELLCHECK_VERSION=v0.9.0
RUN set -ex; \
    # If you need shellcheck for scripts:
    curl -fL \
      "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" \
      -o /tmp/shellcheck.tar.xz || true; \
    # The above .x86_64 package obviously won't run natively on ARM, so you might need QEMU or a separate ARM build
    # => For demonstration only
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
# Add non-root user
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