# Scribe

Scribe records verifiable histories of observed information. It stores exact
bytes in BLAKE3-addressed blobs, organizes them into Merkle trees, and records
snapshots as commits. Inspect earlier states, compare snapshots, verify stored
objects, and replicate archives through a server and command-line client.

Scribe observes source systems; it does not intercept writes or replace live
databases. This release ships the server and CLI. Source-specific adapters are
developed separately and are not included.

This README is the project manual and contributor guide. Background references
are [BLAKE3](blake.md) and [RFC 8785](RFC_8785_canonicalization_scheme.md).
The architecture is human-designed with AI-assisted implementation under human
direction. See [LICENSE.MD](LICENSE.MD) for licensing and warranty terms.

## Contents

- [Build and install](#build-and-install)
- [Quickstart](#quickstart)
- [Architecture](#architecture)
- [Client commands](#client-commands)
- [Ingest contract](#ingest-contract)
- [HTTP API](#http-api)
- [Operator commands](#operator-commands)
- [Storage and configuration](#storage-and-configuration)
- [Operations and troubleshooting](#operations-and-troubleshooting)
- [Performance](#performance)
- [Development and tests](#development-and-tests)
- [Versioned releases](#versioned-releases)

## Build and install

Requirements: C11 compiler, CMake 3.20+, Git, pkg-config, OpenSSL development
headers, and Python 3 for integration tests. BLAKE3, zstd, and Unity are pinned
submodules. Linux is the CI platform. On Windows use Ubuntu WSL; prefix commands
with `wsl -d Ubuntu --` when calling them from PowerShell.

```sh
git clone --recurse-submodules https://github.com/WildMix/Scribe.git
cd Scribe
sudo bash scripts/install-deps-ubuntu.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For an existing clone run `git submodule update --init --recursive` first.
On macOS use `bash scripts/install-deps-macos.sh` before the CMake commands.
Build products are `build/scribe`, `build/scribe-cli`, and the implementation
library `build/libscribe.so` on Linux. To install the executables:

```sh
bash scripts/update-bin-from-build.sh --dest-dir /usr/local/bin
```

The helper uses sudo when needed. Restart running servers after upgrading.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `SCRIBE_BUILD_TESTS` | ON | Unit and integration tests |
| `SCRIBE_ENABLE_SECURE_TRANSPORT` | ON | OpenSSL TLS/HTTP2 support |
| `SCRIBE_ENABLE_ASAN` | OFF | Address and undefined-behavior sanitizers |
| `SCRIBE_ENABLE_TSAN` | OFF | Thread sanitizer; mutually exclusive with ASAN |

## Quickstart

From the project directory, initialize a store and start its server:

```sh
mkdir -p /tmp/scribe-demo
./build/scribe init /tmp/scribe-demo/.scribe
./build/scribe --store /tmp/scribe-demo/.scribe
```

In another terminal, also from the project directory:

```sh
printf '%s\n' '{"name":"Alice"}' > /tmp/scribe-demo/alice.json
./build/scribe-cli ingest --path tenant/users/alice \
  --message "first version" --file /tmp/scribe-demo/alice.json
./build/scribe-cli show HEAD:tenant/users/alice
./build/scribe-cli log --oneline
./build/scribe-cli info
```

Ingest returns `{"commit":"<64-hex>"}`. `show` returns the exact file bytes.
Sending identical bytes to the same path returns the current commit without
adding history, even if the message changes. To delete a path:

```sh
./build/scribe-cli ingest --path tenant/users/alice --delete
./build/scribe-cli diff HEAD~1 HEAD
```

Stop the server with Ctrl-C, then verify and pack the store:

```sh
./build/scribe --store /tmp/scribe-demo/.scribe fsck
./build/scribe --store /tmp/scribe-demo/.scribe gc --pack
./build/scribe --store /tmp/scribe-demo/.scribe fsck
```

## Architecture

`scribe` owns repository serving and operator commands. With no command it
serves one store on `127.0.0.1:9323`. `scribe-cli` is the HTTP query/ingest
client; with no command it opens a line-editing REPL. `libscribe` contains the
store implementation. New integrations should submit HTTP requests.

The v2 release separates server/client responsibilities and adds packs,
branches, tags, reflogs, bundles, replication, raw/JSON/pipe HTTP ingest, and
server-managed checkpoints. It fixes receive-buffer growth so larger HTTP
uploads preserve their exact bytes. The software major version does not change
`scribe_format_version = 1` or the `/scribe/v1` API prefix.

The server handles one connection at a time and acquires a writer lock for
ingest/import requests. Competing writers return `SCRIBE_ELOCKED` / HTTP 423.
Maintenance remains in `scribe`, not HTTP endpoints. Named remote configuration
is a remaining local client operation under `.scribe/remotes`. Stop ingestion
before maintenance or backup. There is no staging index, checkout, or merge
operation; branches are history markers and ingest advances `refs/heads/main`.

## Client commands

Examples assume installed binaries and a running server. Global connection
options precede commands:

```sh
scribe-cli --url http://127.0.0.1:9323 log --oneline -n 20
scribe-cli --url https://archive.example.org:9323 info
scribe-cli --url http://127.0.0.1:9323
```

The default URL is `http://127.0.0.1:9323`, requiring no client-side repository.
`--store` selects configuration for named remotes, not local query mode:

```sh
scribe-cli --store /srv/archive/.scribe remote add origin http://127.0.0.1:9323
scribe-cli --store /srv/archive/.scribe remote list
scribe-cli --store /srv/archive/.scribe remote test origin
scribe-cli --store /srv/archive/.scribe --remote origin log --oneline
scribe-cli --store /srv/archive/.scribe remote remove origin
```

`remote add` accepts `--token`; direct URL mode accepts global `--token`.
Tokens are transmitted as bearer headers, but do not enable authorization in
the server itself.

| Command | Result |
| --- | --- |
| `info` | Version, settings, capabilities, main ref and last fsck health |
| `refs` | Ref names and full commit hashes |
| `log [--oneline] [--paths] [-n N] [--since T] [--until T] [--] [path]` | History scoped by path and inclusive timestamp bounds |
| `show <commit>` | Commit metadata and changed paths |
| `show <commit>:<path>` | Exact blob bytes or tree contents |
| `show <path>` | History affecting a path |
| `diff <commit1> [<commit2>]` | A/M/D path changes; one argument compares with its parent |
| `cat-object (-p|-t|-s) <hash-or-revision>` | Payload, type, or uncompressed payload size |
| `ls-tree <hash-or-revision>` | Recursive, byte-sorted tree entries |
| `ingest --path P [--message M] --file F` | Raw upload; `--file -` reads stdin |
| `ingest --path P [--message M] --delete` | Tombstone |
| `ingest [--json] [file]` | JSON/base64 batch; omitted file or `-` reads stdin |
| `ingest --pipe [file]` | Pipe frames transported over HTTP |

Revisions include `HEAD`, `HEAD~N`, full hashes, refs, branches, and tags.
Use full hashes in scripts. Time filters accept UTC ISO-8601 or Unix nanoseconds.
Quote paths containing shell metacharacters. In the REPL use `help` and `quit`.

## Ingest contract

### Raw content

Send a file's exact bytes as the body. Binary HTTP bodies need no base64;
base64 is useful only when embedding binary data in JSON. Repeat `path` once
per component and percent-encode query values:

```sh
curl --request PUT --header 'Content-Type: application/json' \
  --data-binary @dev.json \
  'http://127.0.0.1:9323/scribe/v1/content?path=du-build-tasks&path=dev&message=second%20version'
scribe-cli ingest --path du-build-tasks/dev --message "second version" --file dev.json
```

`path` is required and `message` is optional. A zero-byte PUT stores an empty
blob. DELETE requires an empty body and removes the path:

```sh
curl --request DELETE \
  'http://127.0.0.1:9323/scribe/v1/content?path=du-build-tasks&path=dev'
```

Raw and JSON ingest have a 16 MiB request-body limit and return a JSON commit
hash on success. Raw payloads are opaque regardless of Content-Type.

### JSON batches and checkpoints

A batch applies its events to one snapshot. No envelope file is required:

```sh
printf '%s\n' '{"message":"snapshot","events":[{"path":["tenant","key"],"op":"put","payload":"aGVsbG8="},{"path":["tenant","old"],"op":"delete"}]}' |
  scribe-cli ingest
```

The payload is base64 for `hello`. A put requires a base64 string; an empty
string stores an empty blob. A delete omits payload or sets it to null. New
clients should send explicit `op: put/delete`; implicit legacy input remains
readable. Metadata may include `message`, `timestamp_unix_nanos`, `author`,
`committer`, and `process`. Identity objects use `name`, `email`, `source`;
process fields are `name`, `version`, `params`, `correlation_id`.

Paths are non-empty arrays of non-empty components without control characters.
Use stable paths and deterministic payload serialization: logically equal
data with different bytes produces different hashes. Canonicalization belongs
to the producer; Scribe does not interpret blob contents.

JSON batches may include:

```json
{
  "events": [{"path":["source","key"],"op":"put","payload":"aGVsbG8="}],
  "checkpoint": {"adapter":"example","key":"source-1","value":"Y3Vyc29yLTE="}
}
```

The server commits first, then persists the checkpoint with that commit hash.
`GET /scribe/v1/checkpoints/example/source-1` returns `commit` and base64 `value`;
missing state returns 404. A crash between commit and checkpoint can replay a
batch. Unchanged content is idempotent; this is not an exactly-once transaction
across the source and Scribe. Advance source cursors only after acknowledgement.

### Pipe v2 over HTTP

`POST /scribe/v1/ingest-stream` accepts pipe v2 with Content-Type
`application/x-scribe-pipe-v2`. The server currently buffers requests, with a
128 MiB limit. Multiple batches in one request commit separately.

Frames use tab-separated headers, newline-separated path components, and
length-delimited message/payload bytes. Lengths include payload newlines;
there is no implicit separator after a payload. This example writes `hello`:

```sh
{
  printf 'BATCH\t2\t1\nAUTHOR\texample\t\tadapter\n'
  printf 'COMMITTER\tscribe\t\tscribe\nPROCESS\texample\t1\t\t\n'
  printf 'TIMESTAMP\t1700000000000000000\nMESSAGE\t0\n'
  printf 'EVENT\tput\t2\t5\ntenant\nkey\nhelloEND\n'
} | scribe-cli ingest --pipe
```

Delete frames use `EVENT\tdelete\t<path-count>\n` then path components; puts
use `EVENT\tput\t<path-count>\t<payload-length>\n`. Responses are
`OK\t<commit-hash>` or ERR frames. Pipe v1 remains readable; v2 explicitly
distinguishes deletes and empty blobs. A lost response leaves an unknown
outcome: inspect/retry, because disconnecting does not undo a commit.

## HTTP API

Prefix every endpoint below with `/scribe/v1`. Query text generally uses
NDJSON line events decoded by `scribe-cli`; blob reads return raw bytes.

| Method and endpoint | Purpose |
| --- | --- |
| `GET /info` | Version, settings, capabilities, health |
| `GET /refs` | Ref inventory |
| `GET /log?...` | Formatted history |
| `GET /show/<percent-encoded-spec>` | Commit/path inspection |
| `GET /diff/<commit1>[/<commit2>]` | Snapshot comparison |
| `GET /cat-object/<revision>?mode=t`, `s`, or `p` | Object inspection |
| `GET /ls-tree/<revision>` | Recursive tree listing |
| `PUT /content?path=...` | Raw single-path ingest |
| `DELETE /content?path=...` | Tombstone |
| `POST /ingest` | JSON/base64 batch and optional checkpoint |
| `POST /ingest-stream` | Binary pipe batches |
| `GET /checkpoints/<adapter>/<key>` | Resume state |
| `POST /negotiate-pull` | Missing-object bundle for replication |
| `GET /upload-pack` | Full bundle download |
| `POST /negotiate-push` | Existing-object inventory |
| `POST /receive-pack` | Verified bundle import with fast-forward ref checks |

`POST /update-ref` is reserved (`SCRIBE_ENOSYS`). Maintenance has no general
HTTP equivalent. `/scribe/v1/status` is served by the separate daemon supervisor.
Common errors: 400 invalid input, 404 absent object/checkpoint, 409 stale ref,
413 oversized ingest, 423 writer contention. Query errors include a Scribe
code and detail. HTTP/1 uploads require Content-Length; chunked uploads are
not supported.

TLS server mode is `scribe --store <path> --cert <cert.pem> --key <key.pem>`.
Secure transport uses ALPN `h2`; plain transport is HTTP/1.1. Clients verify
certificates with system trust or `SCRIBE_TLS_CA_FILE=/path/to/ca.pem`.
The server does not enforce bearer authentication: use trusted networks and
an authenticated gateway before exposing it outside a trusted environment.

## Operator commands

Use `scribe --store /srv/archive/.scribe <command>`. The store path names
the `.scribe` directory itself.

| Command | Behavior |
| --- | --- |
| `init [path]` | Initialize, defaulting to `.scribe` |
| `status` | Inventory, latest commit, storage counts, fsck status |
| `fsck` | Verify hashes, packs, trees, commits, refs; report dangling objects |
| `gc [--dry-run] [--prune-now] [--pack] [--consolidate]` | Prune dangling loose objects, pack reachable objects, consolidate packs |
| `branch <name> [commit]` | Create/move a marker, defaulting to HEAD |
| `tag [--force] <name> <commit>` | Create a tag; replacement requires force |
| `tag -d <name> [--force]` | Remove a tag |
| `reflog <ref>` | Local ref movements; accepts HEAD and shorthand |
| `list-objects [--reachable] [--type=TYPE] [--format=FORMAT]` | Inventory; types blob/tree/commit, fields `%H`, `%T`, `%S`, `%C` |
| `bundle create <file>` | Checksummed objects/packs/refs archive |
| `bundle import <file> <target-store>` | Verify/import, initialize destination if needed |
| `replicate <remote> [--interval-ms N] [--max-cycles N]` | Pull missing objects, advance refs fast-forward-only |
| `daemon --config <file>` | Repository status supervisor |

Reflogs contain UTC time, old hash, new hash, and reason; zero hashes represent
creation/deletion. Bundles and replication omit reflogs, config, checkpoints,
locks, and operational logs. Back up the complete store to preserve those.

For a replica, initialize its store, configure a source using
`scribe-cli --store <archive> remote add source <url>`, then run
`scribe --store <archive> replicate source`. Divergent history is rejected.
Daemon config uses tab-delimited `listen <address>` and `repo <name> <store>`
records; see the executable fixture in `tests/integration/test_cli_features.sh`.
Legacy daemon watch records require an external adapter not shipped here.

## Storage and configuration

```text
.scribe/
  HEAD                       symbolic ref to refs/heads/main
  config                     flat key=value settings
  objects/<xx>/<hash-rest>    zstd-compressed loose objects
  packs/<pack-id>.pack        compressed object records
  packs/<pack-id>.idx         hash/record metadata index
  refs/heads/*               main and branch markers
  refs/tags/*                tag markers
  logs/refs/*                local reflogs
  remotes/*                  named remote configuration
  adapter-state/*            checkpoints
  lock                       advisory writer lock and diagnostics
  log                        operator logs
  scribe_server.log          server logs
```

The object envelope is type byte, unsigned LEB128 payload length, then payload.
BLAKE3-256 hashes this uncompressed envelope, so compression changes do not
alter identity. Trees have byte-sorted entries containing type, raw 32-byte
hash, LEB128 name length, and UTF-8 name. Commits contain a root tree, optional
single parent, tab-separated identity/process fields and timestamps, a blank
line, then message bytes. Blobs are opaque and packs do not use delta chains.

Commit construction applies changes to the previous tree, writes objects, then
publishes the main ref by compare-and-swap. Equal roots return the existing
commit. A crash before ref publication may leave dangling objects. Reflogs are
operational history, not a backup or a transactional recovery journal.

Configuration is strict: unsupported values and unknown keys fail startup.
`scribe init` writes all defaults. Selected settings:

| Key | Default | Meaning |
| --- | --- | --- |
| `scribe_format_version` | 1 | Store format independent of software version |
| `hash_algorithm` | blake3-256 | Object identity |
| `compression`, `compression_level` | zstd, 3 | Compression |
| `commit.storage` | auto | loose, pack, or thresholds |
| `commit.pack_event_threshold` | 1000 | Event threshold for packed commits |
| `commit.pack_payload_threshold` | 16777216 | Byte threshold for packed commits |
| `commit.loose_fsync` | commit | commit, per_object, or none |
| `pack.target_size`, `pack.target_object_count` | 1073741824, 1000000 | Pack targets |
| `pack.dictionary_enabled`, `pack.dictionary_sample` | true, 1024 | Per-pack dictionaries |
| `pack.rollup_loose_threshold`, `pack.rollup_size_threshold` | 100000, 1073741824 | Rollup thresholds |
| `gc.prune_grace_days` | 14 | Dangling-object grace period |
| `worker_threads`, `event_queue_capacity` | 0, 64 | Worker/queue configuration |
| `queue_stall_warn_seconds` | 30 | Backpressure warning interval |

Legacy `adapter.mongodb.*` settings remain accepted for store compatibility;
they do not install an adapter. Do not choose `commit.loose_fsync=none` when
durable writes are required. Durability depends on filesystem locks, rename,
and sync semantics. Hash verification detects changed bytes but cannot prove
that a producer observed correct source data or protect a trusted root from
replacement by someone controlling the entire archive.

## Operations and troubleshooting

Run the server as a dedicated user with an absolute store path. For systemd,
use `ExecStart=/usr/local/bin/scribe --store /srv/archive/.scribe`,
`Restart=on-failure`, and a user owning the initialized store. Keep the default
private listen address unless deployment controls provide access protection.
Use `scribe-cli info` for readiness.

The startup banner reports version, store, address, transport, capabilities,
and log path. Valid requests receive a flushed `request started` line before
dispatch and a completion line with status, duration, and byte counts. These
go to stderr and `.scribe/scribe_server.log`; incomplete uploads may fail
before dispatch. `SCRIBE_LOG_LEVEL=DEBUG` adds diagnostics; `--log-format=json`
or `SCRIBE_LOG_FORMAT=json` enables structured output.

Logs are append-only. Stop the process before rotating its log. Keep secrets
out of messages/query parameters, logs, and version control. For backup, stop
writers and copy `.scribe` completely; verify restored/imported stores with
fsck. Preview pruning with `gc --dry-run`; `--prune-now` bypasses the grace
period. Preserve needed history through refs before reclaiming objects.

| Symptom/code | Action |
| --- | --- |
| Connection failed | Start server; check URL, listen address, and startup log |
| `SCRIBE_ENOT_FOUND` | Check revision/path and store initialization |
| `SCRIBE_ELOCKED` | Wait for or stop active writer; deleting its file does not release the advisory lock |
| `SCRIBE_EREF_STALE` | Inspect divergent refs before replication retry |
| `SCRIBE_ECORRUPT`, `SCRIBE_EHASH` | Preserve evidence, fsck, recover verified backup |
| `SCRIBE_ECONFIG` | Compare settings against a newly initialized config |
| `SCRIBE_EPATH`, `SCRIBE_EINVAL`, `SCRIBE_EMALFORMED`, `SCRIBE_EPROTOCOL` | Check arguments, path components, operations, and byte lengths |
| `SCRIBE_EIO` | Check disk space, permissions, network, filesystem support |
| `SCRIBE_EEXISTS` | Inspect existing target/ref before forcing replacement |
| `SCRIBE_EADAPTER` | Inspect remote/source error detail |
| `SCRIBE_ENOMEM` | Reduce batch sizes and check resource limits |
| `SCRIBE_ENOSYS` | Feature is unavailable or disabled in this build |

Interrupting ingest is not rollback. Verify exact bytes with
`scribe-cli show HEAD:du-build-tasks/dev | cmp - dev.json`. Older builds with
the HTTP buffer-growth bug could commit incorrect large payloads. Upgrade and
re-ingest original files to repair current state; history is not rewritten.

## Performance

Pack reads use indexed ranged record reads; inventory uses index metadata
without inflating every blob. Packs reduce filesystem block overhead for small
objects. Apparent bytes and allocated disk space are different measurements.
Compare performance with identical hardware, payloads, and history shapes.

The current commit builder traverses existing trees; it is not guaranteed
O(depth) for one changed leaf. History/time filtering scans commits, path
history reads trees, and fsck/gc traverse objects. Diff skips equal subtree
hashes. Wide-tree lookup/sorting and blob bytes can dominate writes. Remaining
opportunities include incremental tree editing, persistent index caches,
history indexes, and bounded streaming responses.

Measure fresh temporary stores using raw or pipe HTTP ingest, then compare
query times before and after `gc --pack`. Record payload sizes, object counts,
history depth, filesystem, version, wall time, and allocated disk size. The old
benchmark harness uses retired local commands and is excluded from this release;
historical measurements are not guarantees for the current HTTP workflow.

## Development and tests

`include/scribe` defines the API; `src/core` implements storage, protocols, and
inspection; `src/cli` implements executable dispatch; `src/util` supplies
allocation, logging, encoding, and queue helpers. New integrations use HTTP.
Errors use `scribe_error_t` and thread-local detail. Keep object formats and
ABI changes deliberate. Use bounded parsing, checked size arithmetic, and
explicit ownership; arena-backed data must not outlive its arena.

Style is C11, four spaces, LLVM-based clang-format, 120 columns, unsorted
includes, and warnings as errors. Build and test with:

```sh
find src include tests -name '*.[ch]' -print0 | xargs -0 clang-format --dry-run --Werror
cmake --build build -j
ctest --test-dir build --output-on-failure
bash scripts/smoke-test.sh
```

Unity unit tests cover encoding, hashing, object/tree/commit semantics,
empty/deleted/unchanged data, packs, fsck and gc. CLI integration covers HTTP
and TLS/HTTP2, REPL, raw/JSON/pipe ingest, large byte-exact uploads, request logs,
replication, refs, bundles, and maintenance. The smoke test exercises a complete
init/server/ingest/query/restart/maintenance workflow without external adapters.

```sh
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSCRIBE_ENABLE_ASAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
CC=clang cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSCRIBE_ENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

CI checks versions, GCC/Clang Release/Debug builds on Ubuntu 22.04/24.04,
sanitizers, smoke, and formatting. CodeQL analyzes C/C++ and Actions separately.
All configured quality-gate jobs must succeed before release. Commit subjects
should be short and scoped; include test evidence with behavior changes.

## Versioned releases

Current release: scribe version 2.0.0.

Commit reviewed implementation and docs first. The wrapper requires clean
version-managed files and no unrelated staged changes:

```sh
bash scripts/push-versioned.sh major --dry-run
bash scripts/push-versioned.sh major
```

`major` resets minor/patch to zero; default bump is `patch`. The script updates
CMake, the public version header, and this README, commits, then pushes.
`--no-push` creates a local version commit for final tests; push after checking
it. `-- <git-push-args>` selects the destination.

GitHub creates `v<version>` and a release after main passes the quality gate.
Do not manually move tags to bypass CI. Reusing a version tag that points to
another commit is rejected. Adapter sources and development fixtures are not
part of this server/CLI release.
