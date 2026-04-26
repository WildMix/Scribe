# Scribe Manual

Placeholders used in command output:

- `<64-hex>` is a 64-character lowercase BLAKE3 object or commit hash.
- `<12-hex>` is the 12-character abbreviation printed by `scribe log --oneline`.
- `<unix-nanos>` is a Unix timestamp in nanoseconds.
- `<iso8601>` is a UTC timestamp in `YYYY-MM-DDTHH:MM:SSZ` form.

Unless a block is explicitly marked "Non-executed example", every shell command shown here was run during the v1 release validation on WSL Ubuntu with the v1 source tree.

## Overview

Scribe is an append-only content-addressed history store for data-change events. The v1 command-line tool records document snapshots as blobs, organizes them into trees, and advances one main ref with commits. The Git analogy is precise at the object layer: blobs hold content, trees name blobs and subtrees, commits point to root trees and parents, and refs point to commits. Scribe is not Git for files, does not restore data, does not pack objects, and does not implement branches in v1.

The value of Scribe v1 is a deterministic audit trail for MongoDB document state, backed by local immutable objects that can be inspected with simple commands. For implementation details, read `DESIGN.md`; this manual covers installation, operation, and diagnosis.

## Installation

### Ubuntu

Non-executed example:

```sh
sudo scripts/install-deps-ubuntu.sh
```

The script installs `build-essential`, `cmake`, `pkg-config`, `git`, `libmongoc-dev`, and `libbson-dev` using `apt-get`.

Build from the source root:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSCRIBE_BUILD_MONGO_ADAPTER=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Observed test output:

```text
100% tests passed, 0 tests failed out of 1
```

### Debian

Non-executed example:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config git libmongoc-dev libbson-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSCRIBE_BUILD_MONGO_ADAPTER=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Use the Ubuntu script as a reference; Debian package names are the same on current stable releases.

### macOS

Non-executed example:

```sh
scripts/install-deps-macos.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSCRIBE_BUILD_MONGO_ADAPTER=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The macOS script requires Homebrew and installs `cmake`, `pkg-config`, and `mongo-c-driver`.

### Build From Source On Other Systems

Non-executed example:

```sh
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If `libmongoc` and `libbson` are absent, CMake disables `mongo-watch`. Core commands still build.

### Verifying The Installation

```sh
./build/scribe info
```

Output:

```text
scribe version 1.0.0
hash_algorithm blake3-256
pipe_protocol 1
store .scribe (not initialized)
```

## Quickstart

Start the local MongoDB replica set:

```sh
docker compose -f docker/docker-compose.yml up -d --wait
```

Output:

```text
Network docker_default  Creating
Network docker_default  Created
Container docker-mongo-1  Creating
Container docker-mongo-1  Created
Container docker-mongo-1  Starting
Container docker-mongo-1  Started
Container docker-mongo-1  Waiting
Container docker-mongo-1  Healthy
```

Create a clean Scribe repository:

```sh
rm -rf /tmp/scribe-manual-quick && mkdir -p /tmp/scribe-manual-quick
./build/scribe init /tmp/scribe-manual-quick/.scribe
```

Output:

```text
initialized /tmp/scribe-manual-quick/.scribe
```

Start the MongoDB adapter in one terminal. The Docker fixture publishes host port `27018` to the container's MongoDB port `27017`; use `directConnection=true` because the single-node replica set advertises its internal `localhost:27017` member address:

```sh
./build/scribe mongo-watch "mongodb://localhost:27018/?directConnection=true" --store /tmp/scribe-manual-quick/.scribe
```

Output excerpt:

```text
<iso8601> INFO commit wrote commit
<iso8601> INFO mongo bootstrap commit <64-hex>
<iso8601> INFO mongo watching MongoDB change stream
```

In another terminal, write one document. This uses the `mongosh` binary inside the compose container, so a host `mongosh` install is not required:

