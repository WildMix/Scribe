# Scribe — Design (v1)

**Status:** frozen for v1 implementation. Decisions here are intentionally narrow so that an implementer (human or agentic) can execute against them without having to invent policy. Items marked *Open (v2)* are explicitly out of scope for v1.

---

## 1. Scope

Scribe records the evolution of a data store as a chain of content-addressed snapshots, using the same object model as Git (blobs, trees, commits, refs), stored on disk in the same layout (`.scribe/objects/`, `.scribe/refs/`, `.scribe/HEAD`). The snapshots describe *data*, not files; the analogy to Git is structural, not literal.

Scribe is not a database, does not serve reads of live data, does not restore data, and does not intercept writes by default. It is an observer that produces a verifiable, traversable history of whatever data store it is pointed at. Restoring a store from a Scribe history is possible in principle but is out of scope for v1.

## 2. Core model: state, not change

A Scribe history is a chain of full state snapshots. Each snapshot is a Merkle tree whose root hash uniquely identifies the state of the observed data store at a point in time. A *change* is not a first-class object in Scribe; a change is the result of diffing two snapshot trees by descending only where hashes diverge. This mirrors Git exactly: Git stores snapshots, not patches, and derives diffs on demand.

The consequence is important. Scribe never stores "field X was changed to Y" as an object. It stores "the document containing field X now hashes to H2 instead of H1", and when asked what changed, walks both trees from the root down until it reaches the diverging leaves.

## 3. Object types

Scribe has three object types, matching Git.

A **blob** is an opaque byte sequence representing a single piece of data (for v1, one whole document in its canonical serialization — see §5). Its identity is the hash of its contents.

A **tree** is an ordered list of entries, where each entry is a `(name, type, hash)` triple. `type` is one of `blob` or `tree`. Entries within a tree are sorted by `name` under a defined comparator so that the tree's serialization is deterministic. A tree's identity is the hash of this serialization. Trees are the structural backbone: every internal node in the Merkle hierarchy is a tree, regardless of what semantic level it represents (see §6).

A **commit** is a structured object referencing a single root tree, zero or more parent commits (v1: exactly zero for the initial commit, exactly one for all subsequent commits — see §9), an author, a committer, a timestamp, and an optional message. Its identity is the hash of its serialization. Commits are the only objects that carry human/process attribution; trees and blobs are pure data.

The v1 serialization of each object type (canonical JSON, UTF-8, LF-terminated, no trailing whitespace):

```
blob   : the raw bytes of the serialized document, unwrapped
tree   : {"entries": [{"name": str, "type": "blob"|"tree", "hash": hex}, ...]}
         entries sorted lexicographically by name (byte-wise, UTF-8)
commit : {"tree": hex, "parents": [hex, ...], "author": {...},
          "committer": {...}, "timestamp": iso8601_utc, "message": str}
```

The wire format for objects on disk is `<type> <length>\0<payload>`, zlib-compressed, matching Git's object format. This is chosen for familiarity and because tooling (e.g., `git cat-file`-style inspection) ports trivially.

## 4. Content addressing

Every object is stored at `.scribe/objects/<first two hex chars of hash>/<remaining hex chars>`. The hash algorithm for v1 is **BLAKE3** (256-bit output, hex-encoded). BLAKE3 is chosen over SHA-256 for throughput on the write path; the format is designed so that a future migration to a different hash is a one-time rewrite of the object store rather than a protocol change. The algorithm identifier is recorded in `.scribe/config` so that readers can reject stores written under a hash they do not support.

Deduplication is automatic and pervasive. Any subtree whose contents are identical to a previously written subtree resolves to the same hash and is stored once. In practice, most of the tree between two adjacent commits is shared, because most of the observed data is unchanged between any two observations.

## 5. Canonicalization

Before a document becomes a blob, it is canonicalized. For v1, canonicalization follows **RFC 8785 (JSON Canonicalization Scheme)**: object keys sorted lexicographically, no insignificant whitespace, numbers normalized to the shortest ECMAScript round-trip form, strings in minimal-escape UTF-8. Adapters that observe non-JSON data are responsible for converting to canonical JSON before handing a document to Scribe, or for defining their own canonicalization in a documented extension.

Canonicalization is the single source of truth for document identity. Two documents that differ only in key order, whitespace, or equivalent number representations must produce the same blob hash. Getting this wrong produces spurious changes in the history; getting it right is mandatory.

## 6. The tree shape is adapter-defined

This is the most important design point in the document, and the one most likely to be misread.

Scribe's object model (blob, tree, commit) is fixed. The *shape* of the Merkle tree — how many levels it has, what each level represents, and how a child's `name` is computed — is defined by the adapter for a given data store. Scribe the engine does not know what a "database" or a "collection" is. It knows trees and blobs.

For a MongoDB instance, a natural and recommended tree shape is four levels deep:

```
commit ──► instance-tree ──► database-tree ──► collection-tree ──► document (blob)
           (entries:          (entries:          (entries:
            db names)          collection names)  _id values)
```

The instance-tree's entries are named by database name. Each database-tree's entries are named by collection name. Each collection-tree's entries are named by the document's `_id`, rendered under a defined ordering for mixed BSON `_id` types. Each leaf is the canonical JSON of a document. This shape gives per-database, per-collection, and per-document dedup for free, and it matches how MongoDB operators think about their data.

It is not the only valid shape. A relational adapter might produce `server ──► database ──► schema ──► table ──► row (keyed by primary key)`. A key-value adapter might produce `root ──► shard ──► key (blob)`. A filesystem-like adapter might produce arbitrary-depth trees mirroring directory structure. An in-memory object graph adapter might produce a tree whose leaves are individual objects keyed by stable identity.

The rules an adapter must satisfy are: every internal node is a tree object as defined in §3, every leaf is a blob object as defined in §3, every entry has a stable `name` under a documented comparator so that tree hashes are deterministic, and the same logical entity (same row, same document, same object) must always appear at the same path under the same name across commits. Violating the last rule breaks incremental commit construction (§9) and diff (§10).

For v1, leaves are always whole-document blobs. Splitting a document into a subtree of fields ("document-as-tree") is a recognized v2 extension; the object model already supports it without protocol changes, because a tree can contain a tree.

## 7. Repository layout on disk

```
.scribe/
  HEAD                    # ref: refs/heads/main  (symbolic ref, text)
  config                  # hash algorithm, adapter name, adapter config
  objects/
    <xx>/<rest-of-hash>   # zlib-compressed object, type+length+payload
    ...
  refs/
    heads/
      main                # 64 hex chars: commit hash of tip
```

The layout is deliberately identical in spirit to `.git/`. A v1 implementation does not implement pack files; every object is a loose file. Packing is a v2 optimization and does not change the protocol.

`.scribe/` does not need to live next to the observed data store. For MongoDB, the observer process can run on any host that can reach the cluster, and `.scribe/` lives on that host. For multi-observer or durable-history deployments, `.scribe/objects/` can be backed by an object store (S3, MinIO) with a small local cache; the object model is append-only and content-addressed, so cache invalidation is a non-issue.

## 8. References

v1 supports a single branch named `main` and a single ref file `refs/heads/main` pointing at the tip commit. `HEAD` is a symbolic ref pointing at `refs/heads/main`. Updating the tip is an atomic rename of a temp file within `.scribe/refs/heads/`.

Tags, multiple branches, remote refs, and the reflog are *Open (v2)*.

## 9. Commits are produced incrementally

When the adapter reports that a single leaf has changed (for MongoDB: one document upserted or deleted in one collection in one database), the commit builder does O(tree depth) work, not O(data size):

it writes the new leaf blob; it rebuilds the containing tree by copying the old tree's entry list and replacing the one entry; it rebuilds that tree's parent by the same procedure, and so on up to the root; it writes a new commit referencing the new root tree and the previous commit as its parent; it atomically updates `refs/heads/main` to point at the new commit.

For a MongoDB instance with four tree levels, a single-document change produces exactly five new objects (one blob, three trees, one commit) regardless of how large the overall dataset is. Every other object in the new commit's tree is a reused pointer to an existing object in `.scribe/objects/`.

When the adapter reports a batch of changes that logically belong together (for MongoDB: all document mutations within a single multi-document transaction, grouped by `lsid` + `txnNumber`), the commit builder applies all leaf changes before rebuilding the parent trees, so that a transaction produces exactly one commit, not one per document.

v1 commits always have exactly one parent, except the very first commit in a history, which has zero parents. Merge commits are *Open (v2)* and have no defined semantics in v1, because there is no operation that can produce them (no branches).

## 10. Diffs

A diff between two commits is computed by walking their root trees in lockstep. At each tree, entries are compared by name; entries whose hashes match on both sides are skipped entirely (this is where the dedup pays off at read time, not just at write time). Entries that differ are descended into if both sides are trees, or reported as a blob change if they are leaves. Entries present on only one side are reported as adds or deletes.

The diff surface exposed to users (CLI, API) is therefore naturally scoped: show all changes, or scope to a prefix within the tree (e.g., "show changes under `db1/users/`"). No diff-specific object type exists in the store; diffs are always computed.

## 11. The adapter interface

Adapters are how Scribe stays data-store-agnostic. An adapter is a process (or library) that converts observations of a live data store into leaf-level change events, and hands those events to the Scribe commit builder. The interface has two sides.

On the adapter side, the adapter is responsible for detecting changes in its data store, for producing a canonical blob for each changed leaf, and for reporting the *path* at which that leaf lives in the tree as an ordered list of names (e.g., `["db1", "users", "507f1f77bcf86cd799439011"]`). The adapter is also responsible for grouping related changes into transaction boundaries when the underlying store exposes them.

