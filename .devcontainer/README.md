# Dash Core Development Container

This directory contains a VS Code dev container configuration for Dash Core development. The dev container leverages the existing `contrib/containers/develop` setup, providing a complete, reproducible development environment that's identical to the one used by the Dash Core team.

## Features

- **Consolidated Container Setup**: Built on `contrib/containers/combined.Dockerfile` - a multi-stage build combining ci-slim, ci, and develop stages
- **Complete Build Environment**: All dependencies pre-installed including Clang/LLVM, build tools, and libraries
- **VS Code Integration**: Pre-configured with C++, Python, and debugging extensions
- **Flexible Port Forwarding**: Configurable port forwarding of Dash network ports (9998, 9999, 19998, 19999)
- **Development Tools**: clangd for IntelliSense, clang-format for code formatting, debugging support

## Quick Start

### Prerequisites

- [Docker](https://docs.docker.com/get-docker/)
- [VS Code](https://code.visualstudio.com/)
- [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)

### Usage

1. **Open in Dev Container**:

   - Open VS Code in the Dash repository root
   - Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on Mac)
   - Select "Dev Containers: Reopen in Container"
   - The container will use the `contrib/containers/combined.Dockerfile` with the `develop` target

2. **Build Dash Core**:

   ```bash
   # Using Make (Autotools)
   make -j$(nproc)
   ```

3. **Run Tests**:

   ```bash
   # Unit tests
   make check

   # Functional tests
   test/functional/test_runner.py
   ```

4. **Start Dash Core**:
   ```bash
   # Start regtest node
   ./src/dashd -regtest -server -rpcuser=test -rpcpassword=test
   ```

## Container Details

### Base Setup

- **Container**: Uses `contrib/containers/combined.Dockerfile` with `develop` target via `docker-compose.yml`
- **Multi-stage Build**: Consolidates cppcheck-builder, ci-slim, ci, and develop stages in one Dockerfile
- **OS**: Ubuntu Noble (24.04 LTS)
- **Compiler**: Clang/LLVM 18
- **Build Systems**: Autotools (configure/make) - the official Dash Core build system
- **User**: `dash` user with sudo access

### Installed Packages

- **Development Tools**: clang, clangd, gdb, autotools, ccache
- **Dependencies**: boost, libevent, libzmq, miniupnpc, qt libraries
- **Python**: Python 3 with testing dependencies
- **Utilities**: zsh with oh-my-zsh, git, nano, vim, htop

### Volume Mounts

- **Workspace**: Repository root → `/src/dash` (as defined in existing docker-compose.yml)
- **Docker Socket**: Available for Docker-in-Docker scenarios (if needed)

### Port Forwarding

- **9998**: Mainnet RPC
- **9999**: Mainnet P2P
- **19998**: Testnet RPC
- **19999**: Testnet P2P

## VS Code Configuration

### Extensions

The container automatically installs these VS Code extensions:

- **C/C++ Tools**: IntelliSense, debugging, code browsing
- **clangd**: Advanced C++ language server
- **Python**: Python development support
- **GitLens**: Enhanced Git capabilities
- **Test Explorer**: Test discovery and execution

### Tasks

Pre-configured build tasks accessible via `Ctrl+Shift+P` → "Tasks: Run Task":

- **Build Dash Core**: Default build task using make
- **Configure Build**: Set up Autotools build configuration
- **Run Unit Tests**: Execute unit test suite
- **Run Functional Tests**: Execute functional test suite
- **Generate Compile Commands**: Create compile_commands.json using bear
- **Format Code**: Apply clang-format to source files
- **Run Linter**: Execute code linting
- **Clean Build**: Clean build artifacts

### Debug Configurations

Launch configurations for debugging:

- **Debug dashd**: Debug the Dash daemon in regtest mode
- **Debug dash-cli**: Debug command-line client
- **Debug dash-qt**: Debug Qt GUI application
- **Debug Unit Test**: Debug unit test executable
- **Attach to dashd**: Attach to running dashd process

## Development Workflow

### Initial Setup

The container runs a setup script (`setup.sh`) on first start that:

- Configures Git safe directory
- Sets up ccache with optimal settings (if available)
- Runs `autogen.sh` if needed
- Configures build system (Autotools)
- Creates `compile_commands.json` symlink for clangd (if available)
- Installs Python test dependencies

### Building

```bash
# Configure build (done automatically on first start)
./configure [options]

# Build with make
make -j$(nproc)
```

### Testing

```bash
# Quick unit tests
make check

# Full functional test suite
test/functional/test_runner.py

# Specific functional test
test/functional/example_test.py

# Benchmarks
src/bench/bench_dash
```

### Code Quality

```bash
# Format code
find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Run linter
contrib/devtools/lint-all.sh

# Static analysis with clang-tidy
cd build && clang-tidy ../src/main.cpp
```

## Troubleshooting

### Container Won't Start

- Ensure Docker is running
- Check Docker memory/disk space limits
- Try rebuilding: "Dev Containers: Rebuild Container"
- If the existing container setup has issues, check `contrib/containers/README.md`

### Build Failures

- Clean build: `make clean`
- Check ccache: `ccache -s` and `ccache -C` to clear
- Verify dependencies: `./autogen.sh && ./configure`

### Port Conflicts

- Check if ports 9998, 9999, 19998, 19999 are in use
- Modify port mappings in `docker-compose.yml` if needed

### Performance Issues

- Increase Docker memory allocation (8GB+ recommended)
- Use volume mounts instead of bind mounts on Windows
- Enable Docker BuildKit for faster builds

## Customization

### Using the Combined Container Setup

This dev container configuration leverages the new `contrib/containers/combined.Dockerfile`, which means:

- **Multi-stage Architecture**: Single Dockerfile with 4 stages (cppcheck-builder → ci-slim → ci → develop)
- **No External Dependencies**: No longer requires the `dockerfile-x` syntax extension
- **Consistent with CI**: Same environment used for development and continuous integration
- **Easy Maintenance**: Single file to maintain instead of multiple separate Dockerfiles

### Modify Build Configuration

The existing container already has optimized build settings. If you need to modify them:

- Check `contrib/containers/README.md` for container documentation
- Modify `setup.sh` for post-creation customization
- Add custom build arguments in VS Code tasks

### Adjust VS Code Settings

Edit `.devcontainer/devcontainer.json` to:

- Add/remove extensions
- Modify port forwarding
- Change container settings

### Build Options

Edit build configuration in:

- Autotools: Modify `setup.sh` configure arguments

## Resources

- [Dash Core Developer Documentation](https://docs.dash.org/en/stable/developers/index.html)
- [VS Code Dev Containers Documentation](https://code.visualstudio.com/docs/remote/containers)
- [Docker Documentation](https://docs.docker.com/)
- [Autotools Documentation](https://www.gnu.org/software/automake/manual/automake.html)
- [Clang Documentation](https://clang.llvm.org/docs/)
