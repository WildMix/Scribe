# Scribe v1 C Implementation Plan

## Summary

Implement Scribe from DESIGN.md as a Linux/WSL Ubuntu C11 project, using CMake and GCC. The repo is currently documentation-only, so the first milestone creates the source tree, build system, core library, CLI, and tests. The MongoDB adapter is a second milestone after the core is stable.

Use WSL Ubuntu as the supported dev/test environment. Native Windows support stays out of scope for v1, matching DESIGN.md.

## Environment And Dependencies

- Use wsl -d Ubuntu for all builds and tests.
- Already present in WSL: GCC 13.3, CMake, Git, pkg-config, Docker.
- Core milestone needs no extra system packages.
- Mongo milestone will require:

  sudo apt-get update
  sudo apt-get install -y libmongoc-dev libbson-dev
- Dependencies under vendor/ are git submodules, matching DESIGN.md sections 22.1 and 22.3:
    - vendor/blake3/: upstream BLAKE3 C reference implementation.
    - vendor/zstd/: facebook/zstd.
    - vendor/unity/: ThrowTheSwitch/Unity.
- Do not use vendored tarballs or CMake FetchContent for these dependencies.
- First-time setup initializes them with:

  ```bash
  git submodule update --init --recursive
  ```

- CMake builds from the checked-out submodule directories and fails clearly if a required submodule is missing; it must not download dependencies during configure or build.

## Milestone 1: Core Scribe

- Create the designed layout: CMakeLists.txt, src/core, src/util, src/cli, include/scribe, tests/unit, vendor.
- Build targets:
    - scribe_core: static library with object model, storage, refs, commits, diff, config, logging, utilities.
    - scribe: shared library exposing public scribe_* APIs.
    - scribe_cli: executable named scribe.
    - scribe_tests: Unity unit test runner.
- Public API:
    - Define scribe_error_t exactly as in section 21.
    - Define scribe_identity, scribe_process_info, scribe_change_event, scribe_change_batch.
    - Implement scribe_commit_batch.
    - Add scribe_last_error_detail() as thread-local diagnostic state.
- Threading scope:
    - Keep Milestone 1 single-threaded: commit construction, pipe commit-batch, diff, fsck, and CLI inspection commands run on the main thread.
    - Add the core SPSC queue API and tests in Milestone 1, but do not introduce the hash worker pool until Mongo bootstrap in Milestone 2.
- Core formats:
    - Implement unsigned LEB128 encode/decode with corrupt/truncated checks.
    - Implement tree binary serialization sorted by byte-wise UTF-8 name.
    - Implement commit text serialization exactly as specified.
    - Implement object envelope: <type-byte><uncompressed-length:LEB128><payload>, hash over uncompressed envelope, zstd-compressed on disk.
    - Use BLAKE3-256 hashes and lowercase 64-hex rendering/parsing.
- Memory and ownership:
    - Implement scribe_arena in src/util and use it for request-scoped work such as commit construction, diff walking, tree serialization, and pipe batch parsing.
    - Keep long-lived state such as open stores and configuration on malloc/free with explicit shutdown ownership.
    - Document ownership for every public function: caller-owned, arena-owned, or borrowed.
    - Avoid hidden per-object allocations on the hot write path; compute per-batch arena needs where practical and reset arenas after each request.
- Queue and backpressure:
    - Implement the bounded SPSC batch queue from DESIGN.md section 14 in core utilities.
    - Use the same queue abstraction for pipe-form commit-batch and later Mongo adapter ingestion.
    - Make producers block when the queue is full; do not drop batches.
    - Emit a warning if the queue remains full longer than queue_stall_warn_seconds.
- Filesystem store:
    - Implement .scribe/objects/xx/rest-of-hash loose object storage.
    - Implement idempotent object writes and object reads with hash verification.
    - Implement single refs/heads/main, symbolic HEAD, and atomic ref CAS.
    - Implement .scribe/config read/write with unknown-key rejection.
    - Implement .scribe/lock using POSIX flock.
    - Implement stderr plus .scribe/log operational logging.
