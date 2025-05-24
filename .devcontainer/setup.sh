#!/bin/bash
set -e

echo "🚀 Setting up Dash Core development environment..."

# Verify we're in the right environment
if [ ! -f "/src/dash/configure.ac" ]; then
    echo "❌ Error: Dash source not found at /src/dash"
    echo "Current directory: $(pwd)"
    echo "Contents: $(ls -la /src/dash 2>/dev/null || echo 'Directory not found')"
    exit 1
fi

# Ensure we're in the workspace directory
cd /src/dash

# Configure Git safe directory (for when running as different user)
git config --global --add safe.directory /src/dash

# Set up ccache
if command -v ccache >/dev/null 2>&1; then
    echo "📦 Configuring ccache..."
    ccache --set-config max_size=5G
    ccache --set-config compression=true
    ccache --zero-stats
else
    echo "ℹ️  ccache not available, skipping configuration"
fi

echo "🔧 Running autogen.sh..."
./autogen.sh

echo "⚙️  Configuring build system..."
echo "Using Autotools build system..."
./configure \
    --enable-debug \
    --enable-suppress-external-warnings \
    --enable-werror \
    --with-miniupnpc \
    --enable-zmq \
    --enable-glibc-back-compat \
    --enable-reduce-exports \
    CC=clang \
    CXX=clang++

# Create symlink for compile_commands.json if it exists in build directory
# Note: Dash Core uses Autotools, not CMake, so compile_commands.json won't be generated
# You can use tools like bear or compiledb to generate it if needed for IDE support
if [ -f "build/compile_commands.json" ] && [ ! -f "compile_commands.json" ]; then
    echo "🔗 Creating compile_commands.json symlink..."
    ln -sf build/compile_commands.json compile_commands.json
elif [ ! -f "compile_commands.json" ]; then
    echo "ℹ️  No compile_commands.json found. Consider using 'bear make' for IDE support."
fi

# Install Python test dependencies if requirements exist
if [ -f "test/requirements.txt" ]; then
    echo "🐍 Installing Python test dependencies..."
    pip3 install --user -r test/requirements.txt 2>/dev/null || echo "ℹ️  Could not install Python dependencies"
fi

# Install bear if not available (for generating compile_commands.json)
if ! command -v bear >/dev/null 2>&1; then
    echo "📦 Installing bear for compile_commands.json generation..."
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update && sudo apt-get install -y bear 2>/dev/null || echo "ℹ️  Could not install bear"
    fi
fi

# Set up git hooks if they exist
if [ -d "contrib/devtools" ]; then
    echo "🪝 Setting up git hooks..."
    if [ -f "contrib/devtools/git-hooks/pre-commit" ]; then
        cp contrib/devtools/git-hooks/pre-commit .git/hooks/
        chmod +x .git/hooks/pre-commit
    fi
fi

# Print some helpful information
echo ""
echo "✅ Dash Core development environment is ready!"
echo ""
echo "📚 Useful commands:"
echo "  - Build: make -j\$(nproc)"
echo "  - Run tests: make check"
echo "  - Run functional tests: test/functional/test_runner.py"
echo "  - Format code: clang-format -i src/**/*.{cpp,h}"
echo "  - Run linter: contrib/devtools/lint-all.sh"
echo "  - Generate compile_commands.json: bear make -j\$(nproc)"
echo ""
echo "🏗️  Build configuration:"
echo "  - Build system: Autotools"
echo "  - Compiler: $(clang++ --version | head -n1)"
if command -v ccache >/dev/null 2>&1; then
    echo "  - ccache: $(ccache --version | head -n1)"
fi
echo ""
echo "🚢 Port forwards:"
echo "  - 9998: Mainnet RPC"
echo "  - 9999: Mainnet P2P"
echo "  - 19998: Testnet RPC"
echo "  - 19999: Testnet P2P"
echo ""