```sh
docker compose -f docker/docker-compose.yml exec -T mongo mongosh mongodb://localhost:27017/scribe_test?replicaSet=scribe-rs --quiet --eval "db.users.insertOne({_id:'manual-alice',role:'admin'})"
```

Output:

```text
{ acknowledged: true, insertedId: 'manual-alice' }
```

Stop the adapter with `Ctrl-C` or `SIGTERM`. The release validation used:

```sh
kill -TERM <scribe-pid>
```

Output:

```text
<iso8601> INFO mongo shutdown requested; MongoDB watch stopped cleanly
```

Inspect the log:

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe log --oneline
```

Output:

```text
<12-hex> mongo change stream
<12-hex> mongo bootstrap
```

Inspect the last diff:

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe diff HEAD~1 HEAD
```

Output:

```text
A scribe_test/users/"manual-alice"
```

Inspect the head commit object:

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe cat-object -p <64-hex>
```

Output:

```text
tree <64-hex>
parent <64-hex>
author	unknown		unknown	<unix-nanos>
committer	scribe-mongo		scribe	<unix-nanos>
process	mongo-watch	1.0.0		

mongo change stream
```

Shut down MongoDB:

```sh
docker compose -f docker/docker-compose.yml down
```

Output:

```text
Container docker-mongo-1  Stopping
Container docker-mongo-1  Stopped
Container docker-mongo-1  Removing
Container docker-mongo-1  Removed
Network docker_default  Removing
Network docker_default  Removed
```

## The `.scribe/` Repository

`scribe init` creates these paths:

- `HEAD`: text pointer to `refs/heads/main`.
- `config`: v1 flat configuration file.
- `objects/`: loose compressed objects, split by hash prefix.
- `refs/heads/main`: current commit hash, created after the first commit.
- `adapter-state/mongodb`: MongoDB resume token, last commit, and update time.
- `log`: append-only operational log.
- `lock`: created while a writer has the repository open.

Objects are immutable. If the same document bytes appear twice, they hash to the same blob and the object store keeps one copy. Backups can use ordinary filesystem tools.

Non-executed example:

```sh
tar -C /tmp/scribe-manual-quick -czf scribe-backup.tgz .scribe
rsync -a /tmp/scribe-manual-quick/.scribe/ backup-host:/srv/scribe/.scribe/
```

## Command Reference

### `cat-object`

Synopsis: `scribe [--store <path>] cat-object (-p|-t|-s) <hash>`

Prints object content (`-p`), object type (`-t`), or payload size (`-s`).

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe cat-object -t <64-hex>
```

Output:

```text
commit
```

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe cat-object -s <64-hex>
```

Output:

```text
285
```

### `commit-batch`

Synopsis: `scribe [--store <path>] commit-batch`

Reads one framed pipe batch from stdin and writes `OK\t<hash>` or `ERR\t<symbol>\t<len>`.

```sh
printf $'BATCH\t1\t1\nAUTHOR\tmanual\tmanual@example.com\tmanual\nCOMMITTER\tscribe-manual\t\tscribe\nPROCESS\tmanual-pipe\t1.0\t\t\nTIMESTAMP\t1700000000000000000\nMESSAGE\t12\npipe commit\nEVENT\t1\t12\ndocs\nhello world\nEND\n' | ./build/scribe --store /tmp/scribe-manual-pipe/.scribe commit-batch
```

Output:

```text
OK	<64-hex>
```

### `diff`

Synopsis: `scribe [--store <path>] diff <commit1> [<commit2>]`

Diffs two commits. If only one commit is given, Scribe diffs its parent against it.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe diff HEAD~1 HEAD
```

Output:

```text
A scribe_test/users/"manual-alice"
```

### `fsck`

Synopsis: `scribe [--store <path>] fsck`