- Logging:
    - Use the exact line format from DESIGN.md section 18: <iso8601-utc> <level> <component> <message>.
    - Support DEBUG, INFO, WARN, and ERROR levels; default to INFO.
    - Read SCRIBE_LOG_LEVEL at startup and reject unknown levels with SCRIBE_ECONFIG.
    - Write the same log line to stderr and .scribe/log, flushing .scribe/log after each commit.
    - Never log connection-string passwords, document contents, or full resume tokens.
- Commit builder:
    - Validate batches according to section 12.7.
    - Support inserts/updates/replacements as blob payloads and deletions as tombstones.
    - Rebuild only changed tree paths.
    - Create zero-parent initial commit and one-parent subsequent commits.
    - CAS-update refs/heads/main.
- CLI commands for milestone 1:
    - scribe init <path>
    - scribe info
    - scribe commit-batch
    - scribe log [--oneline] [-n <N>]
    - scribe show <commit>
    - scribe cat-object (-p|-t|-s) <hash>
    - scribe diff <commit1> [<commit2>]
    - scribe fsck
- fsck behavior:
    - Read HEAD and refs/heads/main, then walk reachable commits, trees, and blobs from the main ref.
    - For every reachable object, read the loose object, decompress it, verify the envelope, recompute BLAKE3 over the uncompressed envelope, and compare it to the object path/hash.
    - Verify commit parent/root hashes, tree entry type tags, tree entry ordering, tree child references, and malformed LEB128/envelope data.
    - Report dangling loose objects as warnings, not failures, because interrupted writes may leave valid unreachable objects and v1 has no garbage collection.
    - Fail with SCRIBE_ECORRUPT for missing reachable objects, hash mismatches, bad envelopes, malformed commits, malformed trees, or invalid refs.
- Pipe protocol:
    - Implement scribe commit-batch exactly as section 12.3.
    - Return OK\t<64-hex>\n or ERR\t<symbol>\t<len>\n<detail>\n.
    - Treat protocol mismatch as SCRIBE_EPROTOCOL; malformed framing as SCRIBE_EMALFORMED.

## Milestone 2: MongoDB Adapter Bootstrap

- Add conditional scribe_mongo_adapter target enabled only when libmongoc-1.0 and libbson-1.0 are found.
- Add scribe mongo-watch <uri> [opts] only when the adapter is built; in this milestone it runs bootstrap and exits or reports that steady-state watch is not implemented yet.
- Add dependency helper scripts from DESIGN.md section 22.3:
    - scripts/install-deps-ubuntu.sh installs or skips build-essential, cmake, pkg-config, git, libmongoc-dev, and libbson-dev.
    - scripts/install-deps-macos.sh installs or skips cmake, pkg-config, and mongo-c-driver with Homebrew.
- Implement Mongo tree shape:
    - root/database/collection/document
    - document name is canonical _id Extended JSON
- Implement BSON to canonical Extended JSON:
    - Start from libbson canonical Extended JSON.
    - Add deterministic RFC 8785-style key sorting where libbson does not provide it.
    - No JSON library dependency; write the minimal parser/serializer needed for canonicalization.
- Implement bootstrap from DESIGN.md section 13.4:
    - Verify replica set or sharded topology.
    - Capture starting resume token.
    - Scan databases excluding admin, local, config by default.
    - Stream documents, canonicalize, hash with the worker pool, write blobs, assemble trees, create initial commit.
    - Introduce the hash worker pool here: worker_threads controls canonicalization and hashing parallelism during bootstrap.
    - Persist .scribe/adapter-state/mongodb using the section 13.3 text format:

      resume_token <base64>
      last_commit <64-hex>
      last_updated <iso8601>

## Milestone 3: MongoDB Steady-State Change Streams

- Implement steady-state change stream handling:
    - Use mongoc_client_watch with fullDocument: "updateLookup" and fullDocumentBeforeChange: "whenAvailable".
    - Data events produce commits: insert, update, replace, modify, delete.
    - DDL/schema/sharding events are logged and ignored for commit purposes.
    - invalidate logs a warning, marks the saved resume token unusable, and automatically restarts bootstrap to establish a fresh baseline commit and resume token.
    - Persist .scribe/adapter-state/mongodb only after commit durability.
    - On startup, resume from the bootstrap/last-commit token and replay events until caught up before entering watch mode.
