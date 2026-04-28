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
scribe version 1.0.1
hash_algorithm blake3-256
pipe_protocol 1
store .scribe (not initialized)
```

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
| `scribe mongo-watch <uri>` | Acquires the writer lock, bootstraps MongoDB state into a baseline commit when needed, resumes or opens a change stream, converts data events to commits, logs ignored DDL events, and persists adapter state only after commit success. |

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
- DDL and sharding events are logged and ignored.
- `invalidate`, `drop`, `rename`, and `dropDatabase` trigger a new bootstrap.

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