On the Scribe side, the commit builder accepts a batch of `(path, new_blob | tombstone)` entries plus author/committer/process metadata and produces exactly one commit. The commit builder does not know what the paths mean; it only knows that a path of length *N* implies a tree of depth *N* and a blob leaf at the bottom.

The interface boundary is deliberately narrow:

```
ChangeEvent:
  path:       [str, ...]       # ordered names from root to leaf
  payload:    bytes | TOMBSTONE # canonical blob, or deletion marker
ChangeBatch:
  events:     [ChangeEvent, ...]
  author:     Identity
  committer:  Identity
  process:    ProcessInfo       # name, version, params, correlation_id
  message:    str | None
```

Identity resolution (who the author is) is the adapter's responsibility. The Scribe commit builder never fills in `"unknown"` on its own; if the adapter cannot supply an identity, it must explicitly tag the event as unattributed, and the commit builder records that verbatim.

## 12. The MongoDB adapter (worked example)

The MongoDB adapter is the reference adapter for v1. It exists to prove the generic model against a real system and to exercise the full set of design decisions above.

The adapter consumes MongoDB **Change Streams** cluster-wide. Change Streams require a replica set or sharded cluster. The adapter opens a change stream with `fullDocument: "updateLookup"` and requires `changeStreamPreAndPostImages` enabled on observed collections; without post-images, the adapter cannot produce a correct post-change blob for update events without an extra read. Resume tokens are persisted inside the Scribe object store itself (as a small JSON blob under a known well-formed path, or in `.scribe/config`) so that the adapter can resume cleanly after restart.

The adapter's tree shape is the four-level shape described in §6. Names at each level are: database name (UTF-8 byte-sorted), collection name (UTF-8 byte-sorted), and `_id` rendered in a canonical form that defines a total order across the BSON types that can appear as `_id` (ObjectId, string, int, long, UUID, compound). The canonical `_id` rendering is the single most error-prone piece of the adapter and has its own test suite.

Transactions are grouped by `lsid` + `txnNumber` and produce one commit per transaction. Non-transactional writes produce one commit per event; batching policy (coalescing N events within a time window) is a configuration setting with a default of "no coalescing" for v1 to keep the semantic mapping exact.

The adapter does *not* know the application-level author of a write. MongoDB Change Streams expose the authenticated database user (if auditing is on), not the human or service who issued the write. v1 records this as the author and tags commits as `author_source: "db_auth"` so that consumers know the attribution is at the database-connection level, not the application level. A richer attribution path (e.g., a driver wrapper that injects application-level identity into the write via an unused field and the adapter strips it back out on observation) is *Open (v2)*.

## 13. Bootstrapping

The first commit against an existing data store is a full scan. For MongoDB this means iterating every document in every collection in every database, hashing each as a blob, and assembling the full tree bottom-up. The scan can be hours on a large cluster.

v1 strategy: open the change stream first and record the starting resume token, then scan from a `majority`-read snapshot of the cluster, then replay change stream events from the recorded resume token up to the scan completion time to catch mutations that occurred during the scan. The first commit is written with zero parents once the replay is consistent. This is the one place where Scribe temporarily holds in-memory state larger than a single commit's worth of new objects; it is acceptable because bootstrapping is a one-time operation per data store.

## 14. Consistency and failure

The object store is append-only and content-addressed; writing the same object twice is idempotent and safe. The only mutable file is `.scribe/refs/heads/main`, which is updated by atomic rename. The ordering constraint is: write all new objects, fsync, then update the ref. A crash after objects are written but before the ref is updated leaves unreferenced objects in the store (harmless; garbage-collectable) and the history tip unchanged. A crash mid-object-write leaves a partial file under a temp name that is never referenced.

Concurrent writers to the same `.scribe/` are *not* supported in v1. The adapter process is expected to be the single writer. Multi-writer setups are *Open (v2)* and require a distributed ref update mechanism.

## 15. Non-goals for v1

No data restore. No branches. No merges. No tags. No reflog. No garbage collection (unreferenced objects stay on disk until an external tool is run). No pack files. No encryption at rest (trust the filesystem / object store). No query language beyond commit-log traversal and tree diff. No application-level identity injection in the MongoDB adapter. No field-granularity document subtrees. No staging area — writes to the observed store are committed the moment they happen, and Scribe reflects that; there is no index.

## 16. Open questions (v2)

Field-granularity document subtrees. Branches and merges with defined semantics. Restore-to-commit as a first-class operation. Distributed multi-writer refs. Pack files for object store compaction. Application-level author injection through driver wrappers. Richer ref types (tags, remotes). A query surface over history ("show all commits that touched path X by author Y").

---

*End of v1 design.*
