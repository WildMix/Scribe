#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
COMPOSE_FILE="$ROOT_DIR/docker/docker-compose.yml"
SMOKE_DIR=/tmp/scribe-smoke
STORE="$SMOKE_DIR/.scribe"
MONGO_URI="mongodb://localhost:27017/?replicaSet=scribe-rs"
MONGO_DB_URI="mongodb://localhost:27017/scribe_test?replicaSet=scribe-rs"
SCRIBE_PID=
COMPOSE_STARTED=0

cleanup() {
    if [ -n "$SCRIBE_PID" ] && kill -0 "$SCRIBE_PID" 2>/dev/null; then
        kill -TERM "$SCRIBE_PID" 2>/dev/null || true
        wait "$SCRIBE_PID" 2>/dev/null || true
    fi
    if [ "$COMPOSE_STARTED" -eq 1 ]; then
        docker compose -f "$COMPOSE_FILE" down >/dev/null 2>&1 || true
    fi
}

fail() {
    echo "SMOKE TEST: FAILED: $*" >&2
    exit 1
}

mongo_eval() {
    if command -v mongosh >/dev/null 2>&1; then
        mongosh "$MONGO_DB_URI" --eval "$1"
    else
        docker compose -f "$COMPOSE_FILE" exec -T mongo mongosh "$MONGO_DB_URI" --eval "$1"
    fi
}

trap cleanup EXIT INT TERM

cd "$ROOT_DIR"

echo "1. docker compose -f docker/docker-compose.yml up -d --wait"
docker compose -f docker/docker-compose.yml up -d --wait
COMPOSE_STARTED=1

echo "2. rm -rf /tmp/scribe-smoke && mkdir -p /tmp/scribe-smoke"
rm -rf "$SMOKE_DIR" && mkdir -p "$SMOKE_DIR"

echo "3. ./build/scribe init /tmp/scribe-smoke/.scribe"
./build/scribe init "$STORE"

echo "4. ./build/scribe mongo-watch \"mongodb://localhost:27017/?replicaSet=scribe-rs\" --store /tmp/scribe-smoke/.scribe &"
./build/scribe mongo-watch "$MONGO_URI" --store "$STORE" >"$SMOKE_DIR/watch.out" 2>"$SMOKE_DIR/watch.err" &
SCRIBE_PID=$!

echo "5. sleep 2"
sleep 2
if ! kill -0 "$SCRIBE_PID" 2>/dev/null; then
    cat "$SMOKE_DIR/watch.err" >&2 || true
    fail "mongo-watch exited before the write"
fi
ready_wait=0
while ! grep -F "watching MongoDB change stream" "$SMOKE_DIR/watch.err" >/dev/null 2>&1; do
    if ! kill -0 "$SCRIBE_PID" 2>/dev/null; then
        cat "$SMOKE_DIR/watch.err" >&2 || true
        fail "mongo-watch exited before entering steady-state"
    fi
    ready_wait=$((ready_wait + 1))
    if [ "$ready_wait" -gt 30 ]; then
        cat "$SMOKE_DIR/watch.err" >&2 || true
        fail "mongo-watch did not enter steady-state"
    fi
    sleep 1
done

echo "6. mongosh \"mongodb://localhost:27017/scribe_test?replicaSet=scribe-rs\" --eval 'db.users.insertOne({_id: \"alice\", role: \"admin\"})'"
mongo_eval 'db.users.insertOne({_id: "alice", role: "admin"})' >/dev/null

echo "7. sleep 1"
sleep 1

echo "8. kill -TERM \$SCRIBE_PID && wait \$SCRIBE_PID"
kill -TERM "$SCRIBE_PID"
wait "$SCRIBE_PID"
SCRIBE_PID=

echo "9. ./build/scribe --store /tmp/scribe-smoke/.scribe log"
./build/scribe --store "$STORE" log >"$SMOKE_DIR/log.out"
cat "$SMOKE_DIR/log.out"

echo "10. ./build/scribe --store /tmp/scribe-smoke/.scribe diff HEAD~1 HEAD"
./build/scribe --store "$STORE" diff HEAD~1 HEAD >"$SMOKE_DIR/diff.out"
cat "$SMOKE_DIR/diff.out"

echo "11. docker compose -f docker/docker-compose.yml down"
docker compose -f docker/docker-compose.yml down
COMPOSE_STARTED=0

commit_count=$(grep -c '^commit ' "$SMOKE_DIR/log.out" || true)
if [ "$commit_count" -lt 2 ]; then
    fail "expected at least two commits, found $commit_count"
fi
if ! grep -F 'A scribe_test/users/"alice"' "$SMOKE_DIR/diff.out" >/dev/null; then
    fail "expected alice addition in HEAD~1..HEAD diff"
fi

echo "SMOKE TEST: PASSED"
