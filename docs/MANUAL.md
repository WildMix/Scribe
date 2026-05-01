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
100% tests passed, 0 tests failed out of 2
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
scribe version 1.0.2
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
<iso8601> INFO mongo bootstrap commit <64-hex> with <N> document(s)
<iso8601> INFO mongo watching MongoDB change stream
commit <7-hex>  insert scribe_test/users/"manual-alice"
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
process	mongo-watch	1.0.1	<empty>	<empty>

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

### Inspecting History

`scribe show <commit>:<path>` writes the raw blob bytes stored at that path. Quote the whole argument when the path contains shell-special characters such as braces or quotes:

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe show 'HEAD:scribe_test/users/"manual-alice"' | jq .
```

Output:

```json
{
  "_id": "manual-alice",
  "role": "admin"
}
```

Blob-level diffs are composed with external tools; v1 does not include `scribe diff-blobs` because raw blob output works cleanly with `jq`, `diff`, and similar tools.

```sh
diff -u <(./build/scribe --store /tmp/scribe-manual-diff/.scribe show '<64-hex>:db/users/a' | jq .) <(./build/scribe --store /tmp/scribe-manual-diff/.scribe show '<64-hex>:db/users/a' | jq .)
```

Output:

```diff
--- /dev/fd/63
+++ /dev/fd/62
@@ -1,3 +1,3 @@
 {
-  "role": "user"
+  "role": "admin"
 }
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

### Object Storage Details

Every stored object is addressed by the BLAKE3-256 hash of its uncompressed object envelope, not by its compressed file bytes. The envelope is:

```text
<1-byte object type><unsigned LEB128 payload length><raw payload bytes>
```

The object type byte is `1` for blob, `2` for tree, and `3` for commit. The envelope is compressed with zstd and written as a loose object under `objects/<first-two-hex>/<remaining-62-hex>`. For example, object hash `abcdef...` is stored under `objects/ab/cdef...`. The two-character directory split keeps very large stores from putting every object file in one directory.

When Scribe reads an object, it does all of these checks before returning data to higher-level code:

- the object path exists and can be read;
- the zstd frame has a known decompressed length;
- decompression succeeds and produces exactly that length;
- BLAKE3 over the decompressed envelope equals the requested hash;
- the envelope has a valid type byte and LEB128 payload length;
- the payload length encoded in the envelope exactly accounts for the remaining bytes.

This means commands such as `cat-object`, `diff`, `show`, `log --paths`, `list-objects`, and `fsck` verify object envelopes and hashes as a side effect of normal reads.

### Object Types

A blob stores raw payload bytes. For MongoDB, those bytes are canonical Extended JSON for a document. Scribe core does not know JSON semantics; it treats the blob as opaque bytes.

A tree stores a sorted list of entries. Each entry contains the entry type, the child hash, the entry name length, and the entry name bytes. Entry names are sorted byte-for-byte, duplicates are rejected, and names containing tab or newline are rejected at batch validation. A MongoDB document path normally has three tree components: `database/collection/canonical-document-id`.

A commit stores:

- root tree hash;
- optional parent commit hash;
- author identity and timestamp;
- committer identity and timestamp;
- process name, version, parameters, and correlation id;
- free-form message bytes.

There is one v1 branch-like ref: `refs/heads/main`. A commit becomes visible only after the ref file is atomically replaced with the new commit hash.

### Configuration Details

`.scribe/config` is parsed on every command. Unknown keys fail with `SCRIBE_ECONFIG`; this is intentional because silently accepting misspelled operational settings is risky. Required v1 keys are:

- `scribe_format_version`: currently `1`.
- `hash_algorithm`: must be `blake3-256`.
- `compression`: must be `zstd`.
- `compression_level`: zstd level used for newly written loose objects.
- `worker_threads`: number of Mongo bootstrap worker threads; `0` means use a default.
- `event_queue_capacity`: queue capacity used by pipe/library commit flow and Mongo worker coordination.
- `queue_stall_warn_seconds`: threshold for queue stall warnings.
- `adapter.name`: must be `mongodb`.
- `adapter.mongodb.excluded_databases`: comma-separated database names ignored during cluster-scoped bootstrap.
- `adapter.mongodb.require_pre_post_images`: v1 configuration hook for stricter Mongo collection validation.
- `adapter.mongodb.coalesce_window_ms`: v1 configuration hook for future event coalescing.

Changing configuration affects new command invocations. Existing running `mongo-watch` processes keep the configuration they loaded at startup.

Non-executed example:

```sh
tar -C /tmp/scribe-manual-quick -czf scribe-backup.tgz .scribe
rsync -a /tmp/scribe-manual-quick/.scribe/ backup-host:/srv/scribe/.scribe/
```

