# Scribe

Scribe is an append-only content-addressed history store for data-change events.
The v1 command-line tool records document snapshots as blobs, organizes them into
trees, and advances one main ref with commits. The Git analogy is intentional at
the object layer: blobs hold content, trees name blobs and subtrees, commits point
to root trees and parents, and refs point to commits.

Scribe v1 focuses on MongoDB audit history. It watches a MongoDB replica set,
canonicalizes document state, writes deterministic commits, and lets operators
inspect the resulting history locally.

For the complete operator manual, see [docs/MANUAL.md](docs/MANUAL.md). For the
implementation design, see [DESIGN.md](DESIGN.md).

## Disclaimer

This project was entirely "vibe coded"—the architectural vision and design are mine, but the heavy lifting in the
implementation was handled by AI (gpt 5.5 with codex with the help of opus 4.7 for additional reviews). While I built
this to solve a genuine need in my daily workflow and it’s working
beautifully for me, use it at your own risk. I take no responsibility for the implementation details or any unintended "
hallucinations" in the code.

## Status

v1 includes:

- Core `.scribe/` repository creation and object storage.
- BLAKE3 object hashing and zstd-compressed loose objects.
- Commit, tree, blob, ref, diff, log, show, cat-object, and fsck support.
- Pipe protocol v1 through `scribe commit-batch`.
- MongoDB bootstrap and change-stream adapter.
- Clean `SIGTERM`/`SIGINT` shutdown for the MongoDB adapter.
- Docker-based smoke test and GitHub Actions CI.

v1 does not include restore/replay into MongoDB, branches, pack files, garbage
collection, or object pruning.

## Dependencies

Initialize submodules after cloning:

```sh
git submodule update --init --recursive
```

Ubuntu:

```sh
sudo bash scripts/install-deps-ubuntu.sh
```

macOS with Homebrew:

```sh
bash scripts/install-deps-macos.sh
```

The MongoDB adapter needs `libmongoc` and `libbson`. If those libraries are not
available, CMake disables `mongo-watch` and still builds the core commands.

## Build And Test

Build a Release binary with MongoDB adapter support:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSCRIBE_BUILD_MONGO_ADAPTER=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Expected test summary:

```text
100% tests passed, 0 tests failed out of 2
```

The CLI binary is:

```text
build/scribe
```

Verify the binary:

```sh
./build/scribe info
```

Example output:

```text
scribe version 1.1.0
hash_algorithm blake3-256
pipe_protocol 1
store .scribe (not initialized)
```

## Versioned Push

Use the versioned push wrapper when pushing release changes to `main`:

```sh
bash scripts/push-versioned.sh
```

By default it bumps the patch version, updates `CMakeLists.txt`,
`include/scribe/scribe.h`, and versioned documentation examples, commits the
bump, then runs `git push`. Use `minor` or `major` as the first argument for a
larger bump:

```sh
bash scripts/push-versioned.sh minor
```

The script does not create a local tag. GitHub Actions creates `v<version>`
after the pushed commit passes the quality gate.

## Quickstart

Start the local single-node MongoDB replica set:

```sh
docker compose -f docker/docker-compose.yml up -d --wait
```

Create a clean Scribe repository:

```sh
rm -rf /tmp/scribe-quick && mkdir -p /tmp/scribe-quick
./build/scribe init /tmp/scribe-quick/.scribe
```

Start the MongoDB adapter in one terminal. The Docker fixture publishes host port
`27018` to the container's MongoDB port `27017`; use `directConnection=true` so
the MongoDB driver does not switch to the replica set's advertised internal
`localhost:27017` address:

```sh
./build/scribe mongo-watch "mongodb://localhost:27018/?directConnection=true" --store /tmp/scribe-quick/.scribe
```

In another terminal, insert a document:

```sh
docker compose -f docker/docker-compose.yml exec -T mongo mongosh mongodb://localhost:27017/scribe_test?replicaSet=scribe-rs --quiet --eval "db.users.insertOne({_id:'alice',role:'admin'})"
```

The adapter prints a compact commit summary after the commit and resume state are
durable:

```text
commit <7-hex>  insert scribe_test/users/"alice"
```

Stop the adapter with `Ctrl-C` or `SIGTERM`, then inspect history:

