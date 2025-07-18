# Builder for cppcheck
FROM debian:bookworm-slim AS cppcheck-builder
ARG CPPCHECK_VERSION=2.13.0
RUN set -ex; \
    apt-get update && apt-get install -y --no-install-recommends \
        curl \
        ca-certificates \
        cmake \
        make \
        g++ \
    && rm -rf /var/lib/apt/lists/*; \
    echo "Downloading Cppcheck version: ${CPPCHECK_VERSION}"; \
    curl -fL "https://github.com/danmar/cppcheck/archive/${CPPCHECK_VERSION}.tar.gz" -o /tmp/cppcheck.tar.gz; \
    mkdir -p /src/cppcheck && tar -xzf /tmp/cppcheck.tar.gz -C /src/cppcheck --strip-components=1; \
    rm /tmp/cppcheck.tar.gz; \
    cd /src/cppcheck; \
    mkdir build && cd build && cmake .. && cmake --build . -j"$(nproc)"; \
    strip bin/cppcheck

# Builder for Python
FROM python:3.9.18-slim-bookworm AS python-builder

# Install Python packages
RUN set -ex; \
    pip3 install --no-cache-dir \
    codespell==1.17.1 \
    flake8==3.8.3 \
    jinja2 \
    lief==0.13.2 \
    multiprocess \
    mypy==0.910 \
    pyzmq==22.3.0 \
    vulture==2.3

# Install packages relied on by tests
ARG DASH_HASH_VERSION=1.4.0
RUN set -ex; \
    apt-get update && apt-get install -y --no-install-recommends \
        git \
        build-essential \
    && rm -rf /var/lib/apt/lists/*; \
    cd /tmp; \
    git clone --depth 1 --no-tags --branch=${DASH_HASH_VERSION} https://github.com/dashpay/dash_hash; \
    cd dash_hash && pip3 install -r requirements.txt .; \
    cd .. && rm -rf dash_hash

# Main image
FROM ubuntu:noble

# Include built assets
COPY --from=cppcheck-builder /src/cppcheck/build/bin/cppcheck /usr/local/bin/cppcheck
COPY --from=cppcheck-builder /src/cppcheck/cfg /usr/local/share/Cppcheck/cfg

# Copy Python installation from python-builder
COPY --from=python-builder /usr/local /usr/local

# Ensure Python shared libraries are found
ENV LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH}"
ENV PATH="/usr/local/bin:${PATH}"

# Needed to prevent tzdata hanging while expecting user input
ENV DEBIAN_FRONTEND="noninteractive" TZ="Europe/London"

# Build and base stuff
ENV APT_ARGS="-y --no-install-recommends --no-upgrade"

# Packages needed for runtime and CI operations
RUN set -ex; \
    apt-get update && apt-get install ${APT_ARGS} \
    build-essential \
    ca-certificates \
    curl \
    g++ \
    git \
    libsqlite3-0 \
    libssl3 \
    make \
    xz-utils \
    zstd \
    && rm -rf /var/lib/apt/lists/*

# Python is already installed via COPY from python-builder stage

ARG SHELLCHECK_VERSION=v0.7.1
RUN set -ex; \
    ARCH=$(uname -m); \
    case ${ARCH} in \
        x86_64) SHELLCHECK_ARCH="x86_64" ;; \
        aarch64) SHELLCHECK_ARCH="aarch64" ;; \
        *) echo "Unsupported architecture: ${ARCH}" && exit 1 ;; \
    esac; \
    curl -fL "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.${SHELLCHECK_ARCH}.tar.xz" -o /tmp/shellcheck.tar.xz; \
    mkdir -p /opt/shellcheck && tar -xf /tmp/shellcheck.tar.xz -C /opt/shellcheck --strip-components=1 && rm /tmp/shellcheck.tar.xz
ENV PATH="/opt/shellcheck:${PATH}"

# Packages needed to be able to run sanitizer builds
ARG LLVM_VERSION=18
RUN set -ex; \
    . /etc/os-release; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc; \
    echo "deb [signed-by=/etc/apt/trusted.gpg.d/apt.llvm.org.asc] http://apt.llvm.org/${UBUNTU_CODENAME}/  llvm-toolchain-${UBUNTU_CODENAME}-${LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list; \
    apt-get update && apt-get install ${APT_ARGS} \
    "llvm-${LLVM_VERSION}-dev"; \
    rm -rf /var/lib/apt/lists/*;

# Setup unprivileged user and configuration files
ARG USER_ID=1000 \
    GROUP_ID=1000
RUN set -ex; \
    groupmod -g ${GROUP_ID} -n dash ubuntu; \
    usermod -u ${USER_ID} -md /home/dash -l dash ubuntu; \
    chown ${USER_ID}:${GROUP_ID} -R /home/dash; \
    mkdir -p /src/dash && \
    chown ${USER_ID}:${GROUP_ID} /src && \
    chown ${USER_ID}:${GROUP_ID} -R /src

WORKDIR /src/dash

USER dash