- Signal handling:
    - Handle SIGTERM and SIGINT by setting the shutdown flag, stopping change stream reads, draining any in-flight batch, finishing the current commit if one is in progress, persisting adapter state only after commit success, releasing .scribe/lock, and exiting 0 after clean shutdown.
    - If shutdown occurs before a batch is committed, leave the previous resume token intact so the event is replayed on restart.
    - Ignore SIGHUP in v1.

## Milestone 4: v1 Smoke Test, Manual, And Release Validation

- Add the local MongoDB test environment from DESIGN.md section 22.4:
    - docker/docker-compose.yml with a single-node MongoDB replica set.
    - docker/mongo-init.js to initialize the replica set, seed test data, and enable changeStreamPreAndPostImages.
- Add scripts/smoke-test.sh from DESIGN.md section 22.5:
    - Build/use the local scribe binary.
    - Start MongoDB with docker compose.
    - Initialize a temporary .scribe store.
    - Run scribe mongo-watch.
    - Insert a document through mongosh.
    - Stop the watcher.
    - Verify scribe log and scribe diff show the bootstrap commit and insert commit.
    - Print SMOKE TEST: PASSED only when every step succeeds.
- Add docs/MANUAL.md from DESIGN.md section 23:
    - Include all required top-level sections in the required order.
    - Keep command examples consistent with the implemented CLI.
    - Use terminal transcripts from commands that have actually been run.
    - Mark variable output such as hashes and timestamps with documented placeholders.
- Add CI/release validation assets after the local commands pass:
    - GitHub Actions workflow for Linux builds, tests, sanitizers, smoke test, and formatting.
    - clang-format configuration if absent.

## Tests And Acceptance

- Unit tests:
    - LEB128 valid, boundary, overlong, truncated.
    - Hex hash parsing/rendering.
    - Tree ordering and deterministic serialization.
    - Commit serialization with and without parent.
    - Arena allocation, reset behavior, alignment, and ownership assumptions used by core APIs.
    - SPSC queue FIFO ordering, full/empty behavior, blocking/wakeup behavior, and stall warning timing.
    - Object envelope hash stability independent of compression.
    - Object store put/get/has/idempotency/corruption detection.
    - Ref CAS create/update/stale behavior.
    - Batch validation failures map to required error codes.
    - Pipe parser accepts valid frames and rejects malformed/truncated frames.
    - Diff reports add/delete/change and skips hash-equal subtrees.
- Integration tests:
    - scribe init, commit-batch, log, show, cat-object, diff, fsck against a temp .scribe store.
    - Deletion/tombstone behavior.
    - Restart/open existing repo behavior.
- Mongo tests after milestones 2 and 3:
    - Docker single-node replica set.
    - Milestone 2: bootstrap creates initial commit with the expected tree shape and canonical document blobs.
    - Milestone 3: startup resumes from the saved token and enters steady-state watch mode.
    - Insert/update/replace/delete produce expected commits and diffs.
    - Resume token file updates only after successful commit.
- Documentation and release tests after milestone 4:
    - scripts/smoke-test.sh passes end to end.
    - Every shell command in docs/MANUAL.md is either executed verbatim or explicitly marked as a non-executed example with placeholders.
    - Manual output examples match the implemented CLI behavior.
- Acceptance:
    - cmake -B build -DCMAKE_BUILD_TYPE=Debug
    - cmake --build build -j
    - ctest --test-dir build --output-on-failure
    - Milestone 4 additionally passes scripts/smoke-test.sh.

## Assumptions

- Implementation follows DESIGN.md over any conflicting inference.
- Development and verification happen in WSL Ubuntu, not native Windows.
- Core Scribe is delivered before MongoDB adapter work; MongoDB bootstrap, steady-state change streams, and v1 documentation/release validation are separate sessions.
- Pack files, restore, branches, tags, reflog, Windows support, daemon mode, garbage collection, structured logs, and first-class DDL commits remain out of scope for v1.