## Command Reference

### `cat-object`

Synopsis: `scribe [--store <path>] cat-object (-p|-t|-s) <hash>`

Reads exactly one object by full 64-character hash. The read path verifies the compressed frame, envelope length, and BLAKE3 hash before printing anything.

Modes:

- `-t` prints only the object type name: `blob`, `tree`, or `commit`.
- `-s` prints the uncompressed payload size, not the compressed on-disk size and not the envelope size.
- `-p` pretty-prints known structured objects. Tree objects are printed as one entry per line in storage order. Blob objects are written as raw payload bytes, with a newline added only when the blob payload is empty or does not already end in newline. Commit objects are written as their raw text payload, followed by a newline for terminal readability.

This command does not resolve revisions such as `HEAD`; pass an object hash. To inspect a revision, first get the commit hash from `log`, `show HEAD`, or `refs/heads/main`.

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

Reads pipe protocol v1 frames from standard input. Each frame becomes one commit if validation and storage succeed. On success, stdout receives `OK\t<commit-hash>`. On failure, stdout receives `ERR\t<symbol>\t<len>`, followed by the detail bytes and a newline; the process exits non-zero.

The frame order is strict:

```text
BATCH<TAB>1<TAB><event-count>
AUTHOR<TAB><name><TAB><email><TAB><source>
COMMITTER<TAB><name><TAB><email><TAB><source>
PROCESS<TAB><name><TAB><version><TAB><params><TAB><correlation-id>
TIMESTAMP<TAB><unix-nanos>
MESSAGE<TAB><byte-count>
<message bytes if byte-count is nonzero>
EVENT<TAB><path-depth><TAB><payload-byte-count>
<one path component line per path-depth>
<payload bytes if payload-byte-count is nonzero>
END
```

Path components are line-based UTF-8 byte strings. Empty components, tabs, and newlines are rejected because they would make tree paths ambiguous. A payload byte count of `0` means tombstone/delete. A nonzero payload writes a blob and updates the leaf path to that blob.

Commit construction loads the current `refs/heads/main` tree if it exists, applies the batch in order, writes any new blobs, recursively writes changed trees, writes a commit object, and finally advances `refs/heads/main` with a compare-and-swap. If the ref changed unexpectedly, the command fails with `SCRIBE_EREF_STALE` rather than silently overwriting history.

```sh
printf $'BATCH\t1\t1\nAUTHOR\tmanual\tmanual@example.com\tmanual\nCOMMITTER\tscribe-manual\t\tscribe\nPROCESS\tmanual-pipe\t1.0\t\t\nTIMESTAMP\t1700000000000000000\nMESSAGE\t12\npipe commit\nEVENT\t1\t12\ndocs\nhello world\nEND\n' | ./build/scribe --store /tmp/scribe-manual-pipe/.scribe commit-batch
```

Output:

```text
OK	<64-hex>
```

### `diff`

Synopsis: `scribe [--store <path>] diff <commit1> [<commit2>]`

Compares two commit root trees and prints path-level changes. If only `<commit1>` is supplied, Scribe resolves that commit and compares its parent to it. If `<commit1>` has no parent, there is no output because there is no previous tree to compare against.

Revision arguments may be `HEAD`, `HEAD~<N>`, or a full 64-character commit hash. The output is one changed leaf path per line:

- `A <path>`: the path exists in the newer tree and not in the older tree.
- `D <path>`: the path exists in the older tree and not in the newer tree.
- `M <path>`: the path exists in both trees but the leaf hash differs, or a defensive blob/tree type mismatch is found.