```sh
./build/scribe --store /tmp/scribe-quick/.scribe log --oneline
./build/scribe --store /tmp/scribe-quick/.scribe diff HEAD~1 HEAD
./build/scribe --store /tmp/scribe-quick/.scribe fsck
```

Expected diff:

```text
A scribe_test/users/"alice"
```

Shut down MongoDB:

```sh
docker compose -f docker/docker-compose.yml down
```

## Smoke Test

The release smoke test runs the Docker MongoDB fixture, initializes a repository,
starts `mongo-watch`, writes a document, stops the adapter, verifies the log and
diff, and prints `SMOKE TEST: PASSED` only when every step succeeds.

```sh
bash scripts/smoke-test.sh
```

## Command Summary

Most commands accept `--store <path>` before the command. Commands that read objects verify the zstd frame, Scribe object envelope, and BLAKE3 hash as part of the read.

| Command | What It Does |
|---------|--------------|
| `scribe init [path]` | Creates the repository directories plus `HEAD`, `config`, and `log`. If no path is supplied, it creates `.scribe` in the current directory or the global `--store` path. |
| `scribe info` | Opens the store read-only, validates config if present, and prints binary version, hash algorithm, pipe protocol, and selected repository settings. |
| `scribe commit-batch` | Reads pipe protocol v1 frames from stdin, validates identities, paths, timestamps, messages, and payload/tombstone pairs, applies the events to the current tree, writes new objects, and atomically advances `refs/heads/main`. |
| `scribe log [--oneline] [--paths] [-n <N>] [--] [<path>]` | Walks the parent chain from `HEAD`. With `--paths`, it diffs each commit against its parent. With a path argument, it emits only commits where that path's blob or tree hash changed. |
| `scribe show <commit>` | Prints commit metadata and changed paths. `scribe show <commit>:<path>` resolves a path in the commit root tree and writes the raw blob bytes or recursively lists the tree. |
| `scribe ls-tree <hash>` | Recursively lists a tree. Commit hashes resolve to their root tree; blob hashes fail with `SCRIBE_EINVAL`. |
| `scribe diff <commit1> [<commit2>]` | Diffs two commit root trees. With one argument, it compares the commit's parent to the commit. Output is `A`, `M`, or `D` plus a leaf path. |
| `scribe cat-object (-p|-t|-s) <hash>` | Reads one object by full hash and prints the payload, type, or uncompressed payload size. It does not resolve `HEAD` or abbreviated hashes. |
| `scribe list-objects [opts]` | Iterates the object store. By default it includes all loose objects, including dangling ones. With `--reachable`, it first walks from `HEAD` and prints only objects reachable from main history. |
| `scribe fsck` | Walks all objects reachable from `refs/heads/main`, verifies every referenced object read, then scans `objects/` for loose objects not visited by that walk and reports them as dangling warnings. |
| `scribe mongo-watch <uri>` | Acquires the writer lock, bootstraps MongoDB state into a baseline commit when needed, resumes or opens a change stream, converts data events to commits, logs ignored DDL events, persists adapter state only after commit success, and prints concise `commit <short-hash> <operation> <path>` summaries for data commits. |

`mongo-watch` also accepts the smoke-test form:

```sh
./build/scribe mongo-watch "mongodb://localhost:27018/?directConnection=true" --store /tmp/scribe-quick/.scribe
```

## MongoDB Adapter

Use a replica set or sharded cluster. Standalone MongoDB does not provide change
streams. The local fixture in `docker/` starts a single-node replica set named
`scribe-rs`, runs MongoDB on container port `27017`, publishes it on host port
`27018`, creates `scribe_test.users`, and enables pre/post images on that
collection.

If the MongoDB URI omits a database path, Scribe watches the whole cluster and
needs privileges to open a change stream through `admin`. For hosted MongoDB users
with narrower permissions, include the database path before the query string:

```text
mongodb+srv://<credentials>@<cluster>/<database>?authMechanism=MONGODB-AWS&authSource=%24external
```

With a database path present, Scribe bootstraps only that database and opens a
database-scoped change stream. The explicit database path takes precedence over
the excluded-database list.

MongoDB event handling in v1:

- `insert`, `update`, `replace`, and `modify` commit the canonical full document.
- `delete` commits a tombstone for the document path.
- Multi-document transactions become one Scribe commit.
- `create` is logged and ignored because an empty collection has no document path in the v1 tree; later inserts into that collection are captured normally.
- Index, schema, and sharding events are logged and ignored because v1 stores document contents only, not MongoDB catalog metadata.
- `invalidate`, `drop`, `rename`, and `dropDatabase` trigger a new bootstrap. These events can invalidate the stream or change/remove an entire subtree without one replayable per-document event. The new bootstrap commit records the current MongoDB document state; diffs against the previous commit show the removed or moved documents, but the DDL operation itself is not a first-class commit in v1.

Adapter state is stored in:

```text
.scribe/adapter-state/mongodb
```

On startup, `mongo-watch` resumes from the saved token when possible. If MongoDB
rejects the saved token with `cannot resume stream` or `resume token was not found`,
Scribe marks it invalid, logs a warning, writes a new bootstrap commit parented to
existing history, persists the replacement token, and continues watching.

## Repository Layout

`scribe init` creates a `.scribe/` repository with these paths. With no path argument, it initializes `.scribe` in the
current directory; with global `--store <path>`, it initializes that store path.

- `HEAD`: text pointer to `refs/heads/main`.
- `config`: v1 flat configuration file.
- `objects/`: loose compressed objects, split by hash prefix.
- `refs/heads/main`: current commit hash, created after the first commit.
- `adapter-state/mongodb`: MongoDB resume token and last committed state.
- `log`: append-only operational log.
- `lock`: created while a writer owns the repository.

Objects are immutable. Back up `.scribe/` with normal filesystem tools while the
writer is stopped.

## Operations

Signals:

- `SIGTERM` and `SIGINT` request clean shutdown.
- If a batch is in flight, Scribe finishes the commit first.
- Adapter state is persisted only after the commit succeeds.
- A shutdown-time libmongoc stream interruption is treated as clean shutdown after
  the current batch drains.
- The repository lock is released before exit.

Logging:

- Log lines use `<iso8601-utc> <LEVEL> <component> <message>`.
- Set `SCRIBE_LOG_LEVEL=DEBUG` for debug logging.

Locking:

- One writer owns `.scribe/lock`.
- Concurrent writer attempts fail with `SCRIBE_ELOCKED`.

## Troubleshooting

Common failures:

- `SCRIBE_ENOT_FOUND`: wrong `--store` path or repository was not initialized.
- `SCRIBE_ECONFIG`: unknown or invalid key in `.scribe/config`.
- `SCRIBE_ELOCKED`: another writer holds the repository lock.
- `SCRIBE_EHASH`: object hash is not 64 lowercase hex characters.
- `SCRIBE_EPROTOCOL` or `SCRIBE_EMALFORMED`: invalid pipe protocol frame.
- `SCRIBE_EADAPTER`: MongoDB URI, replica set, or topology problem.
- `SCRIBE_ECORRUPT`: repository metadata or object data is malformed.

Run:

```sh
./build/scribe --store <path-to-.scribe> fsck
```

The full manual includes executed examples and recovery notes for each diagnostic:
[docs/MANUAL.md](docs/MANUAL.md).

## CI

GitHub Actions builds Ubuntu 22.04 and 24.04 with GCC and Clang, Release and
Debug, with and without the MongoDB adapter. CI also runs sanitizer jobs, the
Docker smoke test, and a clang-format dry-run.

Local formatting check:

```sh
find src include tests -name '*.[ch]' -print0 | xargs -0 clang-format --dry-run --Werror
```

## Security Review

The following review was performed against the `main` branch on 2026-04-29. It
covers the entire first-party tree (no per-PR scope).

### Scope

Reviewed all first-party source on `main`:

- **CLI**: `src/cli/main.c`
- **Public API**: `include/scribe/scribe.h`
- **Core library** (14 files): `src/core/blob.c`, `commit.c`, `config.c`,
  `context.c`, `diff.c`, `fs.c`, `fsck.c`, `inspect.c`, `internal.h`,
  `library.c`, `object.c`, `pipe.c`, `ref.c`, `tree.c`
- **MongoDB adapter**: `src/adapter_mongo/mongo_bootstrap.c` (2139 lines),
  `mongo_json.c` (596 lines), plus headers
- **Utilities**: `arena.c`, `error.c`, `hex.c`, `leb128.c`, `log.c`, `queue.c`
- **Build/CI**: `CMakeLists.txt`, `.github/workflows/ci.yml`
- **Scripts**: `scripts/push-versioned.sh`, `smoke-test.sh`,
  `install-deps-{ubuntu,macos}.sh`
