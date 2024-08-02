FROM ubuntu:focal

# Set non-interactive frontend and timezone to prevent tzdata hanging during builds
ENV DEBIAN_FRONTEND="noninteractive" \
    TZ="Europe/London" \
    APT_ARGS="-y --no-install-recommends --no-upgrade" \
    PYENV_ROOT="/usr/local/pyenv" \
    PATH="${PYENV_ROOT}/shims:${PYENV_ROOT}/bin:${PATH}" \
    USER_ID="1000" \
    GROUP_ID="1000"

# One RUN command to update, install, and clean up in one layer
RUN dpkg --print-architecture | grep -Eq 'aarch64|arm64' || dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install $APT_ARGS \
    autotools-dev \
    automake \
    autoconf \
    bison \
    build-essential \
    bsdmainutils \
    curl \
    ccache \
    clang \
    cmake \
    g++ \
    gettext \
    git \
    libc++-dev \
    libc++abi-dev \
    libtool \
    libxcb-icccm4 \
    libxcb-image0 \
    libxcb-keysyms1 \
    libxcb-randr0 \
    libxcb-render-util0 \
    libxcb-shape0 \
    libxcb-sync1 \
    libxcb-xfixes0 \
    libxcb-xinerama0 \
    libxcb-xkb1 \
    libxkbcommon-x11-0 \
    wget \
    unzip \
    m4 \
    pkg-config \
    zlib1g-dev \
    bc \
    gawk \
    g++-arm-linux-gnueabihf \
    g++-mingw-w64-x86-64 \
    jq \
    libz-dev \
    libncurses5 \
    nsis \
    python3-zmq \
    parallel \
    valgrind \
    wine-stable \
    wine64 \
    xorriso \
    ca-certificates \
    libbz2-dev \
    libffi-dev \
    liblzma-dev \
    libncurses5-dev \
    libncursesw5-dev \
    libreadline-dev \
    libsqlite3-dev \
    libssl-dev \
    llvm \
    make \
    tk-dev \
    lsb-release \
    software-properties-common \
    gnupg \
    xz-utils && \
    rm -rf /var/lib/apt/lists/*

# Install Python and Python packages
ENV PYENV_ROOT "/usr/local/pyenv"
ENV PATH "${PYENV_ROOT}/shims:${PYENV_ROOT}/bin:${PATH}"
ENV PYTHON_VERSION=3.8.16
RUN curl https://pyenv.run | bash && \
    pyenv update && \
    pyenv install $PYTHON_VERSION && \
    pyenv global $PYTHON_VERSION && \
    pyenv rehash && \
    pip3 install codespell==1.17.1 flake8==3.8.3 jinja2 lief==0.12.1 pyzmq vulture==2.3 mypy==0.781 yq multiprocess

# Clone and install dash_hash
RUN git clone --depth 1 --no-tags --branch=1.4.0 https://github.com/dashpay/dash_hash /dash_hash && \
    cd /dash_hash && pip3 install -r requirements.txt && \
    rm -rf /dash_hash

# Setup tools
ENV PATH "/tmp/cppcheck-${CPPCHECK_VERSION}/build/bin:${PATH}"
ENV PATH "/tmp/shellcheck-${SHELLCHECK_VERSION}:${PATH}"
ARG LLVM_VERSION=16
ARG CPPCHECK_VERSION=2.13.0
ARG SHELLCHECK_VERSION=v0.7.1
RUN curl -sL "https://github.com/danmar/cppcheck/archive/${CPPCHECK_VERSION}.tar.gz" | tar -xvzf - --directory /tmp/ && \
    cd /tmp/cppcheck-${CPPCHECK_VERSION} && mkdir build && cd build && cmake .. && cmake --build . -j 8 && \
    mkdir /usr/local/share/Cppcheck && ln -s /tmp/cppcheck-${CPPCHECK_VERSION}/cfg/ /usr/local/share/Cppcheck/cfg && \
    curl -sL "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" | tar --xz -xf - --directory /tmp/ && \
    ln -s x86_64-linux-gnu/asm /usr/include/asm && \
    update-alternatives --set x86_64-w64-mingw32-gcc  /usr/bin/x86_64-w64-mingw32-gcc-posix && \
    update-alternatives --set x86_64-w64-mingw32-g++  /usr/bin/x86_64-w64-mingw32-g++-posix && \
    #Setup LLVM
    wget -O /tmp/llvm.sh https://apt.llvm.org/llvm.sh && \
    chmod +x /tmp/llvm.sh && \
    /tmp/llvm.sh ${LLVM_VERSION} && \
    rm -rf /tmp/llvm.sh

# Setup user and directories
RUN groupadd -g ${GROUP_ID} dash && \
    useradd -u ${USER_ID} -g dash -s /bin/bash -m -d /home/dash dash && \
    mkdir -p /src/dash && \
    mkdir -p /cache/ccache && \
    mkdir /cache/depends && \
    mkdir /cache/sdk-sources && \
    chown ${USER_ID}:${GROUP_ID} /src /cache -R

WORKDIR /src/dash
USER dash