Diff walks trees in byte-sorted storage order. When a subtree is added or deleted, Scribe descends through that subtree and reports every affected leaf. It does not inspect blob contents and does not parse JSON.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe diff HEAD~1 HEAD
```

Output:

```text
A scribe_test/users/"manual-alice"
```

### `fsck`

Synopsis: `scribe [--store <path>] fsck`

Checks repository object integrity from the point of view of the current main history.

The check runs in two phases:

1. Reachability walk. Scribe reads `refs/heads/main`. If it exists, that commit is the root of the walk. For each reachable commit, Scribe verifies and parses the commit object, walks to the commit's root tree, and then walks the parent commit if one exists. For each reachable tree, Scribe verifies and parses the tree object and walks every child hash using the entry type recorded in the tree. Blob objects are verified by reading them, but they have no children.
2. Dangling-object scan. Scribe iterates every loose object file under `objects/??/*`. A loose object is considered dangling when its hash was found on disk but was not visited during the reachability walk from `refs/heads/main`.

`fsck` detects:

- malformed or unreadable `refs/heads/main`;
- missing objects that are referenced by a reachable commit or tree;
- corrupted zstd frames;
- object envelope length mismatches;
- BLAKE3 hash mismatches;
- objects whose actual type does not match the type expected from the parent reference;
- malformed commit or tree payloads;
- dangling loose objects.

Dangling objects are warnings, not corruption. v1 intentionally allows them because an interrupted write can leave objects on disk before the main ref advances, and future garbage collection is out of scope for v1. `fsck` prints one `warning: dangling object <hash>` line per dangling object, then prints the final summary. If the repository has no commits, `fsck` prints `fsck: no commits` and exits successfully.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe fsck
```

Output:

```text
fsck: 10 reachable objects, 0 dangling objects
```

### `info`

Synopsis: `scribe [--store <path>] info`

Prints information about the binary and, if the repository exists, selected repository configuration. `info` opens the store read-only, so it does not take `.scribe/lock` and can be run while `mongo-watch` is active.

If the store path does not exist, `info` still prints the binary version, hash algorithm, and pipe protocol, then reports that the store is not initialized. If the store exists but `.scribe/config` has an unknown key, missing required key, unsupported hash algorithm, or unsupported format version, `info` fails with the same configuration error other commands would see.

```sh
./build/scribe info
```

Output:

```text
scribe version 1.0.2
hash_algorithm blake3-256
pipe_protocol 1
store .scribe (not initialized)
```

### `init`

Synopsis: `scribe init [path]`

Creates an empty Scribe repository. If `path` is omitted, Scribe initializes the global `--store` path when supplied, otherwise `.scribe` in the current directory.

`init` creates directories with normal filesystem permissions inherited from the process umask. It writes `HEAD`, `config`, and `log` atomically. It does not create `refs/heads/main`; that ref appears after the first successful commit. Running `init` on an existing repository rewrites the config/log skeleton and should be treated as an administrative action, not a routine command.

```sh
./build/scribe init /tmp/scribe-manual-quick/.scribe
```

Output:

```text
initialized /tmp/scribe-manual-quick/.scribe
```

### `list-objects`

Synopsis: `scribe [--store <path>] list-objects [--type=blob|tree|commit] [--reachable] [--format=<spec>]`

Lists objects known to the object store iterator. v1 object storage is loose files, but the command deliberately uses the object-store iterator instead of walking the filesystem directly; when pack files arrive in a future version, this command should keep working through the iterator.

Default output is unsorted filesystem iteration order and one object per line as `<hash> <type> <uncompressed-size>`. Pipe to `sort` when stable order is needed.

Multiple `--type=` flags accumulate. `--reachable` walks the full parent chain from `HEAD`, plus every tree and blob reachable from each commit root tree, and keeps that reachable hash set in memory. On very large stores this can be significant. `%C` in the format performs one `stat` per object to report compressed on-disk size; this is acceptable for v1 one-off inspection, not a high-volume query path.

Supported format placeholders are `%H` hash, `%T` type, `%S` uncompressed payload size, and `%C` compressed/on-disk size.

Without `--reachable`, dangling objects and objects from abandoned writes are included. With `--reachable`, Scribe first builds an in-memory set of hashes visited from `refs/heads/main`, then iterates the store and prints only objects in that set. This means `--reachable` still verifies objects through normal object reads; a corrupt reachable object fails the command.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe list-objects --reachable --type=commit --format='%H %T %S %C'
```

Output:

```text
<64-hex> commit 285 <compressed-bytes>
<64-hex> commit 240 <compressed-bytes>
```

### `log`

Synopsis: `scribe [--store <path>] log [--oneline] [--paths] [-n <N>] [--] [<path>]`

Walks commit history from `HEAD`. By default, every commit in the parent chain is emitted. With a positional `<path>`, only commits where that path's blob or tree hash differs from the parent are emitted. Use `--` before the path when the path could be mistaken for an option.

`--paths` lists changed leaf paths for each emitted commit, prefixed with `A`, `M`, or `D`. For the initial commit, all leaf paths are listed as `A`. In `--oneline` mode, `--paths` appends `[N changed]` instead of listing each path. When a path filter is active, `-n <N>` limits emitted commits, not scanned commits.

The first commit where a filtered path appears is annotated `(added)`. A commit where it disappears is annotated `(deleted)`. Tree-level paths are valid; any change underneath changes the tree hash and matches the filter.

Full log output prints commit hash, parent hash when present, author line, committer line, message, and optional path information. `--oneline` prints the 12-character hash abbreviation and the commit message only. Abbreviations are display-only; commands that read an object still require a full hash unless the command explicitly accepts `HEAD` or `HEAD~<N>`.

Path filtering uses the same tree resolver as `show <commit>:<path>`. For each commit scanned from newest to oldest, Scribe resolves the path in the current root tree and in the parent root tree. If both resolutions are absent, or both point to the same object hash with the same object type, the commit is skipped. If the state or hash differs, the commit is emitted. This is why tree-level filters match any change under the tree: the tree object's hash changes when any child entry changes.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe log --oneline
```

Output:

```text
<12-hex> mongo change stream
<12-hex> mongo bootstrap
```

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe log --paths -- 'scribe_test/users/"manual-alice"'
```

Output excerpt:

```text
commit <64-hex>
parent <64-hex>
author unknown <> <unix-nanos>
committer scribe-mongo <> <unix-nanos>

mongo change stream

  (added)
  A scribe_test/users/"manual-alice"
```

### `ls-tree`

Synopsis: `scribe [--store <path>] ls-tree <hash>`

Lists a tree recursively in byte-sorted UTF-8 storage order. If `<hash>` is a commit, Scribe lists the commit's root tree. If `<hash>` is a blob, Scribe fails with `SCRIBE_EINVAL`.

Output is tab-separated: `<type>\t<hash>\t<path>`.

`ls-tree` prints tree entries and blob entries. A tree entry is printed before the command descends into that tree. Paths are assembled with `/` between tree entry names. No quoting or escaping is applied because Scribe rejects tab and newline in path components; the tab-separated output remains unambiguous.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe ls-tree <64-hex>
```

Output:

```text
tree	<64-hex>	scribe_test
tree	<64-hex>	scribe_test/users
blob	<64-hex>	scribe_test/users/"manual-alice"
```

### `mongo-watch`

Synopsis: `scribe [--store <path>] mongo-watch <uri>` or `scribe mongo-watch <uri> --store <path>`

Runs the MongoDB adapter. It bootstraps current state, resumes from saved tokens on restart, consumes change streams, and exits cleanly on `SIGTERM` or `SIGINT`.

Startup sequence:

1. Open the `.scribe/` store writable and acquire `.scribe/lock`.
2. Parse the MongoDB URI. If the URI contains a database path, use database-scoped bootstrap/watch. Otherwise use cluster-scoped bootstrap/watch and skip configured excluded databases.
3. Connect to MongoDB and retry `hello` until the replica set topology is usable.
4. Read `.scribe/adapter-state/mongodb`. If no usable resume token exists, run bootstrap.
5. Open a change stream with `fullDocument: "updateLookup"`, `fullDocumentBeforeChange: "whenAvailable"`, and `showExpandedEvents: true`.
6. Convert each data event into a Scribe change. Inserts, updates, replaces, and modifies write the canonical full document. Deletes write a tombstone.
7. Persist adapter state only after the corresponding Scribe commit succeeds.

Bootstrap scans databases, collections, and documents. Worker threads canonicalize BSON documents and compute deterministic paths in parallel. The final snapshot tree is `database/collection/document-id`, where `document-id` is canonical Extended JSON for `_id`. The bootstrap commit is parented to existing history if the repository already has commits.

The adapter-state file has three text lines:

```text
resume_token <base64-token-or-invalid>
last_commit <64-hex>
last_updated <iso8601>
```

The resume token is the MongoDB BSON resume token encoded as base64 so it can be stored safely in a line-oriented file. The parser requires exactly these three lines, in this order, with one space after each field name. The file is written only after a commit lands. If MongoDB later rejects the token as unusable, Scribe writes `invalid`, runs a fresh bootstrap parented to existing history, stores the new token, and continues.

```sh
./build/scribe mongo-watch "mongodb://localhost:27018/?directConnection=true" --store /tmp/scribe-manual-quick/.scribe
```

Output excerpt:

```text
<iso8601> INFO mongo bootstrap commit <64-hex> with <N> document(s)
<iso8601> INFO mongo watching MongoDB change stream
```

After each change-stream commit lands and adapter state is persisted, `mongo-watch` prints a plain commit summary line without the normal log prefix:

```text
commit <7-hex>  insert scribe_test/users/"alice"
commit <7-hex>  update scribe_test/users/"alice"
commit <7-hex>  delete scribe_test/users/"alice"
```

For a multi-document transaction, one Scribe commit is still written. The adapter prints one commit header plus the operations contained in that commit:

```text
commit <7-hex>  transaction 2 events
  update scribe_test/users/"alice"
  insert scribe_test/orders/"o_4913"
```

### `show`

Synopsis: `scribe [--store <path>] show <commit>` or `scribe [--store <path>] show <commit>:<path>`

Without `:<path>`, prints commit metadata and touched paths. With `:<path>`, resolves the path in the commit root tree. Blob paths write raw blob bytes to stdout with no Scribe-added newline; tree paths list recursively like `ls-tree`. An empty path, `<commit>:`, lists the commit root tree.

`show <commit>` resolves `HEAD`, `HEAD~<N>`, or a full commit hash, reads that commit, prints metadata, then computes changed paths by diffing the parent root tree against the commit root tree. Initial commits have no parent, so their `changes:` section is empty in v1 full-show output.

`show <commit>:<path>` splits at the first colon. The left side is resolved as a commit. The right side is split on `/` and matched byte-for-byte against tree entry names. There is no path normalization: `a/b`, `a//b`, and `./a/b` are different byte strings. If an intermediate component resolves to a blob, the command fails with `SCRIBE_ENOT_FOUND` and the message `path component '<name>' resolved to a blob; cannot descend further`.

If the final path resolves to a blob, Scribe writes exactly the stored blob payload bytes. It does not print headers, object hashes, or a trailing newline. This is the intended way to pipe a document snapshot into `jq`, `diff`, or other external tools. If the final path resolves to a tree, Scribe lists that tree recursively with the same output format as `ls-tree`.

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe show HEAD
```

Output:

```text
commit <64-hex>
parent <64-hex>
author unknown <> <unix-nanos>
committer scribe-mongo <> <unix-nanos>
process mongo-watch 1.0.1 <empty> <empty>

mongo change stream

changes:
A scribe_test/users/"manual-alice"
```

```sh
./build/scribe --store /tmp/scribe-manual-quick/.scribe show 'HEAD:scribe_test/users/"manual-alice"'
```

Output:

```text
{"_id":"manual-alice","role":"admin"}
```

## The MongoDB Adapter

Use a replica set or sharded cluster. Standalone MongoDB does not provide change streams. The local fixture in `docker/` starts a single-node replica set named `scribe-rs`, runs MongoDB on container port `27017`, publishes it on host port `27018`, and creates `scribe_test.users` with pre/post images enabled.

Connection URI format:

```text
mongodb://<host>:<port>/?replicaSet=<name>
```

When the URI omits a database path, Scribe bootstraps every non-excluded database and opens a cluster-wide change stream. That requires MongoDB privileges sufficient to watch the cluster through the `admin` database. For hosted MongoDB users with narrower permissions, include the database path in the URI; Scribe then bootstraps only that database and uses a database-scoped change stream:

```text
mongodb+srv://<credentials>@<cluster>/<database>?authMechanism=MONGODB-AWS&authSource=%24external
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

An explicit database path takes precedence over the excluded-database list because it is an operator-selected scope.

On startup, `mongo-watch` reads `.scribe/adapter-state/mongodb`. If the token is usable, it resumes. If MongoDB rejects the saved token with `cannot resume stream` or `resume token was not found`, Scribe treats it like an invalidated stream: it marks the token invalid, logs a warning, writes a new bootstrap commit parented to the existing history, stores the new resume token, and continues watching.

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

`SIGTERM` and `SIGINT` request clean shutdown. If a batch is in flight, Scribe finishes the commit first, then writes adapter-state and releases the lock. If libmongoc reports an interrupted change stream after shutdown has already been requested, Scribe treats that as part of clean shutdown rather than surfacing `SCRIBE_EADAPTER`.

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
scribe version 1.0.2
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

Observed when the URI omits a database path but the MongoDB user cannot open a cluster-wide change stream on `admin`:

```text
scribe: SCRIBE_EADAPTER: failed to capture MongoDB resume token: not authorized on admin to execute command { aggregate: 1, pipeline: [ { $changeStream: { showExpandedEvents: true, allChangesForCluster: true } } ], ... }
```

Fix: either grant cluster-wide change stream privileges, or scope Scribe to one database by adding the database path before the query string, for example `mongodb+srv://<credentials>@<cluster>/<database>?authMechanism=MONGODB-AWS&authSource=%24external`.

Observed when a saved MongoDB change stream token has fallen out of the server oplog window or otherwise cannot be resumed:

```text
scribe: SCRIBE_EADAPTER: failed to open MongoDB change stream: PlanExecutor error during aggregation :: caused by :: cannot resume stream; the resume token was not found.
```

Current code recovers from this automatically: it logs `change stream resume token is unusable; restarting bootstrap`, writes a new bootstrap commit parented to existing history, persists the replacement token, and continues watching. If this error still reaches the CLI, rebuild and confirm the installed `scribe` binary is the updated one.

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