- **Docker fixtures**: `docker/mongo-init.js`, `docker/docker-compose.yml`
- **Integration tests**: `tests/integration/test_cli_features.sh`

Out of scope: vendored code (`vendor/blake3`, `vendor/zstd`, `vendor/unity`),
markdown documentation, unit tests under `tests/unit/`.

### Findings

**No HIGH or MEDIUM severity findings with confidence ≥ 8.**

### Why no findings emerged

Each category in the methodology was traced to a sink and verified safe:

- **Path traversal**: all filesystem paths are built as
  `scribe_path_join(ctx->repo_path, X)` where `X` is a hardcoded suffix
  (`"objects"`, `"refs/heads"`, `"config"`, etc.) or an entry name read from
  `readdir()` and strictly validated as 2 or 62 lowercase hex characters
  (`src/core/object.c:352, 380`). MongoDB-supplied fields (`ns.db`, `ns.coll`,
  `_id`) become tree-entry names inside binary tree object payloads, never
  filesystem paths. Object paths use `scribe_hash_to_hex(hash)`. Ref names are
  the hardcoded literal `"refs/heads/main"` everywhere.
- **Command injection**: zero `system()`, `popen()`, or `exec*()` call sites in
  any C source (verified by grep).
- **NoSQL injection**: the only Mongo queries are `bson_init(&query)` (empty
  filter) and `mongoc_database_get_collection_names_with_opts(..., NULL, ...)`.
  No query is built by string concatenation.
- **Format-string bugs**: every `printf` / `fprintf` / `vsnprintf` call uses a
  string-literal format. Untrusted strings always pass through `%s` or `%.*s`.
- **Unsafe string functions**: all 5 `strcpy` sites are safe — 4 copy from
  string literals; the 5th (`src/core/config.c:228`) is preceded by an explicit
  `strlen(value) >= sizeof(...)` length check at line 224.
- **Integer overflow**: every `malloc(a + b)` with attacker-influenced inputs
  is preceded by `checked_add_size()` (`blob.c:172`, `inspect.c:179`,
  `mongo_bootstrap.c:574`, `diff.c:464`). The theoretical
  `scribe_arena_strdup_len(..., SIZE_MAX)` wraparound requires a caller already
  holding a SIZE_MAX-sized buffer and is not reachable.
- **LEB128 decoding**: loop bounded `i < 10u` so shift can reach 63 but never
  70 — no UB (`src/util/leb128.c:41`).
- **JSON parsing** (`mongo_json.c`): the output buffer is allocated `raw_len`
  bytes; decoded output never exceeds input size. Malformed strings are
  rejected via explicit checks.
- **Pipe-protocol parsing**: `parse_size_field` accepts wraparound from
  `strtoul` for negative inputs, but every downstream `calloc` / `malloc`
  returns NULL on huge sizes and the error propagates safely.
- **Hardcoded credentials**: `docker/mongo-init.js` and
  `docker/docker-compose.yml` are local-only fixtures bound to
  `127.0.0.1:27018`; not production credentials.
- **GitHub Actions** (`.github/workflows/ci.yml`): triggers are `push` and
  `pull_request` only (not `pull_request_target`), so PR runs use a read-only
  `GITHUB_TOKEN`. The `release` job is gated by
  `github.event_name == 'push' && github.ref == 'refs/heads/main'`. The `$tag`
  variable is derived from a sed pattern restricted to
  `[0-9]+\.[0-9]+\.[0-9]+`, so command injection is not possible.
- **Privilege boundaries**: no setuid/setgid handling in the binary. The CLI
  runs at operator privilege, bounded to what the operator already has.
- **MongoDB adapter trust boundary**: a hostile MongoDB server returning
  malicious change-stream events cannot drive Scribe into filesystem
  traversal, command execution, or memory corruption — all event fields land
  in tree-object payload bytes or canonical-JSON blobs in the
  content-addressed object store.

### Conclusion

The Scribe codebase shows consistent defensive C engineering: explicit
`checked_add_size` overflow guards, hex-validated path components,
content-addressed object paths derived from BLAKE3, libmongoc/libbson for all
Mongo I/O, hardcoded `refs/heads/main`, and zero shell-out call sites. No
exploitable HIGH or MEDIUM vulnerabilities were identified.