Walks reachability from `refs/heads/main`, verifies object envelopes and hashes while reading, and reports dangling loose objects.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe fsck
```

Output:

```text
fsck: 10 reachable objects, 0 dangling objects
```

### `info`

Synopsis: `scribe [--store <path>] info`

Prints binary version, hash algorithm, pipe protocol, and repository configuration if the store exists.

```sh
./build/scribe info
```

Output:

```text
scribe version 1.0.0
hash_algorithm blake3-256
pipe_protocol 1
store .scribe (not initialized)
```

### `init`

Synopsis: `scribe init [path]`

Creates an empty Scribe repository. If `path` is omitted, Scribe initializes the global `--store` path when supplied, otherwise `.scribe` in the current directory.

```sh
./build/scribe init /tmp/scribe-manual-quick/.scribe
```

Output:

```text
initialized /tmp/scribe-manual-quick/.scribe
```

### `log`

Synopsis: `scribe [--store <path>] log [--oneline] [-n <N>]`

Walks commit history from HEAD.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe log --oneline
```

Output:

```text
<12-hex> mongo change stream
<12-hex> mongo bootstrap
```

### `mongo-watch`

Synopsis: `scribe [--store <path>] mongo-watch <uri>` or `scribe mongo-watch <uri> --store <path>`

Runs the MongoDB adapter. It bootstraps current state, resumes from saved tokens on restart, consumes change streams, and exits cleanly on `SIGTERM` or `SIGINT`.

```sh
./build/scribe mongo-watch "mongodb://localhost:27018/?directConnection=true" --store /tmp/scribe-manual-quick/.scribe
```

Output excerpt:

```text
<iso8601> INFO mongo bootstrap commit <64-hex>
<iso8601> INFO mongo watching MongoDB change stream
```

### `show`

Synopsis: `scribe [--store <path>] show <commit>`

Prints commit metadata and touched paths.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe show HEAD
```

Output:

```text
commit <64-hex>
parent <64-hex>
author unknown <> <unix-nanos>
committer scribe-mongo <> <unix-nanos>
process mongo-watch 1.0.0  

mongo change stream

changes:
A scribe_test/users/"manual-alice"
```

## The MongoDB Adapter

Use a replica set or sharded cluster. Standalone MongoDB does not provide change streams. The local fixture in `docker/` starts a single-node replica set named `scribe-rs`, runs MongoDB on container port `27017`, publishes it on host port `27018`, and creates `scribe_test.users` with pre/post images enabled.

Connection URI format:

```text
mongodb://<host>:<port>/?replicaSet=<name>
```

For the local Docker fixture from the WSL or Windows host, use:

```text
mongodb://localhost:27018/?directConnection=true
```

The `directConnection=true` option prevents the MongoDB driver from rediscovering the replica set member as `localhost:27017`, which is the container-internal address and may also conflict with a Windows MongoDB service.

Pre- and post-images are enabled per collection:

```sh
docker compose -f docker/docker-compose.yml exec -T mongo mongosh mongodb://localhost:27017/scribe_test?replicaSet=scribe-rs --quiet --eval "db.runCommand({collMod:'users',changeStreamPreAndPostImages:{enabled:true}})"
```

Output:

```text
{ ok: 1 }
```

By default Scribe excludes `admin`, `local`, and `config`. Change `.scribe/config` key `adapter.mongodb.excluded_databases` to override that list.

On startup, `mongo-watch` reads `.scribe/adapter-state/mongodb`. If the token is usable, it resumes. If MongoDB invalidates the stream, Scribe marks the token invalid, logs a warning, and writes a new bootstrap commit parented to the existing history.

Event handling in v1:

- `insert`, `update`, `replace`, `modify`: commit the canonical full document.
- `delete`: commit a tombstone for the document path.
- multi-document transactions: one Scribe commit for the transaction.
- DDL and sharding events: log and ignore.
- `invalidate`, `drop`, `rename`, `dropDatabase`: restart bootstrap.

## Operations

### systemd

Non-executed example:

```ini
[Unit]
Description=Scribe MongoDB watcher
After=network-online.target

