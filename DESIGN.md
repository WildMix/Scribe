# Scribe — Design (v1)

**Status:** frozen for v1 implementation. Decisions here are intentionally narrow so that an implementer (human or agentic) can execute against them without having to invent policy. Items marked *Open (v2)* are explicitly out of scope for v1.

---

## 1. Scope

Scribe records the evolution of a data store as a chain of content-addressed snapshots, using Git's object model (blobs, trees, commits, refs) adapted for structured data rather than files. The analogy to Git is structural, not literal.

Scribe is an observer. It does not serve reads of live data, does not restore data, and does not intercept writes. It produces a verifiable, traversable history of whatever data store it is pointed at. Restoring a store from a Scribe history is technically possible but is not a v1 operation.

## 2. Core model: state, not change

A Scribe history is a chain of full state snapshots. Each snapshot is a Merkle tree whose root hash uniquely identifies the state of the observed data store at a point in time. A *change* is not a first-class object in Scribe; a change is the result of diffing two snapshot trees by descending only where hashes diverge. This mirrors Git: snapshots are stored, diffs are derived.

Scribe never stores "field X was changed to Y" as an object. It stores "the document containing field X now hashes to H2 instead of H1." When asked what changed, it walks both trees from the root down until it reaches the diverging leaves.

## 3. Object types and on-disk format

Scribe has three object types, matching Git.

A **blob** is an opaque byte sequence: one whole logical entity (for MongoDB, one document) in the adapter's chosen canonical form. Scribe core never inspects blob contents. Its identity is the hash of its bytes.

A **tree** is an ordered list of entries. Each entry has a 1-byte type tag (`0x01` = blob, `0x02` = tree), a 32-byte raw hash, a LEB128-encoded name length, and the name as UTF-8 bytes. Entries are sorted by name (byte-wise lexicographic over UTF-8) so that the tree's serialization is deterministic. Its identity is the hash of this serialization.

A **commit** is a structured object referencing a single root tree, zero or one parent commit, an author, a committer, a UTC timestamp with nanosecond precision, a process descriptor, and an optional message. Its identity is the hash of its serialization.

**Serialization choices.**

Trees use the binary format above. This is not cosmetic: a tree with 1000 entries in a textual format is ~90 KB; the same tree in binary is ~45 KB. Trees dominate object count in a large history.

Blobs are the adapter's canonical bytes, opaque to Scribe.

Commits use a Git-style line-based text format. No JSON parser is required. Each line is terminated by `\n`. Fields within header lines are separated by `\t` (tab). The author/committer/process field values are forbidden from containing `\n` or `\t`; such input is rejected at batch submission (§12.7). A blank line separates the header from the message. The message is free text including `\n` but is length-prefixed in the pipe protocol (§12.3) so no escaping is needed.

```
tree <64-hex>\n
parent <64-hex>\n          (absent in the initial commit; exactly one otherwise in v1)
author\t<name>\t<email>\t<source>\t<unix-nanos>\n
committer\t<name>\t<email>\t<source>\t<unix-nanos>\n
process\t<name>\t<version>\t<params>\t<correlation_id>\n
\n
<message bytes, possibly empty, no trailing newline added by Scribe>
```

`<unix-nanos>` is a decimal integer count of nanoseconds since the Unix epoch (UTC). Signed to allow pre-1970 dates if an adapter genuinely reports them; in practice always positive.

**On-disk object envelope.** Every object — blob, tree, or commit — is stored inside a common envelope: `<type-byte><uncompressed-length:LEB128><payload>`, compressed with zstd. The hash is computed over the *uncompressed* envelope, so the compression algorithm is an implementation detail that can change without invalidating hashes.

## 4. Content addressing

Every object is stored under its hash. The hash algorithm for v1 is **BLAKE3-256** (32 raw bytes, rendered as 64 hex characters when humans are involved). BLAKE3 is chosen for throughput — its SIMD reference implementation runs at 5–10 GB/s on modern x86_64. The algorithm identifier is recorded in `.scribe/config`; readers reject stores written under an unsupported hash.

Deduplication is automatic: any subtree whose contents are identical to a previously-written subtree resolves to the same hash and is stored once. In practice, most of the tree between adjacent commits is shared.

### 4.1 LEB128 encoding

Unsigned LEB128 (Little-Endian Base 128) is used wherever a variable-length integer is needed in Scribe's binary formats (tree entry name lengths, envelope uncompressed lengths). The encoding:

```
while value > 0x7F:
    emit byte (value & 0x7F) | 0x80        /* continuation bit set */
    value >>= 7
emit byte value & 0x7F                     /* continuation bit clear */
```

Decoding reads bytes until one with the high bit clear, shifting each low 7 bits into the result at an increasing 7-bit offset. A value is invalid if:

- The encoding has more than 10 bytes (enough for a 64-bit value).
- The last byte read has the continuation bit set (truncated stream).

Scribe uses only unsigned LEB128 in v1. Decoders enforce both limits and return `SCRIBE_ECORRUPT` on violation.

## 5. Data serialization and canonicalization

**Blob canonicalization is the adapter's responsibility.** Scribe core has no opinion on how the adapter serializes a document — JSON, BSON, MessagePack, a custom binary format, anything — because Scribe core never reads blob contents. The only requirement is determinism: the same logical document, observed twice, must produce byte-identical blob bytes. Two documents whose logical equality holds but whose serialization differs (reordered keys, different number representations, etc.) will produce different blob hashes and therefore spurious history entries.

Consequences:

- A JSON-emitting adapter must canonicalize key order, whitespace, and number representation. RFC 8785 (JCS) is the standard; see §13.1 for the MongoDB adapter's concrete mapping.
- A binary-format-emitting adapter must choose a format with a single unambiguous encoding (MessagePack in canonical form, BSON with fixed field order, etc.).
- An adapter that cannot achieve determinism on its source format must canonicalize to one that can. The MongoDB adapter canonicalizes BSON to canonical JSON for exactly this reason — BSON's byte representation depends on insertion order, but two logically identical documents inserted in different orders must produce the same Scribe blob.

Scribe core itself does not parse JSON. Its own textual formats (commits, config, pipe protocol) use simple line-based encodings that require only `memchr`, `strtoll`, and length-prefixed binary reads.

## 6. The tree shape is adapter-defined

Scribe's object model (blob, tree, commit) is fixed. The *shape* of the Merkle tree — how many levels, what each represents, how child names are computed — is defined by the adapter. Scribe the engine does not know what a "database" or a "collection" is. It knows trees and blobs.

For a MongoDB instance, the recommended four-level shape is:

```
commit ──► instance-tree ──► database-tree ──► collection-tree ──► document (blob)
           (names: db       (names: coll      (names: _id
            names)           names)            canonical form)
```

