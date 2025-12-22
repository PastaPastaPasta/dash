# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build (uses stable Rust)
cargo build

# Run all tests with all features
cargo test --workspace --all-features

# Run a single test
cargo test test_name --workspace --all-features

# Run tests for a specific crate
cargo test -p grovedb --all-features
cargo test -p grovedb-merk --all-features

# Check compilation
cargo check

# Build proof verification only (no RocksDB dependency)
cargo build --no-default-features --features verify -p grovedb

# Linting
cargo clippy --all-features

# Formatting (requires nightly)
cargo +nightly fmt
cargo +nightly fmt --check
```

## Architecture

GroveDB is a hierarchical authenticated data structure optimized for efficient secondary index queries and cryptographic proofs. Built for Dash Platform.

### Workspace Crates

- **grovedb** - Main database implementation with insert/delete/query/proof operations
- **merk** - Merkle AVL tree (fork with custom patches); the underlying authenticated data structure
- **storage** - RocksDB storage layer abstraction
- **costs** - Operation cost tracking for resource accounting
- **path** - Path type utilities for tree navigation
- **grovedb-version** - Versioning types (`GroveVersion`)
- **visualize** - Visualization trait implementations
- **grovedbg-types** - Types for the debugger tool
- **node-grove** - Node.js bindings

### Core Concepts

**Element Types**: Items (arbitrary bytes), References (pointers), Trees (subtrees), SumItems (integers for aggregation), SumTrees (trees that compute sums)

**Tree Hierarchy**: GroveDB is a "grove" - a tree of Merk trees. The root tree is accessed via empty path `[]`. Inserting a Tree/SumTree element creates a new sub-Merk.

**Root Hash**: Computed recursively - Tree elements hash includes their sub-Merk's root hash, propagating changes up to the root.

**Paths**: Arrays of byte keys, e.g., `&[b"tree1", b"subtree"]` to navigate the hierarchy.

### Key Files

- `grovedb/src/operations/` - Insert, delete, get, proof operations
- `grovedb/src/query/` - PathQuery and query system
- `grovedb/src/element/` - Element type definitions
- `merk/src/merk/` - Merk tree implementation
- `merk/src/proofs/` - Proof generation and verification
- `storage/src/rocksdb_storage/` - RocksDB backend

### Feature Flags

- `full` (default) - Full functionality with RocksDB
- `verify` - Proof verification only, no storage dependency (for light clients)
- `grovedbg` - Debugger/visualizer support
- `serde` - Serialization support

### Versioning

All operations require a `GroveVersion` parameter: `GroveVersion::latest()`. This enables backward compatibility when element structures change.