[Service]
ExecStart=/usr/local/bin/scribe mongo-watch mongodb://mongo.example:27017/?replicaSet=rs0 --store /var/lib/scribe/.scribe
Restart=on-failure
User=scribe
WorkingDirectory=/var/lib/scribe

[Install]
WantedBy=multi-user.target
```

### Docker

Non-executed example:

```dockerfile
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y libmongoc-1.0-0 libbson-1.0-0 ca-certificates
COPY build/scribe /usr/local/bin/scribe
ENTRYPOINT ["scribe"]
```

### Signal Handling

`SIGTERM` and `SIGINT` request clean shutdown. If a batch is in flight, Scribe finishes the commit first, then writes adapter-state and releases the lock.

Executed verification command:

```sh
kill -TERM <scribe-pid>
```

Observed log:

```text
<iso8601> INFO mongo shutdown requested; MongoDB watch stopped cleanly
```

### Disk Sizing

For loose objects on ext4, expect roughly 4 KiB per unique document blob plus 4 KiB per commit or tree object because small files occupy filesystem blocks. A 1M-document bootstrap with one collection can therefore consume several GiB before compression and filesystem overhead are considered. Pack files and garbage collection are v2 work.

### Locking

One writer owns `.scribe/lock`. A stale lock file is harmless if no process holds the advisory lock. If a writer was killed with `SIGKILL`, confirm no `scribe` process is running, then start Scribe again; the new process takes the lock and rewrites diagnostics.

### Log Rotation

`.scribe/log` is append-only and not rotated by v1. Stop the writer, rotate or truncate the log with normal file tools, then restart.

## Troubleshooting

### `SCRIBE_ERR`

Generic failure. Check the detail text and `.scribe/log`. No stable CLI reproduction exists in v1; this is primarily a fallback for internal failures.

### `SCRIBE_ENOT_FOUND`

Command:

```sh
./build/scribe --store /tmp/scribe-does-not-exist/.scribe log
```

Output:

```text
scribe: SCRIBE_ENOT_FOUND: file not found '/tmp/scribe-does-not-exist/.scribe/config'
```

Fix: pass the correct `--store`, or run `scribe init`.

### `SCRIBE_EEXISTS`

No v1 CLI path currently returns this code. It remains part of the public API for creation operations that may grow in v2.

### `SCRIBE_ELOCKED`

Command run while another writer held the same repository:

```sh
printf 'BATCH\t1\t0\n' | ./build/scribe --store /tmp/scribe-lock/.scribe commit-batch
```

Output:

```text
scribe: SCRIBE_ELOCKED: repository locked: pid <pid>
program scribe
```

Fix: stop the writer cleanly, or confirm it is gone and retry.

### `SCRIBE_EREF_STALE`

Internal compare-and-swap failure while advancing `refs/heads/main`. There is no standalone CLI reproduction in the single-writer v1 command surface. If it appears, rerun the operation after verifying only one writer is active.

### `SCRIBE_EIO`

Command:

```sh
./build/scribe init /proc/scribe-manual-io
```

Output:

```text
scribe: SCRIBE_EIO: failed to create directory '/proc/scribe-manual-io'
```

Fix: use a writable filesystem path.

### `SCRIBE_ECORRUPT`

Command after intentionally writing an invalid ref file:

```sh
./build/scribe --store /tmp/scribe-corrupt/.scribe log
```

Output:

```text
scribe: SCRIBE_ECORRUPT: malformed ref 'refs/heads/main'
```

Fix: restore the repository from backup. Do not edit object or ref files by hand.

### `SCRIBE_EINVAL`

Command:

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe diff HEAD~x HEAD
```

Output:

```text
scribe: SCRIBE_EINVAL: invalid revision 'HEAD~x'
```

Fix: use `HEAD`, `HEAD~<number>`, or a full 64-character commit hash.

### `SCRIBE_EMALFORMED`

Command:

```sh
printf $'BATCH\t1\t0\n' | ./build/scribe --store /tmp/scribe-manual-pipe/.scribe commit-batch
```

Output:

```text
ERR	SCRIBE_EMALFORMED	37
event_count must be greater than zero
```

Fix: correct the adapter frame.

### `SCRIBE_EPROTOCOL`

Command:

```sh
printf $'NOPE\n' | ./build/scribe --store /tmp/scribe-manual-pipe/.scribe commit-batch
```

Output:

```text
ERR	SCRIBE_EPROTOCOL	19
expected BATCH line
```

Fix: emit pipe protocol version 1 frames beginning with `BATCH`.

### `SCRIBE_ECONFIG`

Command after appending `unknown_key=true` to `.scribe/config`:

```sh
./build/scribe --store /tmp/scribe-trouble/.scribe info
```

Output:

```text
scribe version 1.0.0
hash_algorithm blake3-256
pipe_protocol 1
scribe: SCRIBE_ECONFIG: unknown config key 'unknown_key'
```

Fix: remove unknown keys and keep the v1 schema intact.

### `SCRIBE_EADAPTER`

Observed during Mongo startup before the replica set accepted `hello`:

```text
scribe: SCRIBE_EADAPTER: Mongo hello failed: No suitable servers found (`serverSelectionTryOnce` set): [connection closed calling hello on 'localhost:27017']
```

Fix: confirm the URI, replica-set name, and primary state. Current code retries topology checks during startup to avoid this transient in the local compose fixture.

### `SCRIBE_ENOMEM`

No safe CLI reproduction is included. In practice, reduce batch size, increase memory, or inspect whether a malformed adapter frame requested an extreme allocation.

### `SCRIBE_ENOSYS`

Non-executed example: a build without `libmongoc` support returns this for `mongo-watch`.

```text
scribe: SCRIBE_ENOSYS: mongo-watch is not implemented in Milestone 1
```

Fix: install `libmongoc`/`libbson` development packages and rebuild with `SCRIBE_BUILD_MONGO_ADAPTER=ON`.

### `SCRIBE_EINTERRUPTED`

No v1 CLI command reports this on normal `SIGTERM`; Mongo watch exits `0` after clean shutdown. Treat this as an API-level signal result if it appears in embedding code.

### `SCRIBE_EPATH`

No current CLI path preserves an empty argument through usage parsing in the release shell used for validation. For public API callers, pass a non-empty repository path.

### `SCRIBE_EHASH`

Command:

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe cat-object -p not-a-hash
```

Output:

```text
scribe: SCRIBE_EHASH: hash must be exactly 64 lowercase hex characters
```

Fix: pass a full 64-character lowercase hash, not an abbreviation.

### `SCRIBE_ECONCURRENT`

Reserved for concurrent modification detection. The v1 CLI serializes writers with `.scribe/lock`, so no separate reproduction is available.

### `SCRIBE_ESHUTDOWN`

Reserved for rejecting work after shutdown starts. The v1 CLI drains and exits cleanly instead of exposing this code to users.

### FAQ

Why is Scribe using so much disk? v1 stores loose objects only. Pack files and garbage collection are v2.

Why did my write not show up? Check that MongoDB is a replica set, the URI contains the correct `replicaSet`, and `mongo-watch` is still running.

Can I replay history back into MongoDB? No. v1 records and inspects history; it does not restore.

## Glossary

Commit: an object that names a root tree, optional parent, identities, process metadata, timestamp, and message.

Tree: an object mapping sorted names to blob or tree hashes.

Blob: an object containing canonical document bytes.

Leaf: the final path component for a document, normally the canonical Extended JSON `_id`.

Path: the ordered tree path to a document, for MongoDB `database/collection/document-id`.

Adapter: code that converts an external data source into Scribe change batches.

Canonical form: deterministic bytes for semantically equivalent data; MongoDB documents use canonical Extended JSON with sorted keys.

Bootstrap: initial full scan that writes a baseline commit.

Resume token: MongoDB change stream token stored in `.scribe/adapter-state/mongodb`.

Tombstone: a delete event represented as a path with no payload.