It is not the only valid shape. A relational adapter might use `server → database → schema → table → row`. A key-value adapter might use `root → shard → key`. A filesystem adapter might mirror directory structure at arbitrary depth.

**Rules an adapter must satisfy:**

Every internal node is a tree per §3. Every leaf is a blob per §3. Every entry has a stable name under a documented comparator so that tree hashes are deterministic. The same logical entity (same row, same document, same object) must always appear at the same path under the same name across commits — violating this breaks incremental commit construction (§10) and diff (§11).

For v1, leaves are always whole-document blobs. Splitting a document into a subtree of fields is a recognized v2 extension that requires no protocol changes.

## 7. Repository layout on disk

```
.scribe/
  HEAD                    # text, symbolic ref: "ref: refs/heads/main\n"
  config                  # flat key=value, see §17
  lock                    # flock-based write lock, see §15
  log                     # append-only operational log, see §18
  objects/
    <xx>/<rest-of-hash>   # loose zstd-compressed objects
    ...
  refs/
    heads/
      main                # 64 hex chars + \n: commit hash of tip
  adapter-state/
    <adapter-name>        # opaque adapter-private state (e.g., resume tokens)
```

`.scribe/` does not need to live next to the observed data store; for MongoDB, it lives on whatever host runs the adapter process.

**Note on scale.** v1 uses only loose objects — one file per object. On a typical ext4 filesystem, this means a floor of ~4 KB per object regardless of compressed size. For millions of small documents this is storage-inefficient. Pack files (delta-compressed object bundles) are *Open (v2)* and are the known fix. v1 is sized for correctness and for realistic development-size stores (tens of thousands to low millions of objects), not for production at tens of millions.

## 8. Storage interface

All access to persistent state goes through two narrow interfaces. No other component reads or writes `.scribe/` directly.

**`scribe_object_store`** — content-addressed object storage:

```c
int scribe_objects_put(scribe_object_store *s, const uint8_t hash[32],
                       const uint8_t *bytes, size_t len);
int scribe_objects_get(scribe_object_store *s, const uint8_t hash[32],
                       uint8_t **out_bytes, size_t *out_len);  /* caller frees */
int scribe_objects_has(scribe_object_store *s, const uint8_t hash[32]);
int scribe_objects_iter(scribe_object_store *s, scribe_object_visit_fn, void *ctx);
```

`put` is idempotent: writing the same bytes under the same hash twice is a no-op.

**`scribe_ref_store`** — mutable named pointers:

```c
int scribe_refs_read(scribe_ref_store *s, const char *name,
                     uint8_t out_hash[32]);  /* returns SCRIBE_ENOT_FOUND if absent */
int scribe_refs_cas(scribe_ref_store *s, const char *name,
                    const uint8_t expected[32],  /* NULL iff creating */
                    const uint8_t new_hash[32]);
```

`cas` is an atomic compare-and-swap; returns `SCRIBE_EREF_STALE` if the ref no longer matches `expected`.

v1 ships a single implementation of each, backed by the local filesystem. The commit builder, adapter interface, diff logic, and CLI depend only on these interfaces. Alternative implementations (S3-backed, single-file bundle, in-memory for tests) are *Open (v2)*.

## 9. References

v1 supports a single branch `main`, a single ref file `refs/heads/main`, and a symbolic `HEAD` pointing at it. Updates are atomic rename of a temp file within `refs/heads/`. Tags, multiple branches, remote refs, and reflog are *Open (v2)*.

## 10. Commit construction

When the adapter reports a single leaf change, the commit builder does O(tree depth) work regardless of dataset size.

For one changed leaf: write the new leaf blob; rebuild the containing tree by copying the old entry list and replacing one entry; rebuild that tree's parent the same way, and so on up to the root; write a new commit referencing the new root tree and the previous commit as its parent; `cas`-update `refs/heads/main`.

For a four-level MongoDB tree, a single-document change produces exactly five new objects (one blob, three trees, one commit). Every other object in the new root tree is a reused pointer to an existing object.

When the adapter reports a batch of changes that belong together (e.g., a MongoDB transaction), the builder applies all leaf changes first, then rebuilds the parent trees once. A transaction produces one commit, not one per document.

v1 commits always have exactly one parent, except the initial commit with zero parents. Merge commits (multiple parents) are reserved in the format but *Open (v2)* — no v1 operation can produce them.

## 11. Diffs

A diff between two commits walks their root trees in lockstep. At each tree, entries are compared by name. Entries whose hashes match are skipped entirely — this is where dedup pays off at read time. Entries that differ are descended into if both sides are trees, or reported as a blob change if leaves. Entries present on only one side are reported as adds or deletes.

The diff surface is naturally scoped: show all changes, or scope to a prefix (e.g., "changes under `db1/users/`"). No diff-specific object type exists; diffs are always computed.

## 12. Adapter interface

Adapters convert observations of a live data store into leaf-level change events and hand those to the Scribe commit builder. Two wiring forms, same underlying commit builder, identical resulting commit objects.

### 12.1 Structs

The library-form C API surface:

```c
/* Identity of an author or committer. */
typedef struct {
    const char *name;    /* UTF-8, no '\n' or '\t'; required non-empty */
    const char *email;   /* UTF-8, no '\n' or '\t'; may be "" */
    const char *source;  /* free-form tag, e.g. "db_auth", "scribe",
                            "unknown"; no '\n' or '\t'; required non-empty */
} scribe_identity;

/* Provenance of the batch. */
typedef struct {
    const char *name;           /* e.g. "mongo-change-stream"; required */
    const char *version;        /* e.g. Scribe version "1.0.0"; required */
    const char *params;         /* free-form; may be "" */
    const char *correlation_id; /* opaque, e.g. a resume token; may be "" */
} scribe_process_info;

/* One change event: a path from root to leaf, plus the new blob. */
typedef struct {
    const char * const *path;   /* path_len pointers to UTF-8 names */
    size_t path_len;            /* > 0 */
    const uint8_t *payload;     /* canonical blob bytes; NULL iff deletion */
    size_t payload_len;         /* 0 iff payload == NULL (tombstone) */
} scribe_change_event;

/* A batch that becomes exactly one commit. */
typedef struct {
    const scribe_change_event *events;
    size_t event_count;            /* > 0 */
    scribe_identity author;
    scribe_identity committer;
    scribe_process_info process;
    int64_t timestamp_unix_nanos;  /* adapter-provided, UTC; see §12.4 */
    const char *message;           /* NULL for empty; UTF-8 */
    size_t message_len;
} scribe_change_batch;

/* Commit the batch. On success, out_commit_hash receives the 32-byte hash. */
int scribe_commit_batch(scribe_ctx *ctx,
                        const scribe_change_batch *batch,
                        uint8_t out_commit_hash[32]);
```

