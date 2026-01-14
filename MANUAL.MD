# Scribe User Manual

**Version:** 0.1.0
**Protocol:** Verifiable Data Lineage

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Installation](#2-installation)
3. [Quick Start Guide](#3-quick-start-guide)
4. [Core Concepts](#4-core-concepts)
5. [Command Reference](#5-command-reference)
   - [Global Options](#51-global-options)
   - [scribe init](#52-scribe-init)
   - [scribe commit](#53-scribe-commit)
   - [scribe log](#54-scribe-log)
   - [scribe status](#55-scribe-status)
   - [scribe verify](#56-scribe-verify)
   - [scribe watch](#57-scribe-watch)
6. [Configuration](#6-configuration)
7. [Data Structures](#7-data-structures)
8. [Repository Structure](#8-repository-structure)
9. [Database Schema](#9-database-schema)
10. [PostgreSQL Integration](#10-postgresql-integration)
11. [Workflows](#11-workflows)
12. [Error Handling](#12-error-handling)
13. [Security Considerations](#13-security-considerations)
14. [Troubleshooting](#14-troubleshooting)
15. [Best Practices](#15-best-practices)
16. [Frequently Asked Questions](#16-frequently-asked-questions)
17. [Glossary](#17-glossary)
18. [Appendix](#18-appendix)

---

## 1. Introduction

### 1.1 What is Scribe?

Scribe is a protocol for **Verifiable Data Lineage** that brings Git-like version control to data pipelines. It enables you to track:

- **Who** changed a record (author identification)
- **What** process they used (process tracking)
- **Where** the data came from (provenance)
- **When** changes occurred (timestamps)

### 1.2 Key Features

| Feature | Description |
|---------|-------------|
| **Cryptographic Verification** | SHA-256 hashing ensures tamper-proof history |
| **Merkle-DAG Structure** | Efficient verification of data subsets |
| **Content-Addressable Storage** | Every commit is uniquely identified by its hash |
| **PostgreSQL CDC** | Real-time database change monitoring |
| **Immutable History** | Commits cannot be modified after creation |
| **Audit Trail** | Complete lineage for compliance and debugging |

### 1.3 Use Cases

- **Data Pipeline Auditing**: Track all transformations in ETL processes
- **Compliance Requirements**: Maintain immutable audit trails for regulations (SOX, GDPR, HIPAA)
- **Data Quality Debugging**: Trace the source of data anomalies
- **Change Tracking**: Monitor who changed what and when
- **Forensic Analysis**: Investigate data incidents with cryptographic proof
- **Data Governance**: Establish accountability for data modifications
- **Disaster Recovery**: Know exactly what changed and when for recovery planning

### 1.4 Current Limitations

Understanding Scribe's limitations helps you use it effectively:

| Limitation | Description | Workaround |
|------------|-------------|------------|
| Linear history only | No branching or merging support | Use separate repositories for different workflows |
| Single-process writes | SQLite serializes concurrent writes | Coordinate writes or use application-level queuing |
| No built-in encryption | Database stored in plaintext | Use filesystem encryption or encrypt backups |
| PostgreSQL only for CDC | Watch command only supports PostgreSQL | Use manual commits for other databases |
| Trust-based author IDs | No cryptographic verification of authors | Integrate external signing or use secure environments |
| Local storage only | No distributed/federated repositories | Use file synchronization tools for replication |

### 1.5 Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         SCRIBE CLI                              │
├──────────┬──────────┬──────────┬──────────┬──────────┬─────────-┤
│   init   │  commit  │   log    │  status  │  verify  │  watch   │
└────┬─────┴────┬─────┴────┬─────┴────┬─────┴────┬─────┴────┬────-┘
     │          │          │          │          │          │
     ▼          ▼          ▼          ▼          ▼          ▼
┌─────────────────────────────────────────────────────────────────┐
│                       SCRIBE CORE                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │  Hashing │  │  Merkle  │  │ Envelope │  │   JSON   │        │
│  │ (SHA-256)│  │   Tree   │  │  Builder │  │  Parser  │        │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘        │
└─────────────────────────────────────────────────────────────────┘
                              │
     ┌────────────────────────┼────────────────────────┐
     ▼                        ▼                        ▼
┌──────────┐           ┌──────────┐           ┌──────────┐
│  SQLite  │           │ Objects  │           │PostgreSQL│
│ Database │           │ Storage  │           │   CDC    │
└──────────┘           └──────────┘           └──────────┘
```

---

## 2. Installation

### 2.1 Prerequisites

#### Linux/macOS

| Dependency | Version | Required |
|------------|---------|----------|
| CMake | 3.16+ | Yes |
| C Compiler (gcc/clang) | C11 support | Yes |
| OpenSSL | 1.1+ | Yes |
| SQLite3 | 3.x | Yes |
| PostgreSQL libpq | 9.4+ | Optional |

#### Windows

| Dependency | Version | Required |
|------------|---------|----------|
| Visual Studio | 2019 or 2022 | Yes (with "Desktop development with C++" workload) |
| CMake | 3.16+ | Yes (included with Visual Studio) |
| Git | Any | Yes |
| vcpkg | Latest | Yes (installed automatically by setup script) |

### 2.2 Building from Source (Linux/macOS)

```bash
# Clone the repository
git clone <repository-url>
cd Scribe

# Build using the provided script
./scripts/build.sh

# Or build with specific options:
./scripts/build.sh --debug           # Build with debug symbols
./scripts/build.sh --no-postgres     # Build without PostgreSQL support
./scripts/build.sh --test            # Run tests after build
./scripts/build.sh --clean           # Clean build directory first
```

### 2.3 Manual CMake Build (Linux/macOS)

```bash
mkdir build && cd build
cmake ..
make

# Install (optional)
sudo make install
```

### 2.4 Building on Windows

#### 2.4.1 Prerequisites

| Dependency | Version | Required |
|------------|---------|----------|
| Visual Studio | 2019 or 2022 | Yes (with C++ workload) |
| CMake | 3.16+ | Yes |
| Git | Any | Yes |
| vcpkg | Latest | Installed by setup script |

#### 2.4.2 Using Build Scripts (Recommended)

Open PowerShell and run:

```powershell
# Navigate to project directory
cd C:\path\to\Scribe

# First time: install dependencies (takes ~10-15 minutes)
.\scripts\setup_dev.ps1

# Build Scribe
.\scripts\build.ps1

# Build with options
.\scripts\build.ps1 -Debug           # Build with debug symbols
.\scripts\build.ps1 -NoPostgres      # Build without PostgreSQL support
.\scripts\build.ps1 -Test            # Run tests after build
.\scripts\build.ps1 -Clean           # Clean build directory first
```

The executable will be created at `build-win\bin\Release\scribe.exe`.

#### 2.4.3 Manual CMake Build (Windows)

If you prefer manual setup:

```powershell
# Install vcpkg (if not already installed)
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# Install dependencies
C:\vcpkg\vcpkg install openssl:x64-windows sqlite3:x64-windows libpq:x64-windows
C:\vcpkg\vcpkg integrate install

# Build Scribe
cd C:\path\to\Scribe
mkdir build-win
cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build . --config Release

# The executable is at build-win\bin\Release\scribe.exe
```

#### 2.4.4 Adding to PATH (Windows)

To use `scribe` from any directory:

```powershell
# Temporary (current session only)
$env:PATH = "C:\path\to\Scribe\build-win\bin\Release;$env:PATH"

# Permanent (requires admin PowerShell)
[Environment]::SetEnvironmentVariable("PATH",
    "C:\path\to\Scribe\build-win\bin\Release;" + $env:PATH,
    [EnvironmentVariableTarget]::User)
```

### 2.5 Verifying Installation

```bash
scribe --version
# Output: scribe version 0.1.0
```

---

## 3. Quick Start Guide

### 3.1 Initialize a Repository

```bash
# Create a new Scribe repository
scribe init --author "user:alice" --role "data_engineer"
```

### 3.2 Record Your First Commit

```bash
scribe commit \
  --message "Initial data import" \
  --process "import_script.py" \
  --version "git:v1.0.0" \
  --table "users" \
  --operation "INSERT" \
  --data '{"id":1,"name":"Alice","email":"alice@example.com"}'
```

### 3.3 View History

```bash
# Show recent commits
scribe log

# Show condensed one-line format
scribe log --oneline

# Export as JSON
scribe log --json > history.json
```

### 3.4 Verify Integrity

```bash
scribe verify
```

---

## 4. Core Concepts

### 4.1 Commit Envelope

The **envelope** is the fundamental unit in Scribe. Each envelope contains:

```
┌─────────────────────────────────────────────────────────────┐
│                      COMMIT ENVELOPE                        │
├─────────────────────────────────────────────────────────────┤
│  commit_id   │ SHA-256 hash of this entire envelope         │
│  parent_id   │ Hash of the previous commit (chain link)     │
│  tree_hash   │ Merkle root of all changes                   │
├─────────────────────────────────────────────────────────────┤
│  author      │ id, role, email                              │
│  process     │ name, version, params, source                │
│  timestamp   │ Unix timestamp                               │
│  message     │ Human-readable description                   │
├─────────────────────────────────────────────────────────────┤
│  changes[]   │ Array of individual data changes             │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 Hash Chain

Scribe maintains an immutable chain of commits:

```
┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
│  Root    │ ←── │ Commit 2 │ ←── │ Commit 3 │ ←── │   HEAD   │
│ (parent: │     │ parent:  │     │ parent:  │     │ parent:  │
│  0x000)  │     │  Root    │     │ Commit 2 │     │ Commit 3 │
└──────────┘     └──────────┘     └──────────┘     └──────────┘
```

Each commit references its parent by hash, creating an unbreakable chain. Any modification to a past commit would change its hash, breaking all subsequent links.

### 4.3 Merkle Tree

Changes within a commit are organized in a Merkle tree:

```
                    ┌───────────┐
                    │ Tree Root │  ← Stored in tree_hash
                    └─────┬─────┘
              ┌───────────┴───────────┐
        ┌─────┴─────┐           ┌─────┴─────┐
        │  Hash AB  │           │  Hash CD  │
        └─────┬─────┘           └─────┬─────┘
         ┌────┴────┐             ┌────┴────┐
    ┌────┴────┐ ┌──┴───┐   ┌────┴────┐ ┌──┴───┐
    │ Hash A  │ │Hash B│   │ Hash C  │ │Hash D│
    └────┬────┘ └──┬───┘   └────┬────┘ └──┬───┘
         │         │            │         │
    ┌────┴────┐ ┌──┴───┐   ┌────┴────┐ ┌──┴───┐
    │Change 1 │ │Chng 2│   │Change 3 │ │Chng 4│
    └─────────┘ └──────┘   └─────────┘ └──────┘
```

This structure allows:
- Efficient verification of specific changes
- Proving a change exists without revealing all data
- Detecting tampering at any level

### 4.4 Content-Addressable Storage

Every object in Scribe is identified by its SHA-256 hash:

- **Same content = Same hash** (deterministic)
- **Different content = Different hash** (collision-resistant)
- **Hash reveals nothing about content** (one-way function)

### 4.5 Author Identification

Authors are identified using a structured format:

| Format | Example | Use Case |
|--------|---------|----------|
| `user:<name>` | `user:alice` | Human users |
| `service:<name>` | `service:etl-worker` | Automated services |
| `system:<name>` | `system:scheduler` | System processes |

### 4.6 Process Tracking

Processes are tracked with version information:

| Format | Example | Description |
|--------|---------|-------------|
| `git:<tag>` | `git:v2.1.0` | Git tag version |
| `git:<sha>` | `git:abc123` | Git commit SHA |
| `sha256:<hash>` | `sha256:7f9a3b...` | Content hash |
| `<semver>` | `1.2.3` | Semantic version |

---

## 5. Command Reference

### 5.1 Global Options

These options are available for all commands:

| Option | Short | Description |
|--------|-------|-------------|
| `--verbose` | `-v` | Produce verbose output |
| `--quiet` | `-q` | Suppress non-error output |
| `--path=PATH` | `-C` | Run as if started in PATH |

**Example:**

```bash
# Run scribe in a different directory
scribe -C /path/to/repo status

# Verbose output
scribe -v log
```

---

### 5.2 scribe init

Initialize a new Scribe repository.

**Syntax:**

```
scribe init [PATH] [OPTIONS]
```

**Arguments:**

| Argument | Description |
|----------|-------------|
| `PATH` | Directory to initialize (default: current directory) |

**Options:**

| Option | Short | Description |
|--------|-------|-------------|
| `--author=ID` | `-a` | Set default author ID |
| `--role=ROLE` | `-r` | Set default author role |

**What Gets Created:**

```
.scribe/
├── config.json    # Repository configuration
├── scribe.db      # SQLite database
└── objects/       # Object storage directory
```

**Examples:**

```bash
# Initialize in current directory
scribe init

# Initialize with author information
scribe init --author "user:alice" --role "data_engineer"

# Initialize in a specific path
scribe init /path/to/project --author "service:etl-worker" --role "automated"
```

**Error Conditions:**

| Error | Cause |
|-------|-------|
| `SCRIBE_ERR_REPO_EXISTS` | Repository already exists in target directory |
| `SCRIBE_ERR_IO` | Cannot create directory or files |

---

### 5.3 scribe commit

Create a new commit recording data changes.

**Syntax:**

```
scribe commit [OPTIONS]
```

**Options:**

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--message=MSG` | `-m` | Commit message | (none) |
| `--author=ID` | `-a` | Author ID | From config |
| `--role=ROLE` | `-r` | Author role | From config |
| `--process=NAME` | `-p` | Process/script name | `manual` |
| `--version=VER` | `-V` | Process version | (none) |
| `--table=TABLE` | `-t` | Table name affected | (required) |
| `--operation=OP` | `-o` | Operation type | (required) |
| `--data=JSON` | `-d` | Change data as JSON | (required) |

**Operation Types:**

| Operation | Description |
|-----------|-------------|
| `INSERT` | New record created |
| `UPDATE` | Existing record modified |
| `DELETE` | Record removed |

**Behavior:**

1. Computes SHA-256 hash of the change data
2. Creates a change record with table, operation, and hashes
3. Builds Merkle tree of all changes
4. Creates envelope with author, process, and metadata
5. Computes commit hash of entire envelope
6. Stores commit in database
7. Updates HEAD to point to new commit

**Examples:**

```bash
# Basic commit
scribe commit \
  --message "Fixed user record" \
  --table "users" \
  --operation "UPDATE" \
  --data '{"id":123,"name":"Alice"}'

# Full commit with all metadata
scribe commit \
  --message "Monthly reconciliation batch" \
  --author "user:alice" \
  --role "data_engineer" \
  --process "reconciliation.py" \
  --version "git:v2.1.0" \
  --table "accounts" \
  --operation "UPDATE" \
  --data '{"account_id":5001,"balance":1000.00,"status":"reconciled"}'

# Automated service commit
scribe commit \
  --author "service:etl-worker" \
  --role "automated" \
  --process "daily_import.py" \
  --version "sha256:7f9a3b8c..." \
  --table "transactions" \
  --operation "INSERT" \
  --data '{"tx_id":"TX123","amount":500}'
```

**Output:**

```
[abc123de] Fixed user record
  Author:  user:alice (data_engineer)
  Process: reconciliation.py git:v2.1.0
  Changes: 1 (UPDATE users)
```

**Error Conditions:**

| Error | Cause |
|-------|-------|
| `SCRIBE_ERR_NOT_A_REPO` | Not in a Scribe repository |
| `SCRIBE_ERR_INVALID_ARG` | Missing required options |
| `SCRIBE_ERR_JSON_PARSE` | Invalid JSON in --data |
| `SCRIBE_ERR_DB` | Database write error |

---

### 5.4 scribe log

Display commit history.

**Syntax:**

```
scribe log [COMMIT] [OPTIONS]
```

**Arguments:**

| Argument | Description |
|----------|-------------|
| `COMMIT` | Starting commit hash (default: HEAD) |

**Options:**

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--oneline` | `-1` | One commit per line | Off |
| `--limit=NUM` | `-n` | Max commits to show | 10 |
| `--author=ID` | `-a` | Filter by author ID | (none) |
| `--process=NAME` | `-p` | Filter by process name | (none) |
| `--json` | `-j` | Output as JSON array | Off |

**Output Formats:**

**Full Format (Default):**

```
commit 7f9a3b8c9d2e1f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f
Parent: 3b1c9e8d7c6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a
Author: user:alice <data_engineer> (alice@example.com)
Process: reconciliation.py git:v2.1.0 --force-update
Date:   2024-01-15 14:30:45

    Fixed account balance discrepancy

    Changes (1):
      - UPDATE accounts {"account_id":5001}
```

**Oneline Format:**

```
7f9a3b8c (user:alice) Fixed account balance discrepancy
3b1c9e8d (user:bob) Added new customer records
a2b3c4d5 (service:etl) Daily import completed
```

**JSON Format:**

```json
[
  {
    "commit_id": "sha256:7f9a3b8c...",
    "parent_id": "sha256:3b1c9e8d...",
    "tree_hash": "sha256:abcd1234...",
    "author": {
      "id": "user:alice",
      "role": "data_engineer",
      "email": "alice@example.com"
    },
    "process": {
      "name": "reconciliation.py",
      "version": "git:v2.1.0",
      "params": "--force-update"
    },
    "timestamp": 1705329045,
    "message": "Fixed account balance discrepancy",
    "changes": [
      {
        "table_name": "accounts",
        "operation": "UPDATE",
        "primary_key": "{\"account_id\":5001}",
        "before_hash": "sha256:abc123...",
        "after_hash": "sha256:def456..."
      }
    ]
  }
]
```

**Examples:**

```bash
# Show last 10 commits (default)
scribe log

# Show last 50 commits
scribe log -n 50

# Show commits by specific author
scribe log --author "alice"

# Show commits by process
scribe log --process "reconciliation"

# Show oneline format
scribe log --oneline -n 20

# Export to JSON
scribe log --json > audit_trail.json

# Show history from specific commit
scribe log abc123def456

# Combined filters
scribe log --author "alice" --process "import" -n 100 --json
```

**Filtering Behavior:**

- `--author` performs partial matching (e.g., "alice" matches "user:alice")
- `--process` performs partial matching (e.g., "recon" matches "reconciliation.py")
- Multiple filters are combined with AND logic

---

### 5.5 scribe status

Display repository status and configuration.

**Syntax:**

```
scribe status [OPTIONS]
```

**Options:**

| Option | Short | Description |
|--------|-------|-------------|
| `--porcelain` | `-p` | Machine-readable output |

**Human-Readable Output:**

```
On repository: /path/to/project/.scribe

HEAD: 7f9a3b8c...

Latest commit:
  Author:  user:alice (data_engineer)
  Process: reconciliation.py git:v2.1.0
  Message: Fixed account balance discrepancy
  Changes: 1

Configuration:
  Default author: user:alice (data_engineer)
  PostgreSQL: connected
  Watched tables: users, orders, products
```

**Porcelain Output:**

```
head 7f9a3b8c9d2e1f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f
```

**Information Displayed:**

| Field | Description |
|-------|-------------|
| Repository path | Location of .scribe directory |
| HEAD | Current commit hash |
| Latest commit | Summary of most recent commit |
| Configuration | Default author, PostgreSQL status |
| Watched tables | Tables being monitored (if applicable) |

**Examples:**

```bash
# Human-readable status
scribe status

# Machine-readable for scripting
scribe status --porcelain

# Use in scripts
HEAD=$(scribe status -p | grep "^head" | cut -d' ' -f2)
```

---

### 5.6 scribe verify

Verify repository integrity.

**Syntax:**

```
scribe verify [COMMIT] [OPTIONS]
```

**Arguments:**

| Argument | Description |
|----------|-------------|
| `COMMIT` | Starting commit hash (default: HEAD) |

**Options:**

| Option | Short | Description |
|--------|-------|-------------|
| `--verbose` | `-v` | Show detailed verification output |
| `--full` | `-f` | Verify entire repository (not just reachable commits) |

**Verification Checks:**

1. **Envelope Hash**: Computed hash matches stored commit_id
2. **Parent Existence**: Parent commit exists (except for root)
3. **Chain Integrity**: Parent links form valid chain
4. **Tree Hash**: Merkle root matches stored tree_hash
5. **Change Hashes**: Individual change hashes are valid

**Output:**

```
Verifying repository integrity...

Verifying commit 7f9a3b8c... OK
Verifying commit 3b1c9e8d... OK
Verifying commit a2b3c4d5... OK

Verification successful!
  3 commit(s) verified
  All parent links valid
  All commit hashes match
```

**Verbose Output:**

```
Verifying repository integrity...

Verifying commit 7f9a3b8c9d2e1f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f
  Parent: 3b1c9e8d7c6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a... EXISTS
  Envelope hash... VALID
  Tree hash... VALID
  Changes (1)... VALID

Verifying commit 3b1c9e8d7c6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a
  Parent: a2b3c4d5e6f7890123456789abcdef0123456789abcdef0123456789abcdef01... EXISTS
  Envelope hash... VALID
  Tree hash... VALID
  Changes (3)... VALID

[...]

Verification successful!
```

**Error Output (Corruption Detected):**

```
Verifying repository integrity...

Verifying commit 7f9a3b8c... OK
Verifying commit 3b1c9e8d... FAILED
  ERROR: Envelope hash mismatch
         Expected: 3b1c9e8d7c6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a
         Computed: 9f8e7d6c5b4a3210fedcba9876543210fedcba9876543210fedcba98765432

Verification FAILED!
  1 error(s) detected
  Repository may be corrupted
```

**Examples:**

```bash
# Basic verification from HEAD
scribe verify

# Verbose verification
scribe verify --verbose

# Verify entire repository
scribe verify --full

# Verify from specific commit
scribe verify abc123def456 --verbose
```

---

### 5.7 scribe watch

Monitor PostgreSQL database for changes (CDC - Change Data Capture).

**Syntax:**

```
scribe watch [OPTIONS]
```

**Options:**

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--connection=CONN` | `-c` | PostgreSQL connection string | From config |
| `--tables=TABLES` | `-t` | Comma-separated table list | From config |
| `--mode=MODE` | `-m` | CDC mode: `trigger` or `logical` | `logical` |
| `--interval=MS` | `-i` | Poll interval (trigger mode) | 1000 |
| `--slot=NAME` | `-s` | Replication slot name | `scribe_slot` |
| `--setup` | `-S` | Setup CDC infrastructure only | Off |
| `--cleanup` | `-C` | Cleanup CDC infrastructure | Off |

**Connection String Format:**

```
postgresql://username:password@hostname:port/database
```

**CDC Modes:**

| Mode | Description | Pros | Cons |
|------|-------------|------|------|
| `trigger` | Poll-based using audit triggers | Lower requirements | Higher latency |
| `logical` | Real-time using replication | Zero lag, consistent | Requires WAL config |

**Trigger Mode Details:**

- Creates AFTER INSERT/UPDATE/DELETE triggers on watched tables
- Stores changes in `scribe_audit` table
- Polls for changes at specified interval
- Lower PostgreSQL configuration requirements
- Suitable for non-critical monitoring

**Logical Replication Mode Details:**

- Uses PostgreSQL logical replication
- Requires `wal_level = logical` in postgresql.conf
- Real-time change streaming
- Consistent ordering guaranteed
- Requires PostgreSQL 9.4+
- Suitable for critical data tracking

**Examples:**

```bash
# Setup CDC infrastructure
scribe watch \
  --connection "postgresql://user:pass@localhost:5432/mydb" \
  --tables "users,orders,products" \
  --mode logical \
  --setup

# Monitor with logical replication
scribe watch \
  --connection "postgresql://user:pass@localhost:5432/mydb" \
  --tables "users,orders,products" \
  --mode logical

# Monitor with trigger-based CDC
scribe watch \
  --connection "postgresql://user:pass@localhost:5432/mydb" \
  --tables "users" \
  --mode trigger \
  --interval 5000

# Use custom replication slot
scribe watch \
  --connection "postgresql://user:pass@localhost:5432/mydb" \
  --tables "users" \
  --slot "my_custom_slot"

# Cleanup CDC infrastructure
scribe watch \
  --connection "postgresql://user:pass@localhost:5432/mydb" \
  --cleanup

# Run in background
nohup scribe watch \
  --connection "postgresql://user:pass@localhost:5432/mydb" \
  --tables "users,orders" \
  > scribe_watch.log 2>&1 &
```

**Commit Format for Watched Changes:**

```
commit abc123...
Author: pg_cdc:watch <automated>
Process: pg_txid:123456789
Date: 2024-01-15 14:30:45

    PostgreSQL CDC: UPDATE on users

    Changes (1):
      - UPDATE users {"id":123}
```

**Graceful Shutdown:**

Press `Ctrl+C` to stop watching. Scribe will:
1. Stop listening for changes
2. Process any pending changes
3. Commit final batch
4. Close database connections
5. Exit cleanly

---

## 6. Configuration

### 6.1 Configuration File

**Location:** `.scribe/config.json`

**Format:**

```json
{
  "author_id": "user:alice",
  "author_role": "data_engineer",
  "pg_connection_string": "postgresql://localhost/mydb",
  "watched_tables": ["users", "orders", "products"]
}
```

**Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `author_id` | string | Default author ID for commits |
| `author_role` | string | Default author role |
| `pg_connection_string` | string | PostgreSQL connection (optional) |
| `watched_tables` | array | Tables to monitor (optional) |

### 6.2 Configuration Precedence

Configuration values are resolved in this order (highest to lowest):

1. **Command-line arguments** (always win)
2. **Config file** (`.scribe/config.json`)
3. **Built-in defaults**

**Example:**

```bash
# Config file has author_id: "user:alice"
# This command uses "user:bob" instead
scribe commit --author "user:bob" ...
```

### 6.3 Setting Configuration

Configuration is set during `scribe init`:

```bash
scribe init --author "user:alice" --role "data_engineer"
```

To modify configuration, edit `.scribe/config.json` directly:

```bash
# View current config
cat .scribe/config.json

# Edit config
vim .scribe/config.json
```

---

## 7. Data Structures

### 7.1 Commit Envelope

The commit envelope is the core data structure:

```json
{
  "commit_id": "sha256:7f9a3b8c9d2e1f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f",
  "parent_id": "sha256:3b1c9e8d7c6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a",
  "tree_hash": "sha256:abcd1234efgh5678ijkl9012mnop3456qrst7890uvwx1234yzab5678cdef",
  "author": {
    "id": "user:alice",
    "role": "data_engineer",
    "email": "alice@example.com"
  },
  "process": {
    "name": "monthly_reconciliation.py",
    "version": "git:v2.1.0",
    "params": "--force-update --dry-run=false",
    "source": "https://github.com/myorg/scripts/blob/main/reconciliation.py"
  },
  "timestamp": 1705329045,
  "message": "Fixed account balance discrepancy",
  "changes": [
    {
      "table_name": "accounts",
      "operation": "UPDATE",
      "primary_key": "{\"account_id\":5001}",
      "before_hash": "sha256:abc123...",
      "after_hash": "sha256:def456..."
    }
  ]
}
```

### 7.2 Field Descriptions

**Envelope Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `commit_id` | SHA-256 | Hash of entire envelope |
| `parent_id` | SHA-256 | Hash of parent commit |
| `tree_hash` | SHA-256 | Merkle root of changes |
| `author` | object | Author information |
| `process` | object | Process information |
| `timestamp` | integer | Unix timestamp |
| `message` | string | Commit message |
| `changes` | array | List of changes |

**Author Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Author identifier |
| `role` | string | Author role |
| `email` | string | Email address (optional) |

**Process Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Process/script name |
| `version` | string | Version identifier |
| `params` | string | Command-line parameters |
| `source` | string | Source file URL (optional) |

**Change Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `table_name` | string | Affected table |
| `operation` | enum | INSERT, UPDATE, DELETE |
| `primary_key` | JSON | Row identifier |
| `before_hash` | SHA-256 | Hash before change (null for INSERT) |
| `after_hash` | SHA-256 | Hash after change (null for DELETE) |

### 7.3 Hash Format

All hashes in Scribe use SHA-256:

| Format | Example | Use |
|--------|---------|-----|
| Binary | 32 bytes | Database storage |
| Hexadecimal | 64 characters | JSON, CLI, logs |

**Zero Hash:**

The zero hash (`0x00...00`, 64 zeros) indicates "no parent" for root commits.

### 7.4 Hash Computation

| Hash Type | Formula |
|-----------|---------|
| Leaf hash | `SHA256(0x00 || data)` |
| Node hash | `SHA256(0x01 || left || right)` |
| Envelope hash | `SHA256(serialized_json)` |

---

## 8. Repository Structure

### 8.1 Directory Layout

```
project/
├── .scribe/                    # Scribe repository root
│   ├── config.json             # Repository configuration
│   ├── scribe.db               # SQLite database
│   └── objects/                # Object storage
│       ├── 7f/                 # Objects starting with 7f
│       │   └── 9a3b8c...       # Full hash filename
│       └── ...
└── [your project files]
```

### 8.2 Repository Discovery

Scribe finds the repository by searching up the directory tree:

```
/home/user/project/src/module/  ← Current directory
/home/user/project/src/         ← Not found
/home/user/project/             ← .scribe found!
/home/user/project/.scribe/     ← Repository root
```

Commands work from any subdirectory within the repository.

### 8.3 Multiple Repositories

You can have nested repositories:

```
/parent/.scribe/          # Parent repository
/parent/child/.scribe/    # Child repository (independent)
```

Commands in `/parent/child/` use the child repository.

---

## 9. Database Schema

### 9.1 SQLite Database

**Location:** `.scribe/scribe.db`

### 9.2 Tables

#### objects

Content-addressable object storage.

| Column | Type | Description |
|--------|------|-------------|
| `hash` | TEXT (PK) | SHA-256 hash |
| `type` | TEXT | Object type: blob, tree, commit |
| `content` | BLOB | Serialized data |
| `size` | INTEGER | Size in bytes |
| `created_at` | TEXT | Creation timestamp |

#### commits

Commit metadata for fast queries.

| Column | Type | Description |
|--------|------|-------------|
| `hash` | TEXT (PK) | Commit hash |
| `parent_hash` | TEXT | Parent commit |
| `tree_hash` | TEXT | Merkle root |
| `author_id` | TEXT | Author identifier |
| `author_role` | TEXT | Author role |
| `author_email` | TEXT | Author email |
| `process_name` | TEXT | Process name |
| `process_version` | TEXT | Process version |
| `process_params` | TEXT | Parameters |
| `process_source` | TEXT | Source URL |
| `message` | TEXT | Commit message |
| `timestamp` | INTEGER | Unix timestamp |
| `created_at` | TEXT | DB timestamp |

**Indexes:**
- `idx_commits_parent` - Parent chain traversal
- `idx_commits_author` - Author queries
- `idx_commits_process` - Process queries
- `idx_commits_timestamp` - Time-based queries

#### changes

Individual changes within commits.

| Column | Type | Description |
|--------|------|-------------|
| `id` | INTEGER (PK) | Auto-increment ID |
| `commit_hash` | TEXT | Associated commit |
| `table_name` | TEXT | Source table |
| `operation` | TEXT | INSERT/UPDATE/DELETE |
| `primary_key` | TEXT | Row identifier (JSON) |
| `before_hash` | TEXT | Pre-change hash |
| `after_hash` | TEXT | Post-change hash |
| `created_at` | TEXT | DB timestamp |

**Indexes:**
- `idx_changes_commit` - Per-commit queries
- `idx_changes_table` - Table-specific queries

#### refs

References (HEAD, branches, tags).

| Column | Type | Description |
|--------|------|-------------|
| `name` | TEXT (PK) | Reference name |
| `hash` | TEXT | Commit hash |
| `updated_at` | TEXT | Last update |

#### config

Key-value configuration.

| Column | Type | Description |
|--------|------|-------------|
| `key` | TEXT (PK) | Config key |
| `value` | TEXT | Config value |

---

## 10. PostgreSQL Integration

### 10.1 Overview

Scribe can monitor PostgreSQL databases for changes using two CDC methods:

1. **Trigger-based CDC**: Uses database triggers and polling
2. **Logical Replication**: Uses PostgreSQL's built-in replication

### 10.2 Trigger-Based CDC

**Setup:**

```bash
scribe watch --mode trigger --setup --tables "users,orders"
```

**PostgreSQL Functions Created:**

| Function | Description |
|----------|-------------|
| `scribe_watch_table(name)` | Add monitoring to a table |
| `scribe_unwatch_table(name)` | Remove monitoring from a table |
| `scribe_mark_processed(ids)` | Mark changes as processed |

**Audit Table Structure:**

```sql
CREATE TABLE scribe_audit (
    id SERIAL PRIMARY KEY,
    table_name TEXT NOT NULL,
    operation TEXT NOT NULL,
    old_data JSONB,
    new_data JSONB,
    transaction_id BIGINT,
    timestamp TIMESTAMPTZ DEFAULT NOW(),
    processed BOOLEAN DEFAULT FALSE
);
```

**Manual Setup:**

```sql
-- Watch a table
SELECT scribe_watch_table('users');

-- Unwatch a table
SELECT scribe_unwatch_table('users');

-- View pending changes
SELECT * FROM scribe_pending_changes;
```

### 10.3 Logical Replication

**Prerequisites:**

1. PostgreSQL 9.4 or higher
2. `wal_level = logical` in postgresql.conf
3. Sufficient `max_replication_slots`
4. User with REPLICATION privilege

**postgresql.conf settings:**

```
wal_level = logical
max_replication_slots = 4
max_wal_senders = 4
```

**Setup:**

```bash
scribe watch --mode logical --setup --tables "users,orders"
```

**PostgreSQL Functions Created:**

| Function | Description |
|----------|-------------|
| `scribe_create_publication(tables)` | Create publication for tables |
| `scribe_set_replica_identity_full(tables)` | Set REPLICA IDENTITY FULL |
| `scribe_cleanup_logical()` | Remove replication setup |
| `scribe_check_slot_health()` | Check replication slot status |

**Views Created:**

| View | Description |
|------|-------------|
| `scribe_slot_status` | Replication slot information |
| `scribe_publication_info` | Publication details |

**Manual Setup:**

```sql
-- Create publication
SELECT scribe_create_publication(ARRAY['users', 'orders']);

-- Set replica identity for full row data
SELECT scribe_set_replica_identity_full(ARRAY['users', 'orders']);

-- Check slot health
SELECT * FROM scribe_check_slot_health();

-- View slot status
SELECT * FROM scribe_slot_status;
```

### 10.4 Cleanup

Remove CDC infrastructure:

```bash
scribe watch --cleanup
```

Or manually:

```sql
-- Trigger mode
SELECT scribe_unwatch_table('users');
DROP TABLE IF EXISTS scribe_audit;

-- Logical mode
SELECT scribe_cleanup_logical();
```

### 10.5 Health Monitoring

Monitor replication lag:

```sql
-- Check slot lag
SELECT * FROM scribe_check_slot_health();

-- Returns:
-- slot_name | active | lag_bytes | lag_mb
-- scribe_slot | t     | 1024      | 0.001
```

---

## 11. Workflows

### 11.1 Manual Data Correction

Track manual data fixes:

```bash
# Initialize repository
scribe init --author "user:alice" --role "dba"

# Record the fix
scribe commit \
  --message "Fixed duplicate customer record ID 5001" \
  --process "manual_correction.sql" \
  --table "customers" \
  --operation "UPDATE" \
  --data '{"id":5001,"status":"active","duplicate_of":null}'

# View history
scribe log --oneline

# Verify integrity
scribe verify
```

### 11.2 ETL Pipeline Integration

Track automated pipeline changes:

```bash
#!/bin/bash
# etl_pipeline.sh

# Initialize (once)
scribe init -a "service:etl-worker" -r "automated"

# Get version info
VERSION="git:$(git describe --tags 2>/dev/null || git rev-parse --short HEAD)"

# Process each record
for row in $(get_data); do
  # Transform data
  transformed=$(transform "$row")

  # Record in Scribe
  scribe commit \
    --process "etl_pipeline.sh" \
    --version "$VERSION" \
    --table "transformed_data" \
    --operation "INSERT" \
    --data "$transformed"
done
```

### 11.3 Real-Time PostgreSQL Monitoring

Monitor production database:

```bash
# One-time setup
scribe init --author "service:pg-watcher" --role "cdc"

# Setup logical replication
scribe watch \
  --connection "postgresql://replicator:pass@prod-db:5432/production" \
  --tables "users,orders,payments,refunds" \
  --mode logical \
  --setup

# Run monitor (in production, use systemd or supervisor)
scribe watch \
  --connection "postgresql://replicator:pass@prod-db:5432/production" \
  --tables "users,orders,payments,refunds" \
  --mode logical
```

### 11.4 Audit and Compliance

Generate audit reports:

```bash
# Export all commits as JSON
scribe log --json -n 1000 > full_audit.json

# Export commits by specific user
scribe log --author "alice" --json > alice_audit.json

# Export commits by process
scribe log --process "reconciliation" --json > recon_audit.json

# Verify entire history
scribe verify --full --verbose > verification_report.txt
```

### 11.5 Debugging Data Issues

Trace data lineage:

```bash
# Find all changes to a specific table
scribe log --json | jq '.[] | select(.changes[].table_name == "accounts")'

# Find changes by a specific process
scribe log --process "monthly_recon" -n 100

# Find changes in a time range (via JSON)
scribe log --json -n 1000 | jq '.[] | select(.timestamp >= 1705200000 and .timestamp <= 1705300000)'
```

---

## 12. Error Handling

### 12.1 Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | `SCRIBE_OK` | Success |
| -1 | `SCRIBE_ERR_NOMEM` | Out of memory |
| -2 | `SCRIBE_ERR_INVALID_ARG` | Invalid argument provided |
| -3 | `SCRIBE_ERR_NOT_FOUND` | Resource not found |
| -10 | `SCRIBE_ERR_NOT_A_REPO` | Not in a Scribe repository |
| -11 | `SCRIBE_ERR_REPO_EXISTS` | Repository already exists |
| -12 | `SCRIBE_ERR_REPO_CORRUPT` | Repository is corrupted |
| -20 | `SCRIBE_ERR_IO` | I/O error |
| -21 | `SCRIBE_ERR_DB` | Database error |
| -22 | `SCRIBE_ERR_OBJECT_MISSING` | Object not in storage |
| -30 | `SCRIBE_ERR_HASH_MISMATCH` | Hash verification failed |
| -40 | `SCRIBE_ERR_PG_CONNECT` | PostgreSQL connection failed |
| -41 | `SCRIBE_ERR_PG_QUERY` | PostgreSQL query failed |
| -50 | `SCRIBE_ERR_JSON_PARSE` | JSON parsing failed |

### 12.2 Common Errors and Solutions

**"Not a scribe repository"**

```
Error: Not a scribe repository (or any parent up to /)
```

**Solution:** Initialize a repository with `scribe init` or navigate to a directory containing `.scribe/`.

**"Repository already exists"**

```
Error: Repository already exists at /path/to/.scribe
```

**Solution:** Use the existing repository or remove `.scribe/` directory.

**"Hash mismatch"**

```
Error: Hash mismatch for commit abc123...
       Expected: abc123...
       Computed: def456...
```

**Solution:** Repository may be corrupted. Restore from backup or investigate tampering.

**"PostgreSQL connection failed"**

```
Error: Could not connect to PostgreSQL: connection refused
```

**Solution:** Verify connection string, ensure PostgreSQL is running, check credentials.

**"JSON parse error"**

```
Error: Failed to parse JSON: unexpected token at position 15
```

**Solution:** Verify JSON syntax in `--data` parameter. Use a JSON validator.

### 12.3 Recovery Procedures

**Detecting Corruption:**

```bash
scribe verify --full --verbose
```

**If corruption detected:**

1. Identify the corrupted commit from verify output
2. Check if you have backups of `.scribe/scribe.db`
3. If no backup, you may need to reinitialize and replay changes

---

## 13. Security Considerations

### 13.1 Cryptographic Guarantees

| Property | Guarantee |
|----------|-----------|
| **Tamper Detection** | Any modification changes the hash |
| **Chain Integrity** | Cannot insert/remove commits without detection |
| **Content Verification** | Can verify data matches recorded hash |
| **History Immutability** | Past commits cannot be altered |

### 13.2 What Scribe Does NOT Provide

| Feature | Status | Notes |
|---------|--------|-------|
| Author Authentication | Not built-in | Author ID is self-reported |
| Encryption at Rest | Not built-in | Database is unencrypted |
| Access Control | Not built-in | Filesystem permissions only |
| Digital Signatures | Not built-in | Use external signing |

### 13.3 Recommendations

**For Author Authentication:**

```bash
# Include cryptographic proof in version
scribe commit \
  --version "gpg:$(gpg --sign --armor <<< 'commit data' | base64)" \
  ...
```

**For Data Protection:**

```bash
# Encrypt the database file
sqlite3 .scribe/scribe.db ".backup /tmp/backup.db"
gpg --encrypt --recipient admin@company.com /tmp/backup.db
```

**For Access Control:**

```bash
# Restrict permissions
chmod 700 .scribe/
chmod 600 .scribe/scribe.db
chmod 600 .scribe/config.json
```

### 13.4 Connection String Security

**DO NOT:**

```bash
# Don't put passwords in scripts
scribe watch --connection "postgresql://user:password@host/db"
```

**DO:**

```bash
# Use environment variables
export PGPASSWORD="secure_password"
scribe watch --connection "postgresql://user@host/db"

# Or use .pgpass file
echo "host:5432:db:user:password" >> ~/.pgpass
chmod 600 ~/.pgpass
```

---

## 14. Troubleshooting

### 14.1 Repository Issues

**Problem:** Commands don't work in subdirectory

```bash
cd /path/to/project/deep/nested/dir
scribe log
# Error: Not a scribe repository
```

**Solution:** Ensure `.scribe/` exists in parent directory:

```bash
ls -la /path/to/project/.scribe/
```

**Problem:** Repository seems corrupted

```bash
scribe log
# Error: Database error: malformed
```

**Solution:**

```bash
# Check SQLite integrity
sqlite3 .scribe/scribe.db "PRAGMA integrity_check;"

# If corrupted, restore from backup
cp /backup/scribe.db .scribe/scribe.db
```

### 14.2 Commit Issues

**Problem:** Commit fails with "Invalid JSON"

```bash
scribe commit --data '{"key": value}'
# Error: JSON parse error
```

**Solution:** Validate JSON syntax:

```bash
# Use jq to validate
echo '{"key": "value"}' | jq .

# Escape special characters
scribe commit --data '{"key": "value with \"quotes\""}'
```

**Problem:** Author not set

```bash
scribe commit --table "users" --operation "INSERT" --data '{...}'
# Warning: No author specified
```

**Solution:**

```bash
# Set default in config
scribe init --author "user:name" --role "role"

# Or specify per-commit
scribe commit --author "user:name" --role "role" ...
```

### 14.3 PostgreSQL Watch Issues

**Problem:** Logical replication fails

```bash
scribe watch --mode logical
# Error: logical decoding requires wal_level >= logical
```

**Solution:**

```bash
# Edit postgresql.conf
sudo vim /etc/postgresql/14/main/postgresql.conf
# Set: wal_level = logical

# Restart PostgreSQL
sudo systemctl restart postgresql
```

**Problem:** Replication slot not created

```bash
scribe watch --mode logical --setup
# Error: permission denied for function pg_create_logical_replication_slot
```

**Solution:**

```sql
-- Grant replication privilege
ALTER USER scribe_user REPLICATION;
```

**Problem:** High replication lag

```sql
SELECT * FROM scribe_check_slot_health();
-- Shows high lag_bytes
```

**Solution:**

1. Check if scribe watch is running
2. Increase poll frequency if using trigger mode
3. Check for network issues
4. Consider adding more slots for high-throughput tables

### 14.4 Windows Build Issues

**Problem:** PowerShell script execution is disabled

```powershell
.\scripts\setup_dev.ps1
# Error: cannot be loaded because running scripts is disabled
```

**Solution:**

```powershell
# Run PowerShell as Administrator and enable scripts
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

**Problem:** vcpkg fails to bootstrap

```powershell
.\bootstrap-vcpkg.bat
# Error: Could not find a suitable Visual Studio instance
```

**Solution:** Install Visual Studio 2019 or 2022 with the "Desktop development with C++" workload from the Visual Studio Installer.

**Problem:** CMake can't find dependencies

```powershell
cmake ..
# Error: Could not find OpenSSL / SQLite3 / PostgreSQL
```

**Solution:**

```powershell
# Ensure vcpkg toolchain is specified
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

# Or run the build script which handles this automatically
.\scripts\build.ps1
```

**Problem:** DLL not found when running scribe.exe

```powershell
.\scribe.exe
# Error: The code execution cannot proceed because libpq.dll was not found
```

**Solution:** vcpkg DLLs need to be in PATH or copied to the executable directory:

```powershell
# Add vcpkg installed binaries to PATH
$env:PATH = "C:\vcpkg\installed\x64-windows\bin;$env:PATH"

# Or copy DLLs to executable directory
Copy-Item C:\vcpkg\installed\x64-windows\bin\*.dll .\build-win\bin\Release\
```

**Problem:** WSL cannot connect to Windows PostgreSQL

```bash
scribe watch --connection "postgresql://user:pass@localhost:5432/db"
# Error: connection refused
```

**Solution:** Use the Windows host IP instead of localhost:

```bash
# Get Windows host IP from WSL
WIN_IP=$(cat /etc/resolv.conf | grep nameserver | awk '{print $2}')

# Use that IP in the connection string
scribe watch --connection "postgresql://user:pass@$WIN_IP:5432/db"
```

Also ensure PostgreSQL is configured to accept connections:
1. Edit `pg_hba.conf` to allow the WSL subnet (e.g., `host all all 172.16.0.0/12 md5`)
2. Edit `postgresql.conf` to set `listen_addresses = '*'`
3. Restart PostgreSQL service

### 14.5 Performance Issues

**Problem:** Slow log queries

```bash
scribe log -n 10000
# Takes very long
```

**Solution:**

```bash
# Use filters to reduce results
scribe log --author "alice" -n 100

# Use JSON export for bulk analysis
scribe log --json -n 10000 > log.json
jq '.[] | select(.author.id == "user:alice")' log.json
```

**Problem:** Large database size

```bash
du -h .scribe/scribe.db
# 500MB
```

**Solution:**

```bash
# Vacuum database
sqlite3 .scribe/scribe.db "VACUUM;"

# Check for orphaned objects
# (Future feature: scribe gc)
```

---

## 15. Best Practices

### 15.1 Commit Hygiene

**Write Meaningful Messages:**

```bash
# Good
scribe commit -m "Fixed balance discrepancy in account 5001 due to duplicate transaction"

# Bad
scribe commit -m "fix"
```

**Use Consistent Author IDs:**

Establish a naming convention for your organization:

| Type | Convention | Example |
|------|------------|---------|
| Human users | `user:<username>` | `user:alice`, `user:bob.smith` |
| Services | `service:<service-name>` | `service:etl-worker`, `service:reconciler` |
| Systems | `system:<system-name>` | `system:scheduler`, `system:backup` |

**Include Process Versions:**

Always track the version of the process that made changes:

```bash
# Use git tag
scribe commit --version "git:$(git describe --tags)"

# Use git commit SHA
scribe commit --version "git:$(git rev-parse --short HEAD)"

# Use semantic version
scribe commit --version "2.1.0"
```

### 15.2 Repository Organization

**One Repository per Data Domain:**

```
/data/
├── customers/.scribe/     # Customer data lineage
├── transactions/.scribe/  # Transaction data lineage
└── inventory/.scribe/     # Inventory data lineage
```

**Backup Strategy:**

```bash
# Daily backup of Scribe database
cp .scribe/scribe.db /backups/scribe-$(date +%Y%m%d).db

# Verify backup integrity
sqlite3 /backups/scribe-*.db "PRAGMA integrity_check;"
```

### 15.3 PostgreSQL CDC Best Practices

**Choose the Right Mode:**

| Scenario | Recommended Mode |
|----------|------------------|
| Development/Testing | `trigger` |
| Non-critical production | `trigger` |
| Critical data, low latency required | `logical` |
| High-throughput systems | `logical` |

**Monitor Replication Health:**

```bash
# Create a monitoring script
#!/bin/bash
LAG=$(psql -t -c "SELECT lag_bytes FROM scribe_check_slot_health()")
if [ "$LAG" -gt 1000000 ]; then
    echo "WARNING: Scribe replication lag is high: $LAG bytes"
fi
```

**Graceful Maintenance:**

```bash
# Before database maintenance
scribe watch --cleanup

# After maintenance
scribe watch --setup --tables "users,orders"
scribe watch --tables "users,orders"
```

### 15.4 Performance Optimization

**Batch Related Changes:**

When making multiple related changes, consider using a script to batch commits:

```bash
#!/bin/bash
# Instead of individual commits for each row,
# commit once with summary information
scribe commit \
  --message "Bulk update: 1000 records reconciled" \
  --process "bulk_reconciliation.py" \
  --table "accounts" \
  --operation "UPDATE" \
  --data '{"count":1000,"status":"reconciled"}'
```

**Archive Old Data:**

For very active repositories, periodically export and archive old commits:

```bash
# Export old commits
scribe log --json -n 10000 > archive_2024.json

# Compress
gzip archive_2024.json
```

### 15.5 Integration Patterns

**CI/CD Pipeline Integration:**

```yaml
# .gitlab-ci.yml example
deploy:
  script:
    - ./deploy.sh
    - scribe commit \
        --author "service:gitlab-ci" \
        --role "deployment" \
        --process "deploy.sh" \
        --version "git:$CI_COMMIT_SHA" \
        --table "deployments" \
        --operation "INSERT" \
        --data '{"env":"production","version":"'"$CI_COMMIT_SHA"'"}'
```

**Wrapper Function for Applications:**

```python
# Python wrapper example
import subprocess
import json

def scribe_commit(table, operation, data, message=None, process=None):
    cmd = [
        "scribe", "commit",
        "--table", table,
        "--operation", operation,
        "--data", json.dumps(data)
    ]
    if message:
        cmd.extend(["--message", message])
    if process:
        cmd.extend(["--process", process])

    subprocess.run(cmd, check=True)
```

---

## 16. Frequently Asked Questions

### 16.1 General Questions

**Q: How is Scribe different from Git?**

A: While both use SHA-256 hashes and content-addressable storage:

| Feature | Git | Scribe |
|---------|-----|--------|
| Purpose | Source code versioning | Data lineage tracking |
| Branching | Full support | Linear history only |
| Data model | Files and directories | Tables and records |
| Process tracking | No | Yes (process name, version, params) |
| Database integration | No | PostgreSQL CDC built-in |
| Author verification | GPG signing optional | Trust-on-first-use |

**Q: Can I use Scribe with databases other than PostgreSQL?**

A: The `scribe watch` command currently only supports PostgreSQL. However, you can use `scribe commit` manually to record changes from any database by integrating Scribe into your application code or ETL pipelines.

**Q: What happens if I lose my Scribe database?**

A: The `.scribe/scribe.db` file contains all commit history. If lost without backup, the history is unrecoverable. Always maintain backups of this file.

**Q: Can multiple processes write to the same Scribe repository?**

A: SQLite (used for storage) supports concurrent reads but serializes writes. For high-concurrency scenarios, ensure writes are not happening simultaneously from multiple processes, or use file locking.

### 16.2 Technical Questions

**Q: How large can a Scribe repository get?**

A: There's no hard limit. Repository size depends on:
- Number of commits
- Size of change data stored
- Frequency of commits

For very large repositories (millions of commits), consider archiving old history.

**Q: Can I modify or delete old commits?**

A: No. Scribe's design makes history immutable. Any modification would change the commit hash, breaking the chain integrity. This is a feature, not a limitation—it ensures auditability.

**Q: How do I handle sensitive data?**

A: Scribe stores hashes of data, not the raw data itself (unless you include it in the `--data` parameter). For sensitive data:

1. Only store non-sensitive identifiers in `--data`
2. Store hashes of sensitive values, not the values themselves
3. Use encryption at rest for the `.scribe/scribe.db` file

**Q: What's the maximum size for commit data?**

A: There's no hard limit in Scribe, but SQLite has practical limits. Keep individual commit data under 1MB for best performance.

### 16.3 Operational Questions

**Q: How do I migrate a Scribe repository?**

A: Simply copy the entire `.scribe/` directory:

```bash
cp -r /old/path/.scribe /new/path/.scribe
```

**Q: Can I merge two Scribe repositories?**

A: Currently, there's no built-in merge functionality. Each repository maintains its own independent history.

**Q: How do I export Scribe data for reporting?**

A: Use the JSON output:

```bash
# Export to JSON
scribe log --json -n 10000 > report.json

# Query with jq
jq '.[] | select(.author.id | contains("alice"))' report.json
```

---

## 17. Glossary

| Term | Definition |
|------|------------|
| **Audit Trail** | A chronological record of all changes to data, including who made them and when |
| **CDC (Change Data Capture)** | A technique for identifying and tracking changes made to data in a database |
| **Commit** | A single recorded change or set of changes in Scribe |
| **Content-Addressable Storage** | A storage system where content is retrieved based on its cryptographic hash |
| **DAG (Directed Acyclic Graph)** | A graph structure where edges have direction and no cycles exist |
| **Envelope** | The complete data structure containing a commit's metadata and changes |
| **HEAD** | A reference pointing to the most recent commit in the repository |
| **Hash** | A fixed-size string computed from data using a cryptographic function (SHA-256) |
| **Lineage** | The record of where data came from and how it was transformed |
| **Logical Replication** | PostgreSQL feature that streams changes in real-time using WAL |
| **Merkle Tree** | A tree structure where each node is labeled with the hash of its children |
| **Parent** | The commit that directly precedes another commit in the chain |
| **Primary Key** | A unique identifier for a row in a database table |
| **Process** | The script, program, or action that caused a data change |
| **Provenance** | Information about the origin and history of data |
| **Repository** | The `.scribe/` directory and its contents storing all commit history |
| **Replication Slot** | A PostgreSQL mechanism for tracking WAL consumption by a subscriber |
| **SHA-256** | A cryptographic hash function producing a 256-bit (32-byte) hash |
| **Tree Hash** | The Merkle root hash of all changes in a commit |
| **Trigger** | A database object that automatically executes in response to events |
| **WAL (Write-Ahead Log)** | PostgreSQL's transaction log used for durability and replication |
| **Zero Hash** | A hash of all zeros, used to indicate no parent (root commit) |

---

## 18. Appendix

### 18.1 Complete CLI Reference

```
scribe - Verifiable Data Lineage Protocol

Usage: scribe [OPTIONS] COMMAND [ARGS]

Global Options:
  -v, --verbose       Verbose output
  -q, --quiet         Quiet output
  -C, --path=PATH     Run in PATH

Commands:
  init [PATH]         Initialize repository
    -a, --author=ID     Default author
    -r, --role=ROLE     Default role

  commit              Create commit
    -m, --message=MSG   Commit message
    -a, --author=ID     Author ID
    -r, --role=ROLE     Author role
    -p, --process=NAME  Process name
    -V, --version=VER   Process version
    -t, --table=TABLE   Table name
    -o, --operation=OP  INSERT|UPDATE|DELETE
    -d, --data=JSON     Change data

  log [COMMIT]        Show history
    -1, --oneline       One line per commit
    -n, --limit=NUM     Limit commits
    -a, --author=ID     Filter by author
    -p, --process=NAME  Filter by process
    -j, --json          JSON output

  status              Show status
    -p, --porcelain     Machine-readable

  verify [COMMIT]     Verify integrity
    -v, --verbose       Detailed output
    -f, --full          Full repository

  watch               Monitor PostgreSQL
    -c, --connection=CONN  Connection string
    -t, --tables=TABLES    Table list
    -m, --mode=MODE        trigger|logical
    -i, --interval=MS      Poll interval
    -s, --slot=NAME        Slot name
    -S, --setup            Setup only
    -C, --cleanup          Cleanup only
```

### 18.2 JSON Schema

**Commit Envelope Schema:**

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "required": ["commit_id", "parent_id", "tree_hash", "author", "process", "timestamp", "changes"],
  "properties": {
    "commit_id": {
      "type": "string",
      "pattern": "^sha256:[a-f0-9]{64}$"
    },
    "parent_id": {
      "type": "string",
      "pattern": "^sha256:[a-f0-9]{64}$"
    },
    "tree_hash": {
      "type": "string",
      "pattern": "^sha256:[a-f0-9]{64}$"
    },
    "author": {
      "type": "object",
      "required": ["id", "role"],
      "properties": {
        "id": { "type": "string" },
        "role": { "type": "string" },
        "email": { "type": "string", "format": "email" }
      }
    },
    "process": {
      "type": "object",
      "required": ["name"],
      "properties": {
        "name": { "type": "string" },
        "version": { "type": "string" },
        "params": { "type": "string" },
        "source": { "type": "string", "format": "uri" }
      }
    },
    "timestamp": {
      "type": "integer",
      "minimum": 0
    },
    "message": {
      "type": "string"
    },
    "changes": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["table_name", "operation"],
        "properties": {
          "table_name": { "type": "string" },
          "operation": { "enum": ["INSERT", "UPDATE", "DELETE"] },
          "primary_key": { "type": "string" },
          "before_hash": { "type": ["string", "null"] },
          "after_hash": { "type": ["string", "null"] }
        }
      }
    }
  }
}
```

### 18.3 SQL Quick Reference

**PostgreSQL Trigger Mode:**

```sql
-- Setup
SELECT scribe_watch_table('tablename');

-- Cleanup
SELECT scribe_unwatch_table('tablename');

-- View pending
SELECT * FROM scribe_pending_changes;

-- Mark processed
SELECT scribe_mark_processed(ARRAY[1, 2, 3]);
```

**PostgreSQL Logical Mode:**

```sql
-- Setup
SELECT scribe_create_publication(ARRAY['t1', 't2']);
SELECT scribe_set_replica_identity_full(ARRAY['t1', 't2']);

-- Monitor
SELECT * FROM scribe_slot_status;
SELECT * FROM scribe_check_slot_health();

-- Cleanup
SELECT scribe_cleanup_logical();
```

### 18.4 Environment Setup

#### Linux/macOS

**Recommended Shell Aliases:**

```bash
# Add to ~/.bashrc or ~/.zshrc
alias sl='scribe log --oneline'
alias ss='scribe status'
alias sv='scribe verify'
alias sc='scribe commit'
```

**Git Integration:**

```bash
# Get current git version for process tracking
GIT_VERSION="git:$(git describe --tags 2>/dev/null || git rev-parse --short HEAD)"

scribe commit --version "$GIT_VERSION" ...
```

#### Windows

**PowerShell Aliases:**

```powershell
# Add to $PROFILE (run: notepad $PROFILE)
function sl { scribe log --oneline @args }
function ss { scribe status @args }
function sv { scribe verify @args }
function sc { scribe commit @args }
```

**Git Integration (PowerShell):**

```powershell
# Get current git version for process tracking
$GIT_VERSION = "git:$(git describe --tags 2>$null)"
if (-not $GIT_VERSION) { $GIT_VERSION = "git:$(git rev-parse --short HEAD)" }

scribe commit --version $GIT_VERSION ...
```

**Add to PATH permanently:**

```powershell
# Run as Administrator
$scribePath = "C:\path\to\Scribe\build-win\bin\Release"
$currentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
[Environment]::SetEnvironmentVariable("PATH", "$scribePath;$currentPath", "User")
```

### 18.5 Version History

| Version | Date | Changes |
|---------|------|---------|
| 0.1.0 | 2024 | Initial release |

---

## License

Scribe is open-source software. See LICENSE file for details.

---

## Support

For issues, feature requests, or contributions, please visit the project repository.

---

*This manual was generated for Scribe version 0.1.0*
