# GroveDB Integration for Dash Core Compact Block Filters

## Executive Summary

This document outlines the integration of GroveDB into Dash Core to provide cryptographic proofs for compact block filters (BIP 157/158). The goal is to enable light clients to request filter data and receive a proof that the data is authentic, verified against a root hash committed in the coinbase transaction.

**Key Design Principles:**
- Keep existing BIP 157 implementation unchanged
- Add GroveDB as a parallel storage system for proof support
- Accept data duplication in exchange for simplicity and safety
- Minimal, focused schema - only store what's needed for proofs

---

## Table of Contents

1. [Background and Motivation](#1-background-and-motivation)
2. [Current Implementation Analysis](#2-current-implementation-analysis)
3. [GroveDB Integration Architecture](#3-grovedb-integration-architecture)
4. [GroveDB Schema Design](#4-grovedb-schema-design)
5. [Protocol Design](#5-protocol-design)
6. [Build System Integration](#6-build-system-integration)
7. [GroveDB FFI Layer Review](#7-grovedb-ffi-layer-review)
8. [Implementation Phases](#8-implementation-phases)
9. [File Locations and References](#9-file-locations-and-references)

---

## 1. Background and Motivation

### What Are Compact Block Filters?

Compact block filters (BIP 157/158) are Golomb-coded sets that allow light clients to determine if a block might contain transactions relevant to them without downloading the full block. They're much smaller than full blocks and enable efficient SPV-style wallet operation.

### Why GroveDB?

Currently, clients must trust the server providing filter data. With GroveDB integration:

1. **Cryptographic Proofs**: Clients can verify filter data against a blockchain-committed root hash
2. **Trustless Verification**: No need to trust the server - proof verification is mathematical
3. **Efficient Range Queries**: Request filters for height ranges with a single proof
4. **Blockchain Commitment**: Root hash in coinbase transaction (cbtx) provides trusted anchor

### The Vision

```
1. Client downloads block headers
2. Client fetches coinbase transaction for tip → contains GroveDB root hash
3. Client requests: "give me filters for blocks 0-20000 with proof"
4. Server returns: filter data + GroveDB Merkle proof
5. Client verifies: proof against cbtx-committed root
   → If valid, filters are cryptographically authenticated
```

---

## 2. Current Implementation Analysis

### Existing Storage Architecture

**Location**: `src/index/blockfilterindex.cpp`

The current BlockFilterIndex uses two storage mechanisms:

#### LevelDB (via CDBWrapper)
Stores metadata indexed by:
- **Height key**: `[DB_BLOCK_HEIGHT, uint32_be]` → `{block_hash, filter_hash, header, FlatFilePos}`
- **Hash key**: `[DB_BLOCK_HASH, uint256]` → `{filter_hash, header, FlatFilePos}` (for reorged blocks)
- **Position key**: `DB_FILTER_POS` → next write position
- **Version key**: `DB_VERSION` → index format version

#### Flat Files (fltr?????.dat)
Stores actual filter data:
- Format: `[block_hash, encoded_filter_bytes]`
- Referenced by FlatFilePos from LevelDB
- Max file size: 16 MiB, chunk size: 1 MiB

### Key Classes

```cpp
// src/blockfilter.h
class GCSFilter {
    // Golomb-coded set implementation
    // Probabilistic set membership testing
};

class BlockFilter {
    BlockFilterType m_filter_type;
    uint256 m_block_hash;
    GCSFilter m_filter;

    uint256 GetHash() const;  // SHA256d of encoded filter
    uint256 ComputeHeader(const uint256& prev_header) const;
};

// src/index/blockfilterindex.h
class BlockFilterIndex : public BaseIndex {
    BlockFilterType m_filter_type;
    std::unique_ptr<BaseIndex::DB> m_db;  // LevelDB
    std::unique_ptr<FlatFileSeq> m_filter_fileseq;  // Flat files

    bool LookupFilter(const CBlockIndex*, BlockFilter&) const;
    bool LookupFilterRange(int start, const CBlockIndex* stop, std::vector<BlockFilter>&) const;
    bool LookupFilterHeader(const CBlockIndex*, uint256&);
};
```

### BIP 157 Protocol Messages

| Message | Purpose | Data Needed |
|---------|---------|-------------|
| `getcfilters` | Request filters | filter_bytes |
| `cfilter` | Single filter response | filter_type, block_hash, filter_bytes |
| `getcfheaders` | Request filter headers | filter_hashes, prev_header |
| `cfheaders` | Headers response | filter_type, stop_hash, prev_header, filter_hashes[] |
| `getcfcheckpt` | Request checkpoints | filter_headers at intervals |
| `cfcheckpt` | Checkpoints response | filter_headers[] at 1000-block intervals |

### What BIP 157 Requires

- **filter_bytes**: The actual encoded filter data
- **filter_hash**: `SHA256d(filter_bytes)` - for integrity verification
- **filter_header chain**: `header[N] = SHA256d(filter_hash[N] || header[N-1])`
- **block_hash**: To identify which block a filter belongs to

---

## 3. GroveDB Integration Architecture

### Design Decision: Parallel Storage

**DO NOT modify existing BlockFilterIndex behavior.**

```
┌─────────────────────────────────────────────────────────────────┐
│                      BlockFilterIndex                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   EXISTING (UNCHANGED)              │    NEW (ADDED)             │
│   ════════════════════              │    ════════════            │
│                                     │                            │
│   ┌──────────────┐  ┌───────────┐   │   ┌──────────────────┐    │
│   │   LevelDB    │  │   Flat    │   │   │     GroveDB      │    │
│   │              │  │   Files   │   │   │                  │    │
│   │ • height idx │  │ fltr*.dat │   │   │ /filters/basic/  │    │
│   │ • hash idx   │  │           │   │   │   [height] →     │    │
│   │ • filter_h   │  │ filter    │   │   │   filter_bytes   │    │
│   │ • headers    │  │ bytes     │   │   │                  │    │
│   │ • positions  │  │           │   │   │ (proofs only)    │    │
│   └──────────────┘  └───────────┘   │   └──────────────────┘    │
│          │                │         │           │                │
│          └────────────────┴─────────┼───────────┘                │
│                                     │                            │
│            WriteBlock() writes to ALL THREE                      │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   BIP 157 requests  →  Existing LevelDB + flat files            │
│   Proof requests    →  GroveDB (new protocol)                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Why This Approach?

1. **Safety**: Existing BIP 157 code is battle-tested, don't risk breaking it
2. **Simplicity**: Add new functionality without complex refactoring
3. **Incremental**: Can develop and test GroveDB path independently
4. **Reversible**: Easy to remove GroveDB if issues arise
5. **Acceptable Trade-off**: Storage duplication is cheap, bugs are expensive

### Data Flow

```
New Block Arrives
       │
       ▼
┌──────────────────┐
│ Build BlockFilter│
└────────┬─────────┘
         │
         ├──────────────────────────────────────┐
         │                                      │
         ▼                                      ▼
┌─────────────────────────┐          ┌─────────────────────────┐
│ Existing Write Path     │          │ New GroveDB Write Path  │
│                         │          │                         │
│ 1. Write to flat file   │          │ 1. Insert filter_bytes  │
│ 2. Write to LevelDB:    │          │    at height key        │
│    - height index       │          │                         │
│    - compute header     │          │ (That's it - minimal)   │
│    - store metadata     │          │                         │
└─────────────────────────┘          └─────────────────────────┘
```

---

## 4. GroveDB Schema Design

### Design Philosophy

Store **only what's needed for proofs**. GroveDB's Merkle proofs provide integrity guarantees, so we don't need:
- `filter_hash` - proof guarantees integrity
- `filter_header` chain - proof replaces this verification mechanism
- `block_hash` - client has headers, knows hash for each height

### Final Schema

```
GroveDB Root
│
└── filters                              # Subtree for all filter types
    │
    └── basic                            # BASIC_FILTER (BIP 158 type 0)
        │
        ├── 0x00000000                   # Height 0 (4-byte big-endian)
        │   └── Item(filter_bytes)       # Raw encoded GCS filter
        │
        ├── 0x00000001                   # Height 1
        │   └── Item(filter_bytes)
        │
        ├── 0x00000002                   # Height 2
        │   └── Item(filter_bytes)
        │
        ├── ...
        │
        ├── 0x000003E8                   # Height 1000
        │   └── Item(filter_bytes)
        │
        └── ...
```

### Key Format

Height is encoded as **4-byte big-endian unsigned integer**:

| Height | Key (hex) |
|--------|-----------|
| 0 | `0x00000000` |
| 1 | `0x00000001` |
| 255 | `0x000000FF` |
| 1000 | `0x000003E8` |
| 65535 | `0x0000FFFF` |
| 1000000 | `0x000F4240` |
| 4294967295 | `0xFFFFFFFF` |

Big-endian ensures lexicographic ordering matches numeric ordering, enabling efficient range queries.

### Value Format

Just the raw filter bytes - the encoded GCS filter as returned by `BlockFilter::GetEncodedFilter()`.

Typical size: 100-1000 bytes per filter (varies by block transaction count).

### Why Not Store More?

| Field | Stored in GroveDB? | Reason |
|-------|-------------------|--------|
| filter_bytes | **YES** | Core data for proofs |
| filter_hash | NO | Computable: `SHA256d(filter_bytes)` |
| filter_header | NO | BIP 157 only, handled by existing code |
| block_hash | NO | Client knows from headers |

### Future Filter Types

The schema supports multiple filter types:

```
/filters/
    /basic/[height] → filter_bytes      # Type 0 (current)
    /extended/[height] → filter_bytes   # Type 1 (future, if defined)
```

---

## 5. Protocol Design

### Commitment Mechanism

The GroveDB root hash will be committed in the **coinbase transaction (cbtx)**:

1. Add new field to cbtx structure
2. Bump cbtx version to indicate new field
3. Masternodes validate the root hash as part of consensus
4. Disagreement on root hash = invalid block

This provides a trustless anchor for proof verification.

### New Protocol Flow

```
┌──────────────────────────────────────────────────────────────────┐
│                    Light Client Protocol                          │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  1. HEADERS SYNC                                                  │
│     ─────────────                                                 │
│     Client downloads block headers (existing protocol)            │
│     → Client now knows block hashes for all heights               │
│                                                                   │
│  2. GET TRUSTED ROOT                                              │
│     ────────────────                                              │
│     Client fetches cbtx for chain tip                             │
│     → Extracts GroveDB root hash from cbtx                        │
│     → This is the trusted anchor for verification                 │
│                                                                   │
│  3. REQUEST FILTERS WITH PROOF                                    │
│     ───────────────────────────                                   │
│     Client: "Give me filters for heights 0-20000 with proof"      │
│     Server: PathQuery on /filters/basic range [0, 20000]          │
│           → Returns filter_bytes for each height                  │
│           → Returns Merkle proof to root                          │
│                                                                   │
│  4. VERIFY PROOF                                                  │
│     ────────────                                                  │
│     Client verifies: proof_root == cbtx_committed_root            │
│     → If valid: filters are cryptographically authenticated       │
│     → If invalid: server is lying or data is stale                │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
```

### Handling Reorgs

```
Scenario: Reorg happens while client is syncing

Before reorg: Root = R1, tip = block A
After reorg:  Root = R2, tip = block B

Client has headers with R1, requests filters, gets proof for R2
→ Proof verification FAILS (R2 ≠ R1)
→ Client detects stale state
→ Client re-syncs headers
→ Client gets new cbtx with R2
→ Client re-requests filters
→ Proof now verifies
```

No special handling needed in GroveDB - the protocol naturally handles staleness.

### Backwards Compatibility

**BIP 157 remains fully supported** via existing LevelDB + flat file storage.

| Protocol | Storage Used | Status |
|----------|--------------|--------|
| BIP 157 (getcfilters, etc.) | Existing LevelDB + flat files | UNCHANGED |
| New proof protocol | GroveDB | NEW |

Clients can use either protocol. Old clients work exactly as before.

### Future P2P Messages (Conceptual)

```
// Request filters with proof
getprovenfilters:
    filter_type: uint8
    start_height: uint32
    end_height: uint32

// Response with filters and proof
provenfilters:
    filter_type: uint8
    start_height: uint32
    end_height: uint32
    filters: []bytes           // filter_bytes for each height
    proof: bytes               // GroveDB Merkle proof

// Or combined tip info message (optimization)
gettipinfo:
    (no params)

tipinfo:
    block_hash: [32]byte
    height: uint32
    cbtx: bytes                // Full coinbase transaction
    merkle_proof: bytes        // Proof that cbtx is in block
    grovedb_root: [32]byte     // Extracted for convenience
```

---

## 6. Build System Integration

### Dash Core Build System Overview

Dash Core uses **GNU Autotools** (configure.ac, Makefile.am) with an optional **depends** system for cross-compilation.

**Key directories:**
- `configure.ac` - Autoconf configuration
- `Makefile.am` - Top-level Makefile template
- `src/Makefile.am` - Source Makefile template
- `depends/` - Cross-compilation dependency system
- `depends/packages/` - Package definitions (.mk files)

**Vendored dependencies pattern:**
- Located directly in `src/` (e.g., `src/leveldb/`, `src/secp256k1/`, `src/dashbls/`)
- Built as part of main build
- No git submodules used

### GroveDB Build Requirements

GroveDB is a Rust project that produces:
- `libgrovedb_ffi.a` - Static library with C FFI
- `grovedb.h` - C header (auto-generated by cbindgen)
- C++17 header-only wrappers in `grovedb-cpp/include/`

**Build command:**
```bash
cd /path/to/grovedb
cargo build --release -p grovedb-ffi
# Outputs: target/release/libgrovedb_ffi.a
# Header: grovedb-ffi/include/grovedb.h
```

### Integration Approach: Vendored in src/

Dash Core does not mandate using the `depends` system. Dependencies that are not vendored must be user-installable via package managers (apt, brew, etc.). Since Rust/GroveDB is not available via standard package managers, **GroveDB must be vendored in `src/`**.

This follows the same pattern as other vendored dependencies:
- `src/leveldb/` - LevelDB database
- `src/secp256k1/` - Elliptic curve library
- `src/dashbls/` - BLS signatures
- `src/grovedb/` - **NEW: GroveDB (to be added)**

### Directory Structure

```
src/
├── grovedb/                      # Vendored GroveDB repository
│   ├── Cargo.toml               # Workspace root
│   ├── Cargo.lock               # Locked dependencies
│   ├── grovedb/                  # Core grovedb crate
│   ├── grovedb-ffi/              # FFI crate
│   │   ├── include/
│   │   │   └── grovedb.h        # C header (auto-generated)
│   │   └── src/
│   ├── grovedb-cpp/              # C++ headers
│   │   └── include/
│   │       └── grovedb/
│   │           ├── grovedb.hpp
│   │           ├── database.hpp
│   │           └── ...
│   ├── merk/                     # Merk tree crate
│   ├── storage/                  # Storage crate
│   └── ...                       # Other workspace crates
├── Makefile.grovedb.include      # Build integration
└── ...
```

### Build Integration

#### src/Makefile.grovedb.include

```makefile
# GroveDB build integration
# Builds the Rust FFI library and provides include/link paths

GROVEDB_DIST_DIR = $(srcdir)/grovedb

# Include paths for C and C++ headers
GROVEDB_CPPFLAGS = \
    -I$(GROVEDB_DIST_DIR)/grovedb-ffi/include \
    -I$(GROVEDB_DIST_DIR)/grovedb-cpp/include

# Static library path
GROVEDB_LIB = $(GROVEDB_DIST_DIR)/target/release/libgrovedb_ffi.a

# Platform-specific link flags
if TARGET_DARWIN
GROVEDB_LDFLAGS = -framework Security -framework CoreFoundation
else
GROVEDB_LDFLAGS = -lpthread -ldl -lm
endif

# Build rule for the FFI library
$(GROVEDB_LIB):
	cd $(GROVEDB_DIST_DIR) && \
	CARGO_TARGET_DIR=$(GROVEDB_DIST_DIR)/target \
	cargo build --release -p grovedb-ffi

# Clean rule
clean-grovedb:
	rm -rf $(GROVEDB_DIST_DIR)/target

# Phony targets
.PHONY: clean-grovedb
```

#### Makefile.am Integration

```makefile
# In src/Makefile.am

include Makefile.grovedb.include

# Add to appropriate targets
libdash_server_a_CPPFLAGS += $(GROVEDB_CPPFLAGS)
libdash_server_a_SOURCES += \
    index/blockfilterindex_grovedb.cpp

# Link the FFI library
dashd_LDADD += $(GROVEDB_LIB) $(GROVEDB_LDFLAGS)

# Ensure GroveDB is built before linking
dashd$(EXEEXT): $(GROVEDB_LIB)
```

#### configure.ac Changes

```autoconf
# Check for Rust toolchain (required for GroveDB)
AC_MSG_CHECKING([for cargo (Rust package manager)])
if test -z "$CARGO"; then
    AC_PATH_PROG([CARGO], [cargo], [])
fi
if test -z "$CARGO"; then
    AC_MSG_RESULT([no])
    AC_MSG_ERROR([cargo is required to build GroveDB. Install Rust from https://rustup.rs/])
else
    AC_MSG_RESULT([$CARGO])
fi

# Verify minimum Rust version (1.70+ recommended for GroveDB)
AC_MSG_CHECKING([Rust version])
RUST_VERSION=$($CARGO --version | cut -d' ' -f2)
AC_MSG_RESULT([$RUST_VERSION])
```

### Build Requirements

**Rust Toolchain:**
- Install via rustup: `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`
- Minimum version: 1.70 (check GroveDB's rust-toolchain.toml)
- Stable channel is sufficient

**Build Command:**
```bash
# Standard Dash build process - GroveDB builds automatically
./autogen.sh
./configure
make -j$(nproc)
```

### Updating Vendored GroveDB

When updating to a new GroveDB version:

```bash
# Remove old vendored code (keep any local patches)
rm -rf src/grovedb

# Copy new version
cp -r /path/to/grovedb src/grovedb

# Remove unnecessary files to reduce size
rm -rf src/grovedb/.git
rm -rf src/grovedb/target
rm -rf src/grovedb/node-grove  # Node.js bindings not needed

# Verify build
make clean-grovedb
make -j$(nproc)
```

### CI/CD Considerations

CI environments need Rust installed:

```yaml
# Example GitHub Actions
- name: Install Rust
  uses: dtolnay/rust-action@stable

- name: Build Dash
  run: |
    ./autogen.sh
    ./configure
    make -j$(nproc)
```

### Linking Requirements

GroveDB FFI links against:
- `pthread` - Threading
- `dl` - Dynamic loading
- `m` - Math library
- Platform-specific: `Security.framework` (macOS), etc.

Add to `configure.ac`:
```autoconf
# Check for GroveDB
AC_ARG_WITH([grovedb],
    [AS_HELP_STRING([--with-grovedb],
        [enable GroveDB support for proven block filters])],
    [use_grovedb=$withval],
    [use_grovedb=auto])

if test "x$use_grovedb" != "xno"; then
    # Check for library and headers
    # Set GROVEDB_LIBS and GROVEDB_CFLAGS
fi
```

---

## 7. GroveDB FFI Layer Review

### Summary

The GroveDB FFI layer has been reviewed and is **production-ready** for Dash Core integration.

### Architecture

```
Rust Core (grovedb)
       │
       ▼
C FFI Layer (grovedb-ffi)
       │
       ▼
C++ Wrappers (grovedb-cpp) ← Use these in Dash Core
       │
       ▼
Dash Core C++ code
```

### Key Patterns

| Pattern | Implementation | Status |
|---------|---------------|--------|
| Handle-based opaque pointers | Global handle table with RwLock | Sound |
| Transaction lifetime safety | OwnedTransaction with Arc | Sound |
| Panic catching at FFI boundary | catch_panic() wrapper | Sound |
| Memory ownership | Clear borrowed vs owned types | Sound |
| Error handling | Comprehensive error codes | Sound |
| RAII in C++ | Proper destructors | Sound |
| Thread safety | DB handle thread-safe, transactions single-threaded | Standard |

### C++ API Usage

```cpp
#include <grovedb/grovedb.hpp>

using namespace grovedb;

// Open database
auto result = Database::open("/path/to/db");
if (!result) {
    LogPrintf("GroveDB error: %s\n", result.error().message());
    return false;
}
auto& db = result.value();

// Create tree structure (one-time setup)
db.insert(Path{}, ByteSpan("filters"), Element::tree());
db.insert(Path{ByteSpan("filters")}, ByteSpan("basic"), Element::tree());

// Insert filter at height
std::array<uint8_t, 4> height_key = HeightToKey(height);
db.insert(
    Path{ByteSpan("filters"), ByteSpan("basic")},
    ByteSpan(height_key),
    Element::item(ByteSpan(filter_bytes))
);

// Generate proof for range
auto query = PathQueryBuilder(Path{ByteSpan("filters"), ByteSpan("basic")})
    .add_range_inclusive(ByteSpan(start_key), ByteSpan(end_key))
    .build();
auto proof_result = db.prove_query(std::move(query));
if (proof_result) {
    std::vector<uint8_t> proof = proof_result.value();
    // Return proof to client
}
```

### Thread Safety Contract

- **Database handle**: Thread-safe, can be shared
- **Transactions**: Single-threaded use only
- For BlockFilterIndex: Database opened once, used from sync thread

---

## 8. Implementation Phases

### Phase 1: Build System Integration

**Goal:** Get GroveDB compiling and linking with Dash Core

**Tasks:**
1. Vendor GroveDB source into `src/grovedb/`
2. Create `src/Makefile.grovedb.include` with build rules
3. Add configure.ac checks for Rust/cargo
4. Integrate into `src/Makefile.am` (include paths, link flags)
5. Verify compilation with GroveDB headers
6. Verify linking with libgrovedb_ffi.a
7. Test on Linux and macOS

**Success Criteria:** `make` succeeds with GroveDB symbols available

### Phase 2: GroveDB Storage Integration

**Goal:** Write filters to GroveDB alongside existing storage

**Tasks:**
1. Add GroveDB database handle to BlockFilterIndex
2. Initialize GroveDB in constructor (create tree structure)
3. Add WriteFilterToGroveDB() helper method
4. Call GroveDB write in WriteBlock() after existing writes
5. Handle GroveDB errors gracefully (log, don't fail)

**Success Criteria:** Filters written to both LevelDB and GroveDB

### Phase 3: Proof Generation

**Goal:** Generate GroveDB proofs for filter ranges

**Tasks:**
1. Add ProveFilterRange(start, stop) method to BlockFilterIndex
2. Implement PathQuery building for height ranges
3. Call grovedb_prove_query()
4. Return serialized proof

**Success Criteria:** Can generate valid proofs for filter ranges

### Phase 4: Testing

**Goal:** Comprehensive test coverage

**Tasks:**
1. Verify all existing BIP 157 tests pass (unchanged)
2. Add unit tests for GroveDB write path
3. Add unit tests for proof generation
4. Add proof verification tests
5. Test reorg handling (both systems updated correctly)
6. Performance benchmarks

**Success Criteria:** All tests pass, acceptable performance

### Phase 5: Consensus Integration (Future)

**Goal:** Commit GroveDB root in coinbase transaction

**Tasks:**
1. Design cbtx field for root hash
2. Bump cbtx version
3. Add root hash computation in block creation
4. Add root hash validation in block validation
5. Update masternode validation

**Success Criteria:** Root hash committed and validated in consensus

### Phase 6: P2P Protocol (Future)

**Goal:** New messages for proof-based filter requests

**Tasks:**
1. Design message formats
2. Implement message serialization
3. Add message handlers
4. Client-side proof verification
5. Integration testing

**Success Criteria:** End-to-end proof-based filter sync working

---

## 9. File Locations and References

### Dash Core Files

**Working Directory:** `/Users/pasta/workspace/dash-grovedb-integration`
**Branch:** `feature/grovedb-integration` (based on `upstream/develop` at `a77ef5701bf`)

| File | Purpose |
|------|---------|
| `/Users/pasta/workspace/dash-grovedb-integration/src/blockfilter.h` | BlockFilter, GCSFilter classes |
| `/Users/pasta/workspace/dash-grovedb-integration/src/blockfilter.cpp` | Filter implementation |
| `/Users/pasta/workspace/dash-grovedb-integration/src/index/blockfilterindex.h` | BlockFilterIndex class |
| `/Users/pasta/workspace/dash-grovedb-integration/src/index/blockfilterindex.cpp` | Index implementation |
| `/Users/pasta/workspace/dash-grovedb-integration/src/index/base.h` | BaseIndex class |
| `/Users/pasta/workspace/dash-grovedb-integration/src/dbwrapper.h` | LevelDB wrapper |
| `/Users/pasta/workspace/dash-grovedb-integration/configure.ac` | Autoconf configuration |
| `/Users/pasta/workspace/dash-grovedb-integration/src/Makefile.am` | Source Makefile |
| `/Users/pasta/workspace/dash-grovedb-integration/depends/packages/` | Depends package definitions |

### GroveDB Files

| File | Purpose |
|------|---------|
| `/Users/pasta/workspace/grovedb/grovedb-ffi/include/grovedb.h` | C FFI header |
| `/Users/pasta/workspace/grovedb/grovedb-cpp/include/grovedb/` | C++ wrappers |
| `/Users/pasta/workspace/grovedb/grovedb-cpp/include/grovedb/database.hpp` | Database class |
| `/Users/pasta/workspace/grovedb/grovedb-cpp/include/grovedb/element.hpp` | Element class |
| `/Users/pasta/workspace/grovedb/grovedb-cpp/include/grovedb/query.hpp` | Query builder |
| `/Users/pasta/workspace/grovedb/grovedb-cpp/include/grovedb/result.hpp` | Result type |
| `/Users/pasta/workspace/grovedb/grovedb/src/lib.rs` | Main GroveDB implementation |

### Documentation

| File | Purpose |
|------|---------|
| `/Users/pasta/workspace/dash/CLAUDE.md` | Dash Core development guide |
| `/Users/pasta/workspace/grovedb/CLAUDE.md` | GroveDB development guide |
| `/Users/pasta/workspace/grovedb/README.md` | GroveDB overview |

---

## Appendix A: Height Key Encoding

```cpp
// Convert height to 4-byte big-endian key
std::array<uint8_t, 4> HeightToKey(uint32_t height) {
    return {
        static_cast<uint8_t>((height >> 24) & 0xFF),
        static_cast<uint8_t>((height >> 16) & 0xFF),
        static_cast<uint8_t>((height >> 8) & 0xFF),
        static_cast<uint8_t>(height & 0xFF)
    };
}

// Convert key back to height
uint32_t KeyToHeight(const std::array<uint8_t, 4>& key) {
    return (static_cast<uint32_t>(key[0]) << 24) |
           (static_cast<uint32_t>(key[1]) << 16) |
           (static_cast<uint32_t>(key[2]) << 8) |
           static_cast<uint32_t>(key[3]);
}
```

## Appendix B: GroveDB Path Construction

```cpp
// Path to filter subtree: /filters/basic
grovedb::Path FilterPath() {
    return grovedb::Path{
        grovedb::ByteSpan(reinterpret_cast<const uint8_t*>("filters"), 7),
        grovedb::ByteSpan(reinterpret_cast<const uint8_t*>("basic"), 5)
    };
}

// Full path to specific filter: /filters/basic/[height]
grovedb::Path FilterPathWithHeight(uint32_t height) {
    auto key = HeightToKey(height);
    return grovedb::Path{
        grovedb::ByteSpan(reinterpret_cast<const uint8_t*>("filters"), 7),
        grovedb::ByteSpan(reinterpret_cast<const uint8_t*>("basic"), 5),
        grovedb::ByteSpan(key.data(), 4)
    };
}
```

## Appendix C: Quick Reference

### GroveDB Operations Needed

| Operation | GroveDB API | When Used |
|-----------|-------------|-----------|
| Open database | `Database::open(path)` | Index initialization |
| Create subtree | `db.insert(path, key, Element::tree())` | One-time setup |
| Insert filter | `db.insert(path, key, Element::item(data))` | WriteBlock |
| Delete filter | `db.delete(path, key)` | Reorg handling |
| Prove range | `db.prove_query(path_query)` | Proof requests |
| Get root hash | `db.root_hash()` | Block creation |

### Error Handling Pattern

```cpp
auto result = db.some_operation();
if (!result) {
    LogPrintf("GroveDB error: %s (code %d)\n",
              result.error().message(),
              static_cast<int>(result.error().code()));
    // For write operations: log and continue (don't fail block validation)
    // For read operations: return appropriate error to caller
}
```

---

*Document created: December 2024*
*For implementation by future development agents*