### 12.2 Library form

Same-process C adapters link against `libscribe.so` and call `scribe_commit_batch` directly. Zero serialization, maximum throughput. The MongoDB adapter uses this form.

### 12.3 Pipe form

Any-language adapters emit batches over stdout and pipe into `scribe commit-batch`, which reads stdin and drives the same internal builder. The framing is line-oriented with length-prefixed binary payloads — no JSON, trivially parseable with `fgets`/`fread`. Lines end with `\n`. Fields within header lines are separated by `\t`.

**Command (adapter → Scribe):**

```
BATCH\t<protocol-version>\t<event_count>\n
AUTHOR\t<name>\t<email>\t<source>\n
COMMITTER\t<name>\t<email>\t<source>\n
PROCESS\t<name>\t<version>\t<params>\t<correlation_id>\n
TIMESTAMP\t<unix-nanos>\n
MESSAGE\t<byte-length>\n
<message bytes; no trailing \n added>
EVENT\t<path-depth>\t<payload-byte-length>\n
<path-component-1>\n
<path-component-2>\n
...
<path-component-N>\n
<payload bytes; no trailing \n added>
EVENT\t...                           (repeated event_count times)
END\n
```

- `<protocol-version>` is `1` for v1. Unknown versions produce an immediate error response and process exit.
- Path components must not contain `\n` or `\t`. Payload bytes are arbitrary.
- Lengths are decimal ASCII integers.
- `<payload-byte-length>` of `0` with no following bytes signals deletion (tombstone).
- The receiver reads exactly `<byte-length>` bytes after `MESSAGE`, and exactly `<payload-byte-length>` bytes after the last path line of each `EVENT`. No delimiter is expected between these binary regions and the next line.

**Response (Scribe → adapter, on stdout):**

```
OK\t<64-hex-commit>\n
```

or:

```
ERR\t<error-symbol>\t<detail-byte-length>\n
<detail bytes>\n
```

`<error-symbol>` is the symbolic name from §21 (e.g. `SCRIBE_EMALFORMED`). `<detail bytes>` is a human-readable UTF-8 explanation. After emitting an error the process exits with a non-zero status.

Backpressure is handled by OS pipe buffers. Scribe reads one complete BATCH, commits it, writes the response, then reads the next. A slow Scribe causes the adapter's `write` to block — this is the intended behavior.

### 12.4 Timestamps

Commit timestamps are **adapter-provided**, never synthesized by Scribe core. The adapter supplies `timestamp_unix_nanos` as a signed 64-bit count of nanoseconds since the Unix epoch (UTC). This choice preserves causal correctness: the commit is timestamped at the moment the *underlying event* happened, not when Scribe processed it. For the MongoDB adapter this is the change event's `clusterTime` converted to nanoseconds (§13.3).

Scribe does not enforce monotonicity. Out-of-order timestamps are permitted and recorded verbatim; consumers that want a monotonic history use commit parent links, which always reflect processing order.

### 12.5 Exactly-once delivery

The commit builder is idempotent at the *object* level (same bytes under same hash is a no-op) but not at the *commit* level, since commit timestamps produce distinct hashes even for identical tree content. Adapters with resumable upstream progress (MongoDB change stream tokens) must persist the resume token only *after* Scribe acknowledges the commit. The MongoDB adapter follows this rule (§13.3).

### 12.6 Scope of the contract

Scribe core has no knowledge of data stores, transaction models, serialization formats, or identity systems. Everything store-specific — change detection, transaction grouping, identity extraction, path derivation, canonicalization — lives in the adapter.

### 12.7 Malformed input

All fields listed as required in §12.1 must be non-NULL and non-empty, except where explicitly permitted to be `""`. Input failing any of the following checks is rejected with `SCRIBE_EMALFORMED` and the batch is not committed:

- Any required string field is NULL, empty, or contains `\n` or `\t`.
- `event_count == 0` or any event's `path_len == 0`.
- Any path component is empty or contains `\n`.
- `payload` is NULL but `payload_len != 0`, or vice versa, except for the documented tombstone case (`payload == NULL && payload_len == 0`).
- LEB128 or line-framed pipe input that is truncated, over-long, or otherwise violates §4.1 or §12.3.
- Protocol version mismatch on a pipe frame (returns `SCRIBE_EPROTOCOL` instead).
- A hex hash is not exactly 64 lowercase hex characters (returns `SCRIBE_EHASH` instead).

Rejection is terminal for the current batch but not for the session (library form) or the process (pipe form, which exits). In library form the adapter may continue submitting subsequent batches after a rejection.

## 13. The MongoDB adapter

The reference adapter for v1. Ships as part of the Scribe source tree. Links against `libscribe` (library form) and `libmongoc` + `libbson`. If `libmongoc` is absent at build time, the `mongo-watch` subcommand is not built; `scribe` itself still builds.

**Tree shape.** Four levels as in §6:

```
/                                 instance-tree
  <database-name>/                database-tree
    <collection-name>/            collection-tree
      <canonical-_id>             document blob
```

Names at each level: database name and collection name as UTF-8 bytes, compared byte-wise. `_id` in canonical form per §13.2.

### 13.1 BSON ↔ canonical JSON mapping

Mongo stores documents as BSON, which has types JSON cannot represent. The adapter canonicalizes to **MongoDB Extended JSON v2, canonical mode**, with the following table locked down for v1:

| BSON type             | Canonical JSON representation                                   |
|-----------------------|-----------------------------------------------------------------|
| Double                | `{"$numberDouble": "<shortest-round-trip>"}`                    |
| String                | bare JSON string                                                |
| Object / Document     | JSON object (keys sorted per RFC 8785)                          |
| Array                 | JSON array (order preserved)                                    |
| Binary                | `{"$binary": {"base64": "...", "subType": "<2-hex>"}}`          |
| ObjectId              | `{"$oid": "<24-hex>"}`                                          |
| Boolean               | `true` / `false`                                                |
| Date                  | `{"$date": {"$numberLong": "<ms-since-epoch>"}}`                |
| Null                  | `null`                                                          |
| Regex                 | `{"$regularExpression": {"pattern": "...", "options": "..."}}`  |
| JavaScript code       | `{"$code": "..."}`                                              |
| Int32                 | `{"$numberInt": "<decimal>"}`                                   |
| Timestamp (internal)  | `{"$timestamp": {"t": <uint32>, "i": <uint32>}}`                |
| Int64                 | `{"$numberLong": "<decimal>"}`                                  |
| Decimal128            | `{"$numberDecimal": "<decimal>"}`                               |
| MinKey                | `{"$minKey": 1}`                                                |
| MaxKey                | `{"$maxKey": 1}`                                                |
| DBPointer (deprecated)| `{"$dbPointer": {"$ref": "...", "$id": {"$oid": "..."}}}`       |
| Symbol (deprecated)   | `{"$symbol": "..."}`                                            |

The adapter obtains base Extended JSON via `bson_as_canonical_extended_json()`, then applies a key-sort pass (RFC 8785). libbson does not sort keys; the adapter owns this step. Two documents that are byte-identical BSON must canonicalize to byte-identical canonical JSON, tested exhaustively against libbson's own round-trip.

### 13.2 `_id` canonical form and total ordering

MongoDB allows `_id` to be any BSON value. Scribe needs both a canonical string form (for the tree entry's name) and a total ordering.

**Canonical form.** The `_id` is encoded as its canonical JSON representation per §13.1. ObjectId becomes `{"$oid":"507f1f77bcf86cd799439011"}`; a string `_id` becomes `"abc"` including surrounding quotes; an int becomes `{"$numberInt":"42"}`.

**Total ordering.** Tree entries are sorted by the UTF-8 byte sequence of this canonical form. Deterministic within a type (ObjectIds sort by hex, strings lexicographically) and stable across types (all ObjectIds cluster together, all strings cluster together). The ordering does not emulate MongoDB's native BSON comparison order — it only needs to be deterministic, and it is.

### 13.3 Change stream consumption

The adapter opens a cluster-wide change stream via `mongoc_client_watch` with `fullDocument: "updateLookup"` and `fullDocumentBeforeChange: "whenAvailable"`.

Collections observed should have `changeStreamPreAndPostImages` enabled. The adapter logs a warning (once per collection) for collections without it and proceeds using `updateLookup` only; this is correct but pays an extra round-trip per update.

Events are grouped into batches:

- Events within the same transaction (matching `lsid` + `txnNumber`) form one batch.
- Standalone events form single-event batches. Time-window coalescing is configurable (default off for exact semantic mapping).

Each batch produces one Scribe commit with:

- `author` from the change event's `operationDescription.authenticatedUsers` when available, tagged `"source": "db_auth"`; otherwise `{"name": "unknown", "email": "", "source": "unknown"}`.
- `committer` = `{"name": "scribe-mongo", "email": "", "source": "scribe"}`.
- `process.name` = `"mongo-change-stream"`, `version` = Scribe version, `correlation_id` = the change event's `_id._data` (resume token opaque form).
- `timestamp_unix_nanos` = the event's `clusterTime` converted to nanoseconds. `clusterTime` in Mongo has second precision plus a 32-bit increment counter; the adapter packs both by computing `seconds * 10^9 + increment`. This preserves monotonicity within a second without claiming precision it does not have.

**Resume tokens.** Written to `.scribe/adapter-state/mongodb` as a simple text file:

```
resume_token <base64>
last_commit <64-hex>
last_updated <iso8601>
```

Updated via temp-file + atomic rename *after* the corresponding commit has been durably written. Read at startup to resume; absent means bootstrap is required.

**Change event kinds.** The MongoDB change stream emits many event types; see §20 for the per-type reference documents. v1 handles them as follows:

- `insert`, `update`, `replace`, `modify`, `delete` — data mutations; each produces a `scribe_change_event` (insert/update/replace/modify → payload; delete → tombstone). Batched by transaction when applicable.
- `create`, `drop`, `dropDatabase`, `rename` — DDL events. v1 logs these at INFO level and ignores them for commit purposes. Subsequent data events on the recreated/renamed collection will naturally appear at the new path. v2 may promote these to first-class commits.
- `createIndexes`, `dropIndexes`, `refineCollectionShardKey`, `reshardCollection`, `shardCollection` — schema/sharding events. Logged and ignored for commit purposes in v1.
- `invalidate` — the change stream is no longer valid; close and reopen from a new resume token. Logged at WARN level; triggers the reopen.

Application-level author attribution (identifying the service or human that issued the write, not just the DB user) is *Open (v2)*.

### 13.4 Bootstrap

The first commit against an existing Mongo cluster is a full scan.

**Sequence:**

1. Connect. Verify replica set or sharded topology via the `hello` response (change streams require one of these).
2. Open change stream; record starting resume token; close stream.
3. For each database (excluding `admin`, `local`, `config` by default):
    - For each collection:
        - Stream documents via `find()` with `{readConcern: "majority"}`, batch size 1000.
        - Parallel hash: worker pool of `worker_threads` threads (§19.4) converts BSON to canonical JSON, hashes with BLAKE3, emits `(path, blob_hash, blob_bytes)` records to a bounded channel.
        - Main thread drains the channel, writes blobs via `scribe_objects_put`, accumulates `(name, hash)` pairs for the collection-tree.
    - Write collection-tree object.
4. Assemble database-trees, then the instance-tree.
5. Write the initial commit (zero parents) referencing the instance-tree.
6. Reopen the change stream from the saved resume token. Replay events until caught up to the scan-completion time. Each replayed event produces a normal incremental commit.
7. Transition to steady state.

**Memory discipline.** The only bounded-but-large in-memory structure is the accumulated `(name, hash)` list per collection — a few MB per million documents. Document bytes stream through the hasher and object store; they are not retained.

**Resumable bootstrap.** If interrupted, already-hashed blobs are already in the object store (idempotent). On restart the adapter notices no commit exists, restarts the scan, but `scribe_objects_put` short-circuits on already-present hashes, so the re-scan pays disk I/O but not hash or compression cost. Acceptable given bootstrap runs once per store.

## 14. Event queue and backpressure

Between the adapter's event producer and the commit builder's consumer, Scribe maintains a bounded SPSC (single-producer single-consumer) lock-free queue holding up to `event_queue_capacity` batches (default 64, configurable). Both the library-form and pipe-form paths use the same queue.

**Producer side.** The adapter accumulates events into a batch (by transaction boundary, coalesce window, or single-event policy) and attempts to enqueue. If the queue is full, the adapter blocks until the commit builder drains one. In the MongoDB adapter specifically, "blocks" means it stops pulling from the change stream — MongoDB's server-side cursor then pauses, which is the desired backpressure shape.

**Consumer side.** The commit builder drains batches in FIFO order, produces one commit per batch, updates the ref, logs the result, signals the resume-token writer (adapter-specific), and repeats.

**Queue capacity.** Default 64 batches is chosen so that short commit-builder stalls (disk fsync) don't immediately starve the producer. Capacity and batch size together bound worst-case memory use; the adapter must be able to hold all events in a single batch in RAM simultaneously (this is already required since a batch is a single commit).

**Overflow is not silent.** A queue that stays full for longer than `queue_stall_warn_seconds` (default 30) emits a warning to the log. There is no dropping; the system prefers backpressure over data loss.

## 15. Consistency, failure, and locking

The object store is append-only and content-addressed. Writing the same object twice is idempotent and safe. The only mutable files are `refs/heads/main`, `adapter-state/*`, `HEAD` (rarely updated), and the operational `log`. All mutable-file updates except `log` are temp-file + `fsync` + atomic rename.

**Per-commit ordering:** write all new objects and fsync the containing directory; update the ref via compare-and-swap (atomic rename, with the old hash read just before the rename to detect races — v1 is single-writer so races should not occur, but the check is cheap). A crash between object writes and ref update leaves unreferenced objects (harmless; reclaimable by a future `scribe gc`). A crash mid-object-write leaves a partial temp file that is never referenced.

**Locking.** `.scribe/lock` is held with `flock(LOCK_EX | LOCK_NB)` by any process that writes to the store. The lock file's contents are a diagnostic text document rewritten on acquisition:

```
pid <int>
host <hostname>
started_at <iso8601>
command <scribe ... argv>
```

A process that fails to acquire the lock exits with `SCRIBE_ELOCKED` and prints the holder's diagnostic info to stderr. Concurrent *readers* (`scribe log`, `scribe cat-object`, `scribe diff`) do not take the lock and are safe because objects are immutable once written and ref updates are atomic.

**Signal handling.** `SIGTERM` and `SIGINT` trigger a clean shutdown: the main loop sets an atomic `shutdown_requested` flag. The running batch (if any) completes commit and ref update normally; the resume-token writer then persists the new state; only then does the process exit with status 0. In-flight change stream pulls are cancelled at the next iteration boundary (by closing the change stream handle). If a second `SIGTERM`/`SIGINT` arrives before clean shutdown completes, the process exits immediately with `SCRIBE_EINTERRUPTED` (non-zero); this may leave unreferenced objects on disk but cannot corrupt refs or the resume token, since neither has been updated for the in-flight batch.

`SIGHUP` is reserved for log rotation in v2. In v1 it is ignored.

Multi-writer setups are *Open (v2)* and require a distributed ref update mechanism.

## 16. CLI surface

The `scribe` binary is the single entry point. Subcommands:

| Command                                 | Purpose                                                           |
|-----------------------------------------|-------------------------------------------------------------------|
| `scribe init <path>`                    | Create a new `.scribe/` store with a config skeleton              |
| `scribe info`                           | Print version, config, hash algorithm, supported protocol range   |
| `scribe log [--oneline] [-n <N>]`       | Walk commit history from HEAD                                     |
| `scribe show <commit>`                  | Print commit metadata + list of touched paths                     |
| `scribe cat-object (-p\|-t\|-s) <hash>` | Inspect an object: pretty, type, or size                          |
| `scribe diff <commit1> [<commit2>]`     | Diff two commits (default: parent vs. commit1)                    |
| `scribe commit-batch`                   | Pipe-form adapter entry point; reads framed input on stdin        |
| `scribe mongo-watch <uri> [opts]`       | MongoDB adapter entry point (only if built with libmongoc)        |
| `scribe fsck`                           | Verify object store integrity: every referenced object present and hashes match |

Exit codes: 0 success, non-zero values enumerated in §21. Errors are printed to stderr as `scribe: <error-symbol>: <detail>`.

## 17. Configuration

`.scribe/config` is a flat `key = value` text file, one assignment per line, written at `scribe init`, read on every invocation. Lines beginning with `#` are comments. Whitespace around `=` is ignored. Values are UTF-8 strings; numeric fields are decimal integers, booleans are `true` / `false`, lists are comma-separated with no whitespace. Keys are dot-separated namespaces.

v1 schema:

```
scribe_format_version = 1
hash_algorithm = blake3-256
compression = zstd
compression_level = 3
worker_threads = 0
event_queue_capacity = 64
queue_stall_warn_seconds = 30

adapter.name = mongodb
adapter.mongodb.excluded_databases = admin,local,config
adapter.mongodb.require_pre_post_images = false
adapter.mongodb.coalesce_window_ms = 0
```

`worker_threads = 0` means "autodetect: number of physical cores". Unknown keys are rejected at startup (not ignored) to prevent silent misconfiguration. A config file missing any required v1 key is also rejected; defaults apply only where explicitly stated above.

## 18. Logging

Scribe writes two log streams:

**Stderr.** Human-readable one-line-per-event messages for operational visibility when run interactively. Format: `<iso8601-utc> <level> <component> <message>`. Levels: `DEBUG`, `INFO`, `WARN`, `ERROR`. Default level `INFO`, overridable via `SCRIBE_LOG_LEVEL` environment variable.

**`.scribe/log`.** Append-only operational log on disk, same format as stderr. Not rotated in v1; grows unbounded. Entries are flushed after each commit. Not a crash-recovery log — it is purely operational.

Both streams receive the same content. `.scribe/log` exists so that a `scribe` process run from a supervisor (systemd, Docker) that captures stderr separately still has a durable local trace. Programmatic consumption of logs (structured output, centralized log shipping) is *Open (v2)*.

Sensitive data (connection strings with passwords, document contents, resume token bytes beyond a short prefix) is never logged. Identities are logged because they are already recorded in commit objects.

## 19. Implementation in C

### 19.1 Toolchain

- **Language:** C11 (uses `_Static_assert`, anonymous unions, `stdint.h`, `stdatomic.h`, `pthread.h`).
- **Build:** CMake 3.20+. Top-level `CMakeLists.txt` produces `libscribe.so`/`.a` and the `scribe` binary. The MongoDB adapter is conditionally built based on `find_package(libmongoc-1.0)`.
- **Compilers:** GCC 11+ and Clang 14+ on Linux x86_64/aarch64 (primary). macOS Apple Silicon secondary. Windows is *Open (v2)*.
- **Warnings:** `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` by default. Vendored deps excluded from `-Werror`.
- **Sanitizers:** ASan + UBSan in debug builds. TSan for threading work. A dedicated `ci-asan` build runs the full test suite under sanitizers.

### 19.2 Dependencies

Scribe minimizes runtime dependencies deliberately. v1's complete list:

| Library           | Purpose                                  | Source                     | Required?          |
|-------------------|------------------------------------------|----------------------------|--------------------|
| BLAKE3 (C ref)    | Hashing                                  | Vendored (CC0/Apache-2.0)  | Yes                |
| zstd              | Compression                              | Vendored (BSD-3)           | Yes                |
| libmongoc/libbson | MongoDB client                           | System package             | `mongo-watch` only |
| Unity             | Unit test harness                        | Vendored (MIT)             | Dev only           |

No JSON library. No logging framework. No generic data-structure library. Scribe's needs are small enough that the remaining utility code (hash tables for name interning, LEB128 codec, LRU, line-based parsers) is written directly.

### 19.3 Memory model

- **Arena allocators** (`scribe_arena`) for request-scoped work: building a commit, walking a diff, processing a change batch. Arenas are reset per request; no per-allocation `free` on the hot path.
- **Long-lived state** (the open object store, configuration) uses `malloc`/`free` at startup/shutdown only.
- **Explicit ownership documented per function.** Every function comment specifies whether returned buffers are caller-owned, arena-owned, or borrowed.
- **No hidden allocations** on the hot write path: per-commit allocation is computed upfront from `event_count` and drawn from a single arena.

### 19.4 Threading

- **Single-writer invariant** enforced by `.scribe/lock`. The writing process may internally parallelize.
- **Main thread** runs the adapter loop (change stream consumption or stdin reading) and commit construction.
- **Hash worker pool** (`worker_threads` threads) used during bootstrap and large batch processing. Each worker pulls from a work queue, canonicalizes, hashes, emits to an SPSC lock-free ring buffer. The main thread drains buffers round-robin.
- **No shared mutable state on the hot path.** The arena-per-request model means workers operate on disjoint memory. Atomics (`stdatomic.h`) are used only for the shutdown flag and SPSC queue indices.

### 19.5 Performance tactics

**Hashing (§4).** BLAKE3 reference C implementation with runtime SIMD dispatch (AVX-512 → AVX2 → SSE4.1 → portable). Hash state is reused across chunks during streaming blob ingestion.

**BSON → canonical JSON (§13.1).** Works on libbson's iterator without materializing an intermediate BSON tree. Key-sort pass uses a stack-allocated pointer array when field count is small (≤32), heap otherwise, single arena allocation.

**Tree serialization (§3).** Binary format. A tree object's serialized size is known exactly from the entry list. Serialize into a single arena-allocated buffer; no intermediate representation.

**Compression (§3).** zstd level 3 for loose objects (fast, good ratio). `ZSTD_CCtx` reused across writes in the same session to avoid setup cost.

**Storage (§8).** `O_TMPFILE` + `linkat` on Linux for atomic creation without temp-file churn; portable fallback is `open(O_CREAT|O_EXCL)` with a temp name + `rename`. Batched `fsync`: all objects in a commit are written, then a single `fsync` on the parent directory, then the ref update with its own `fsync`. Safe because content-addressed objects never race.

**Bootstrap (§13.4).** Each collection is independent; distributed across the hash worker pool with no inter-worker coordination. Final tree assembly is single-threaded but trivial.

**Tree rebuild on commit (§10).** Only the changed path's trees are rebuilt. Unchanged sibling entries keep their existing hashes verbatim. For N changed leaves across K paths, new tree count is bounded by the depth-sum of K distinct paths.

**Diff (§11).** Subtree skip on hash-equal is the whole game. No byte comparison, no content scan. A 1M-document collection with one changed document produces one tree walk along a single root-to-leaf spine.

**String handling.** Tree entry names are length-prefixed UTF-8, never null-terminated internally. Frequently-occurring path components (database/collection names) are interned in a small open-addressed hash table for the duration of a batch.

**I/O buffering.** Sequential writes use `setvbuf` with 1 MB buffers. Loose-object reads use a single `read` into an arena buffer sized by one `stat` call.

## 20. Reference documentation

The repository includes a `docs/` directory containing authoritative reference material that the implementer should consult during development. These documents take precedence over any training-data recollection about the same topics. The agent must read each relevant file before implementing code that touches its subject.

| File                                          | Subject                                        | Used by                       |
|-----------------------------------------------|------------------------------------------------|-------------------------------|
| `blake.md`                                    | BLAKE3 hash specification                      | §4, §19.5                     |
| `RFC_8785_canonicalization_scheme.md`         | JSON Canonicalization Scheme                   | §5, §13.1                     |
| `mongodb_change_streams.md`                   | Change Streams API and semantics               | §13.3                         |
| `mongodb_production_reccomandations.md`       | MongoDB deployment recommendations             | §13, §13.4                    |
| `change_events.md`                            | Change event schema index                      | §13.3                         |
| `mongo_insert_event.md`                       | `insert` change event payload                  | §13.3                         |
| `mongo_update_event.md`                       | `update` change event payload                  | §13.3                         |
| `mongo_replace_event.md`                      | `replace` change event payload                 | §13.3                         |
| `mongo_modify_event.md`                       | `modify` change event payload                  | §13.3                         |
| `mongo_delete_event.md`                       | `delete` change event payload                  | §13.3                         |
| `mongo_create_event.md`                       | `create` change event payload                  | §13.3                         |
| `mongo_drop_event.md`                         | `drop` change event payload                    | §13.3                         |
| `mongo_drop_database_event.md`                | `dropDatabase` change event payload            | §13.3                         |
| `mongo_rename_event.md`                       | `rename` change event payload                  | §13.3                         |
| `mongo_invalidate_event.md`                   | `invalidate` change event payload              | §13.3                         |
| `mongo_create_indexes.md`                     | `createIndexes` change event payload           | §13.3                         |
| `mongo_drop_indexes.md`                       | `dropIndexes` change event payload             | §13.3                         |
| `mongo_refine_collection_shard_key_event.md`  | `refineCollectionShardKey` event payload       | §13.3                         |
| `mongo_reshard_collection.md`                 | `reshardCollection` event payload              | §13.3                         |
| `shard_collection.md`                         | Shard collection semantics                     | §13.3                         |

## 21. Error codes

Every Scribe error is one of the following `scribe_error_t` values. Exit codes and stderr output use the symbolic name; structured APIs use the integer. New codes may be added in future versions but existing codes and values are stable.

```c
typedef enum {
    SCRIBE_OK                = 0,   /* success                                 */
    SCRIBE_ERR               = 1,   /* generic failure, detail in message      */
    SCRIBE_ENOT_FOUND        = 2,   /* object, ref, or file not present        */
    SCRIBE_EEXISTS           = 3,   /* creation of something that exists       */
    SCRIBE_ELOCKED           = 4,   /* .scribe/lock held by another process    */
    SCRIBE_EREF_STALE        = 5,   /* ref CAS expected_old mismatch           */
    SCRIBE_EIO               = 6,   /* underlying I/O failure                  */
    SCRIBE_ECORRUPT          = 7,   /* hash mismatch, bad envelope, truncated  */
    SCRIBE_EINVAL            = 8,   /* invalid argument (programming error)    */
    SCRIBE_EMALFORMED        = 9,   /* invalid input from adapter or pipe      */
    SCRIBE_EPROTOCOL         = 10,  /* pipe protocol version or framing error  */
    SCRIBE_ECONFIG           = 11,  /* bad config file: unknown key, type, etc */
    SCRIBE_EADAPTER          = 12,  /* adapter-level failure; wraps inner err  */
    SCRIBE_ENOMEM            = 13,  /* allocation failure                      */
    SCRIBE_ENOSYS            = 14,  /* unsupported operation, e.g. hash algo   */
    SCRIBE_EINTERRUPTED      = 15,  /* signal received, clean shutdown asked   */
    SCRIBE_EPATH             = 16,  /* invalid path: empty, zero-length, etc.  */
    SCRIBE_EHASH             = 17,  /* hash format invalid or algo unsupported */
    SCRIBE_ECONCURRENT       = 18,  /* concurrent modification detected        */
    SCRIBE_ESHUTDOWN         = 19,  /* operation rejected: shutdown in progress*/
} scribe_error_t;
```

Every public `scribe_*` function that can fail returns `scribe_error_t`. Output parameters are populated only on `SCRIBE_OK`. A thread-local `scribe_last_error_detail()` function returns a human-readable message for the most recent error on the calling thread.

## 22. Build system and installation

Scribe ships a complete CMake-based build that produces the `scribe` binary, the `libscribe` library, and the test suite with a single command. Vendored dependencies are checked out as git submodules under `vendor/`. The only system dependency is `libmongoc` / `libbson` for the MongoDB adapter, and the build gracefully degrades if it is absent.

### 22.1 Source tree layout

```
scribe/
  CMakeLists.txt                     # top-level build
  cmake/                             # helper modules: FindBLAKE3.cmake, etc.
  src/
    core/                            # object store, refs, commit builder, diff
    adapter_mongo/                   # MongoDB adapter (conditionally built)
    cli/                             # main.c and subcommand dispatch
    util/                            # arena, leb128, hex, hashtable, logging
  include/
    scribe/                          # public headers for libscribe
  vendor/
    blake3/                          # git submodule: BLAKE3 reference C
    zstd/                            # git submodule: facebook/zstd
    unity/                           # git submodule: ThrowTheSwitch/Unity
  tests/
    unit/                            # Unity-based unit tests
    integration/                     # end-to-end tests against Mongo
  scripts/
    install-deps-ubuntu.sh           # apt-based libmongoc install
    install-deps-macos.sh            # brew-based libmongoc install
    smoke-test.sh                    # build + bring up Mongo + run adapter
  docker/
    docker-compose.yml               # single-node replica set for testing
    mongo-init.js                    # rs.initiate() + seed data
  docs/
    *.md                             # reference material from §20
    MANUAL.md                        # the user manual, see §23
  DESIGN.md                          # this document
  README.md                          # project intro + pointer to MANUAL.md
  LICENSE
```

### 22.2 CMake targets and options

Top-level `CMakeLists.txt` declares these targets:

| Target                  | Kind              | Contents                                                 |
|-------------------------|-------------------|----------------------------------------------------------|
| `scribe_core`           | static lib        | `src/core/*` + `src/util/*` + vendored BLAKE3 + zstd     |
| `scribe`                | shared lib + PIC  | Public `libscribe.so` wrapping `scribe_core`             |
| `scribe_cli`            | executable        | `src/cli/*`; final binary is named `scribe`              |
| `scribe_mongo_adapter`  | static lib        | `src/adapter_mongo/*`; built only if libmongoc is found  |
| `scribe_tests`          | executable        | Unity test runner linking `scribe_core` and all tests    |

Build options (all default `ON` unless noted):

```
-DSCRIBE_BUILD_MONGO_ADAPTER=ON     # auto-disabled if libmongoc missing
-DSCRIBE_BUILD_TESTS=ON
-DSCRIBE_ENABLE_ASAN=OFF            # debug builds only; ON flips UBSan too
-DSCRIBE_ENABLE_TSAN=OFF            # mutually exclusive with ASan
-DSCRIBE_VENDORED_ZSTD=ON           # OFF uses system zstd if available
-DCMAKE_BUILD_TYPE=Release          # Release | Debug | RelWithDebInfo
```

When `SCRIBE_BUILD_MONGO_ADAPTER=ON` but `find_package(libmongoc-1.0)` fails, CMake emits a clear warning, disables the flag, and continues — the final `scribe` binary lacks `mongo-watch` but everything else builds. This is intentional: contributors without Mongo installed can still hack on the core.

### 22.3 Building from source

The happy path for a first-time builder:

```bash
git clone https://github.com/<org>/scribe.git
cd scribe
git submodule update --init --recursive        # BLAKE3, zstd, Unity
./scripts/install-deps-ubuntu.sh                # or install-deps-macos.sh

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # optional: run tests
sudo cmake --install build                      # installs to /usr/local
```

The install step places `scribe` under `bin/`, `libscribe.so` under `lib/`, and headers under `include/scribe/`. `DESTDIR` is respected for packaging workflows.

`scripts/install-deps-ubuntu.sh` runs (non-interactively, with `sudo` if needed):

```
apt-get update
apt-get install -y build-essential cmake pkg-config git libmongoc-dev libbson-dev
```

`scripts/install-deps-macos.sh` runs:

```
brew install cmake pkg-config mongo-c-driver
```

Both scripts check for already-installed versions and skip the install step cleanly. Other distributions are *Open (v2)*; the agent should make it obvious to contributors how to add one (a small shell script per distro in `scripts/`).

### 22.4 Local MongoDB for testing

`docker/docker-compose.yml` defines a single-node MongoDB replica set suitable for exercising change streams — a replica set is mandatory, so a plain `mongo:latest` container is not sufficient. The compose file:

```yaml
services:
  mongo:
    image: mongo:7
    command: ["mongod", "--replSet", "scribe-rs", "--bind_ip_all", "--port", "27017"]
    ports:
      - "27017:27017"
    healthcheck:
      test: ["CMD", "mongosh", "--quiet", "--eval", "db.runCommand({ping: 1}).ok"]
      interval: 2s
      timeout: 5s
      retries: 20
    volumes:
      - ./mongo-init.js:/docker-entrypoint-initdb.d/mongo-init.js:ro
```

`mongo-init.js` runs `rs.initiate()` on first startup, waits for `PRIMARY`, creates a `scribe_test` database with a seed collection, and enables `changeStreamPreAndPostImages` on that collection.

### 22.5 Smoke test

`scripts/smoke-test.sh` is the end-to-end "does it actually work" check. It is the first thing the agent runs after building, and the first thing a new contributor runs:

```
1. docker compose -f docker/docker-compose.yml up -d --wait
2. rm -rf /tmp/scribe-smoke && mkdir -p /tmp/scribe-smoke
3. ./build/scribe init /tmp/scribe-smoke/.scribe
4. ./build/scribe mongo-watch "mongodb://localhost:27017/?replicaSet=scribe-rs" \
       --store /tmp/scribe-smoke/.scribe &
   SCRIBE_PID=$!
5. sleep 2   # let bootstrap complete
6. mongosh "mongodb://localhost:27017/scribe_test?replicaSet=scribe-rs" \
       --eval 'db.users.insertOne({_id: "alice", role: "admin"})'
7. sleep 1   # let change stream propagate
8. kill -TERM $SCRIBE_PID && wait $SCRIBE_PID
9. ./build/scribe --store /tmp/scribe-smoke/.scribe log
10. ./build/scribe --store /tmp/scribe-smoke/.scribe diff HEAD~1 HEAD
11. docker compose -f docker/docker-compose.yml down
```

Expected output: at least two commits (one for bootstrap, one for the insert), and the diff shows `scribe_test/users/"alice"` added. The script exits non-zero if any step fails; if all pass, it prints `SMOKE TEST: PASSED`. This is the minimum correctness bar for v1; passing it is the acceptance criterion for the agent's work.

### 22.6 Continuous integration

A GitHub Actions workflow (`.github/workflows/ci.yml`) runs on every push:

- **build-linux**: Ubuntu 22.04 and 24.04, GCC and Clang, Release and Debug, with and without the Mongo adapter. Runs `ctest`.
- **sanitizers**: Debug + ASan/UBSan, Debug + TSan. Runs `ctest`.
- **smoke**: brings up `docker-compose.yml`, runs `scripts/smoke-test.sh`.
- **format**: `clang-format --dry-run --Werror` over `src/` and `include/`.

Failing any of the above blocks merge.

## 23. User manual

The agent produces `docs/MANUAL.md` as part of the v1 deliverable. The manual is the operator-facing companion to this design document: whereas `DESIGN.md` tells the implementer *how Scribe is built*, `MANUAL.md` tells the user *how to use it*. The two documents must stay consistent; when they disagree, `DESIGN.md` wins and `MANUAL.md` is corrected.

### 23.1 Audience and tone

The manual targets engineers running Scribe against a real data store — their own development machine or a staging cluster, and eventually production. It assumes UNIX literacy, general database familiarity, and basic MongoDB knowledge. It does not assume prior exposure to Git's internals, but it freely uses Git as an analogy when the analogy is accurate.

Tone is operational reference, not marketing. No "blazing fast," no "enterprise-grade," no hedging. Every claim is falsifiable; every command in the manual has been run by the author and its output is reproduced verbatim. Screenshots are not used; terminal transcripts are.

### 23.2 Required structure

The manual contains the following top-level sections in this order:

1. **Overview.** One page. What Scribe is, what it isn't, the Git analogy made precise, the one-sentence statement of value. Points to `DESIGN.md` for internals and to the other sections for how-to.

2. **Installation.** Mirrors §22.3. Distinct subsections for Ubuntu, Debian, macOS, "build from source on other systems," and "verifying the installation" (runs `scribe info` and shows expected output).

3. **Quickstart.** A complete, copy-pasteable session: start the docker-compose MongoDB, run `scribe init`, run `scribe mongo-watch`, make a write in another terminal, stop the watcher, run `scribe log`, run `scribe diff`, run `scribe cat-object -p <hash>`. Shows real terminal output. This is the section a new user reads first; it must work on a fresh machine in under five minutes.

4. **The `.scribe/` repository.** What each file and directory is (mirrors §7 but user-facing). How to inspect the object store manually. How content addressing makes deduplication automatic. How to back up a Scribe repository (answer: `tar` or `rsync`; it's immutable).

5. **Command reference.** Every subcommand from §16, one subsection each, with synopsis, description, all flags documented, example invocation, and example output. Commands are in alphabetical order within this section for lookup. The tone here is `man`-page-like.

6. **The MongoDB adapter.** How to configure and run `scribe mongo-watch`. Connection URI format and required replica set / sharding topology. Pre- and post-image configuration (what it is, how to enable it in Mongo, what happens without it). Excluded databases and how to override. Resume semantics: what happens on restart, what happens if the resume token is invalid, how bootstrap works, how long bootstrap takes on a cluster of size N. Change event types and what v1 does with each (links to §13.3).

7. **Operations.** Running Scribe under systemd (a sample unit file). Running Scribe in Docker (a sample Dockerfile and compose entry). Signal handling in practice — what `SIGTERM` does, how to verify clean shutdown, what to do if Scribe is unkillable. Disk sizing: "expect roughly 4 KB per unique document plus 4 KB per commit on ext4; for a 1M-document collection churning 10K documents/day, expect X GB/month." Locking: symptoms of a stale lock, how to recover. Log rotation (manual in v1).

8. **Troubleshooting.** Every error code from §21, one subsection per code, with: likely causes, how to diagnose, how to fix. Plus a top-level FAQ covering "why is Scribe using so much disk" (pack files are v2), "why didn't my write show up" (most likely: no replica set, or post-images disabled), "can I replay history" (no, v1 doesn't restore), and similar recurring questions.

9. **Glossary.** Commit, tree, blob, leaf, path, adapter, canonical form, bootstrap, resume token, tombstone. Each a paragraph, with cross-references to `DESIGN.md` for the full treatment.

### 23.3 Validation

Before the manual is considered complete, every shell command in it is executed verbatim in a clean environment and the output is captured and compared. Commands that produce output that varies between runs (timestamps, hashes) use stable placeholder conventions (`<64-hex>`, `<iso8601>`) that are clearly marked as placeholders.

The `Quickstart` section in particular is executed end-to-end on a fresh Ubuntu 22.04 VM as part of the release checklist. If it fails to run cleanly, the release is blocked until either the manual or the code is fixed.

### 23.4 Out of scope for v1 manual

Performance tuning guides, capacity planning spreadsheets, high-availability deployment patterns, migration guides from similar tools, API reference for `libscribe` (the C header is its own reference in v1), and internationalization. These are *Open (v2)* for the manual.

## 24. Non-goals for v1

No pack files (loose objects only). No data restore. No branches, merges, tags, or reflog. No encryption at rest. No query language beyond commit-log traversal and tree diff. No application-level identity injection in the MongoDB adapter. No field-granularity document subtrees. No staging area. No Windows support. No daemon mode. No multi-writer coordination. No garbage collection (`scribe gc` arrives with pack files in v2). No log rotation. No first-class DDL events. No structured log output.

## 25. Open questions (v2)

Pack files with delta compression. Field-granularity document subtrees. Branches and merges with defined semantics. Restore-to-commit as a first-class operation. Distributed multi-writer refs. Application-level author injection through driver wrappers. Richer ref types (tags, remotes). Query surface over history. Daemon mode with multiple concurrent adapter sessions. Windows support. S3-backed object store. Single-file bundle format for export/import. DDL events as first-class commits. Structured log output. `scribe gc` for unreferenced loose objects.

---

*End of v1 design.*