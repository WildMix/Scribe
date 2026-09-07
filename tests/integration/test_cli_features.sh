#!/usr/bin/env sh
set -eu

BIN=$(realpath "$1")
if [ $# -ge 2 ]; then
    CLI_BIN=$(realpath "$2")
else
    CLI_BIN="$BIN"
fi
ROOT=$(mktemp -d)
STORE="$ROOT/.scribe"
SERVE_PIDS=""
SYNC_ROOT=$(mktemp -d)
MIRROR_STORE="$SYNC_ROOT/mirror/.scribe"
ARCHIVE_STORE="$SYNC_ROOT/archive/.scribe"
SOURCE_PORT=$((32000 + ($$ % 10000)))
SOURCE_URL="http://127.0.0.1:$SOURCE_PORT"
TLS_PORT=$((37000 + ($$ % 10000)))
ARCHIVE_PORT=$((43000 + ($$ % 10000)))
RELATIVE_PORT=$((55000 + ($$ % 500)))
JSON_PORT=$((46000 + ($$ % 500)))
MIRROR_PORT=$((46500 + ($$ % 500)))

fail() {
    echo "test_cli_features: $*" >&2
    exit 1
}

stop_pid() {
    pid=$1
    [ -n "$pid" ] || return 0
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

cleanup() {
    for pid in $SERVE_PIDS; do
        stop_pid "$pid"
    done
}

trap cleanup EXIT INT TERM

commit_doc_to_store() {
    store=$1
    ts=$2
    id=$3
    json=$4
    {
        printf 'BATCH\t2\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tcase\n'
        printf 'TIMESTAMP\t%s\n' "$ts"
        printf 'MESSAGE\t0\n'
        printf 'EVENT\tput\t3\t%s\n' "$(printf '%s' "$json" | wc -c | tr -d ' ')"
        printf 'db\nusers\n%s\n' "$id"
        printf '%s' "$json"
        printf 'END\n'
    } | "$CLI_BIN" --url "$SOURCE_URL" ingest --pipe | awk -F '\t' '$1 == "OK" { print $2 }'
}

commit_doc() {
    commit_doc_to_store "$STORE" "$1" "$2" "$3"
}

delete_doc() {
    ts=$1
    id=$2
    {
        printf 'BATCH\t2\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tcase\n'
        printf 'TIMESTAMP\t%s\n' "$ts"
        printf 'MESSAGE\t0\n'
        printf 'EVENT\tdelete\t3\n'
        printf 'db\nusers\n%s\n' "$id"
        printf 'END\n'
    } | "$CLI_BIN" --url "$SOURCE_URL" ingest --pipe | awk -F '\t' '$1 == "OK" { print $2 }'
}

wait_for_remote() {
    store=$1
    remote=$2
    out=$3
    i=0
    while [ "$i" -lt 20 ]; do
        if "$CLI_BIN" --store "$store" remote test "$remote" >"$out" 2>"$out.err"; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    cat "$out.err" >&2 || true
    return 1
}

trace_value() {
    key=$1
    file=$2
    tr ' ' '\n' <"$file" | awk -F= -v key="$key" '$1 == key { print $2; exit }'
}

"$BIN" init "$STORE" >/dev/null
"$BIN" --store "$STORE" --listen "127.0.0.1:$SOURCE_PORT" >"$SYNC_ROOT/source-serve.out" \
    2>"$SYNC_ROOT/source-serve.err" &
SOURCE_PID=$!
SERVE_PIDS="$SERVE_PIDS $SOURCE_PID"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if "$CLI_BIN" --url "$SOURCE_URL" info >"$SYNC_ROOT/source-info.ready" 2>"$SYNC_ROOT/source-info.ready.err"; then
        break
    fi
    sleep 0.2
done
[ -s "$SYNC_ROOT/source-info.ready" ] || fail "source server did not become ready"

c1=$(commit_doc 100 alice '{"v":1}')
[ -n "$c1" ] || fail "first commit was not created"
printf '{"v":1}' >"$ROOT/expected-v1"
"$CLI_BIN" --url "$SOURCE_URL" show "$c1:db/users/alice" >"$ROOT/show-v1"
cmp -s "$ROOT/expected-v1" "$ROOT/show-v1" || fail "show commit:path did not emit exact blob bytes"

c2=$(commit_doc 200 alice '{"v":2}')
[ -n "$c2" ] || fail "second commit was not created"
printf '{"v":2}' >"$ROOT/expected-v2"
"$CLI_BIN" --url "$SOURCE_URL" show "$c2:db/users/alice" >"$ROOT/show-v2"
cmp -s "$ROOT/expected-v2" "$ROOT/show-v2" || fail "second version was not byte exact"
"$CLI_BIN" --url "$SOURCE_URL" cat-object -t HEAD >"$ROOT/cat-head-type"
grep -Fx 'commit' "$ROOT/cat-head-type" >/dev/null || fail "cat-object did not resolve HEAD"
"$CLI_BIN" --url "$SOURCE_URL" cat-object -t HEAD~1 >"$ROOT/cat-head-parent-type"
grep -Fx 'commit' "$ROOT/cat-head-parent-type" >/dev/null || fail "cat-object did not resolve HEAD~1"
"$CLI_BIN" --url "$SOURCE_URL" ls-tree HEAD >"$ROOT/ls-tree-head"
grep -F 'db/users/alice' "$ROOT/ls-tree-head" >/dev/null || fail "ls-tree did not resolve HEAD"
"$CLI_BIN" --url "$SOURCE_URL" ls-tree HEAD~1 >"$ROOT/ls-tree-head-parent"
grep -F 'db/users/alice' "$ROOT/ls-tree-head-parent" >/dev/null || fail "ls-tree did not resolve HEAD~1"
"$CLI_BIN" --url "$SOURCE_URL" show "$c2" >"$ROOT/show-c2-meta"
grep -Fx 'committed_at 1970-01-01T00:00:00.000000200Z' "$ROOT/show-c2-meta" >/dev/null ||
    fail "show did not print human commit registration time"

json_id='{"$oid":"6a0f03bf2535f0adaae0a6c7"}'
json_path="db/users/$json_id"
c_json=$(commit_doc 250 "$json_id" '{"v":3}')
[ -n "$c_json" ] || fail "extended-json id commit was not created"
printf '{"v":3}' >"$ROOT/expected-json-id"
"$CLI_BIN" --url "$SOURCE_URL" show "$c_json:$json_path" >"$ROOT/show-json-id"
cmp -s "$ROOT/expected-json-id" "$ROOT/show-json-id" ||
    fail "show commit:path did not handle literal Extended JSON _id path"
"$CLI_BIN" --url "$SOURCE_URL" show "$json_path" >"$ROOT/json-path-history"
grep -F "commit $c_json" "$ROOT/json-path-history" >/dev/null ||
    fail "show path history treated Extended JSON path as commit:path"
c1_abbrev=$(printf '%s' "$c1" | cut -c 1-12)
c2_abbrev=$(printf '%s' "$c2" | cut -c 1-12)
c_json_abbrev=$(printf '%s' "$c_json" | cut -c 1-12)
"$CLI_BIN" --url "$SOURCE_URL" log --oneline --since 1970-01-01T00:00:00.000000150Z \
    --until 1970-01-01T00:00:00.000000250Z >"$ROOT/log-time-range"
range_lines=$(awk 'END { print NR }' "$ROOT/log-time-range")
[ "$range_lines" = "2" ] || fail "timestamp-filtered log emitted $range_lines commits"
grep -F "$c_json_abbrev" "$ROOT/log-time-range" >/dev/null || fail "timestamp-filtered log omitted upper bound"
grep -F "$c2_abbrev" "$ROOT/log-time-range" >/dev/null || fail "timestamp-filtered log omitted matching commit"
if grep -F "$c1_abbrev" "$ROOT/log-time-range" >/dev/null; then
    fail "timestamp-filtered log included commit before --since"
fi
if "$CLI_BIN" --url "$SOURCE_URL" log --since not-a-time >"$ROOT/log-bad-time.out" 2>"$ROOT/log-bad-time.err"; then
    fail "invalid log --since value unexpectedly succeeded"
fi
grep -F 'SCRIBE_EINVAL' "$ROOT/log-bad-time.err" >/dev/null || fail "invalid log --since did not use SCRIBE_EINVAL"

JSON_STORE="$ROOT/json-log/.scribe"
"$BIN" init "$JSON_STORE" >/dev/null
"$BIN" --log-format=json --store "$JSON_STORE" --listen "127.0.0.1:$JSON_PORT" \
    >"$ROOT/json-info-server.out" 2>"$ROOT/json-info.err" &
JSON_PID=$!
SERVE_PIDS="$SERVE_PIDS $JSON_PID"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if "$CLI_BIN" --url "http://127.0.0.1:$JSON_PORT" info >"$ROOT/json-info.out" 2>"$ROOT/json-info-client.err"; then
        break
    fi
    sleep 0.2
done
[ -s "$ROOT/json-info.out" ] || fail "json log server did not become ready"
JSON_LOG="$ROOT/json-info.err" python3 - <<'PY' || fail "json log output did not parse"
import json
import os

with open(os.environ["JSON_LOG"], "r", encoding="utf-8") as fh:
    lines = [line.strip() for line in fh if line.strip()]
assert lines
for line in lines:
    obj = json.loads(line)
    assert obj["timestamp"].endswith("Z")
    assert obj["level"] in {"DEBUG", "INFO", "WARN", "ERROR"}
    assert "component" in obj
    assert "event" in obj
    assert "message" in obj
PY

"$CLI_BIN" --url "$SOURCE_URL" ls-tree "$c2" >"$ROOT/ls-tree"
grep -F 'db/users/alice' "$ROOT/ls-tree" >/dev/null || fail "ls-tree did not recurse to blob"
"$BIN" --store "$STORE" list-objects --type=commit --format='%H:%T:%S:%C' >"$ROOT/list-format"
grep -E "^[0-9a-f]{64}:commit:[0-9]+:[0-9]+$" "$ROOT/list-format" >/dev/null ||
    fail "list-objects custom format failed"
if "$BIN" --store "$STORE" list-objects --format='%X' >"$ROOT/bad-format.out" 2>"$ROOT/bad-format.err"; then
    fail "invalid list-objects format unexpectedly succeeded"
fi
grep -F 'SCRIBE_EINVAL' "$ROOT/bad-format.err" >/dev/null || fail "invalid format did not use SCRIBE_EINVAL"

"$CLI_BIN" --url "$SOURCE_URL" log --oneline -- db/users/alice >"$ROOT/alice-log"
alice_lines=$(awk 'END { print NR }' "$ROOT/alice-log")
[ "$alice_lines" = "2" ] || fail "path-filtered log emitted $alice_lines commits"
if "$CLI_BIN" --url "$SOURCE_URL" log -n nope >"$ROOT/log-bad-limit.out" 2>"$ROOT/log-bad-limit.err"; then
    fail "invalid log -n value unexpectedly succeeded"
fi
grep -F 'SCRIBE_EINVAL' "$ROOT/log-bad-limit.err" >/dev/null || fail "invalid log -n did not use SCRIBE_EINVAL"
"$CLI_BIN" --url "$SOURCE_URL" show db/users/alice >"$ROOT/alice-show-history"

c3=$(delete_doc 300 alice)
[ -n "$c3" ] || fail "delete commit was not created"
"$CLI_BIN" --url "$SOURCE_URL" log --oneline -- db/users/alice >"$ROOT/alice-deleted-log"
head -n 1 "$ROOT/alice-deleted-log" | grep -F '(deleted)' >/dev/null || fail "path log did not annotate deletion"

BUNDLE="$ROOT/repo.sbundle"
IMPORT_STORE="$ROOT/imported/.scribe"
"$BIN" --store "$STORE" bundle create "$BUNDLE" >"$ROOT/bundle-create"
"$BIN" bundle import "$BUNDLE" "$IMPORT_STORE" >"$ROOT/bundle-import"
"$BIN" --store "$IMPORT_STORE" fsck >"$ROOT/import-fsck"
grep -F 'fsck:' "$ROOT/import-fsck" >/dev/null || fail "imported bundle did not fsck"

"$BIN" --store "$STORE" status >"$ROOT/status-before-pack"
grep -Fx 'status ok' "$ROOT/status-before-pack" >/dev/null || fail "status before pack did not report ok"
"$BIN" --store "$STORE" gc --pack >"$ROOT/gc-pack"
grep -E '^gc: pack [1-9][0-9]* reachable loose object\(s\),' "$ROOT/gc-pack" >/dev/null ||
    fail "gc --pack did not report reachable loose objects"
loose_after_pack=$(find "$STORE/objects" -type f | wc -l | tr -d ' ')
[ "$loose_after_pack" = "0" ] || fail "gc --pack left loose object files"
"$BIN" --store "$STORE" fsck >"$ROOT/fsck-after-pack"
grep -F 'fsck:' "$ROOT/fsck-after-pack" >/dev/null || fail "fsck failed after pack"
"$CLI_BIN" --url "$SOURCE_URL" info >"$ROOT/info-after-fsck"
grep -E '^last_fsck_at [0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$' "$ROOT/info-after-fsck" \
    >/dev/null || fail "info did not report last fsck time"
grep -Fx 'last_fsck_status ok' "$ROOT/info-after-fsck" >/dev/null ||
    fail "info did not report last fsck status"
"$BIN" --store "$STORE" status >"$ROOT/status-after-fsck"
grep -Fx 'status ok' "$ROOT/status-after-fsck" >/dev/null || fail "status did not report ok"
for field in commits objects objects_blob objects_tree objects_commit storage_entries refs; do
    before=$(awk -v field="$field" '$1 == field { print $2; exit }' "$ROOT/status-before-pack")
    after=$(awk -v field="$field" '$1 == field { print $2; exit }' "$ROOT/status-after-fsck")
    [ "$before" = "$after" ] || fail "status field $field changed after packing: $before vs $after"
done
grep -Fx 'commits 4' "$ROOT/status-after-fsck" >/dev/null || fail "status did not count HEAD history commits"
grep -Fx "last_commit $c3" "$ROOT/status-after-fsck" >/dev/null || fail "status did not report last commit"
grep -Fx 'last_commit_at 1970-01-01T00:00:00.000000300Z' "$ROOT/status-after-fsck" >/dev/null ||
    fail "status did not report last commit time"
grep -E '^objects [1-9][0-9]*$' "$ROOT/status-after-fsck" >/dev/null || fail "status did not count objects"
grep -E '^pack_files [1-9][0-9]*$' "$ROOT/status-after-fsck" >/dev/null || fail "status did not count pack files"
grep -Fx 'last_fsck_status ok' "$ROOT/status-after-fsck" >/dev/null || fail "status did not report fsck status"
SCRIBE_TRACE_PERF=1 "$BIN" --store "$STORE" status >"$ROOT/status-trace-after-pack" 2>"$ROOT/status-trace-after-pack.err"
grep -Fx 'status ok' "$ROOT/status-trace-after-pack" >/dev/null || fail "traced status did not report ok"
trace_full_reads=$(trace_value full_pack_reads "$ROOT/status-trace-after-pack.err")
trace_object_reads=$(trace_value packed_object_reads "$ROOT/status-trace-after-pack.err")
status_objects=$(awk '$1 == "objects" { print $2; exit }' "$ROOT/status-trace-after-pack")
[ -n "$trace_full_reads" ] || fail "status trace did not report full_pack_reads"
[ -n "$trace_object_reads" ] || fail "status trace did not report packed_object_reads"
[ "$trace_full_reads" -lt "$status_objects" ] ||
    fail "packed status performed one full-pack read per object or worse"
[ "$trace_object_reads" -lt "$status_objects" ] ||
    fail "packed status performed one packed object read per object or worse"

"$BIN" init "$MIRROR_STORE" >/dev/null
"$CLI_BIN" --store "$MIRROR_STORE" remote add origin "http://127.0.0.1:$SOURCE_PORT" --token test-token >/dev/null
wait_for_remote "$MIRROR_STORE" origin "$SYNC_ROOT/source-remote-test" || fail "source remote did not become ready"
grep -F 'scribe server ' "$SYNC_ROOT/source-serve.out" >/dev/null || fail "server banner did not print version"
grep -Fx "listening on 127.0.0.1:$SOURCE_PORT" "$SYNC_ROOT/source-serve.out" >/dev/null ||
    fail "server banner did not print listen address"
grep -Fx "repository $STORE" "$SYNC_ROOT/source-serve.out" >/dev/null ||
    fail "server banner did not print repository path"
grep -Fx 'capabilities info refs log show diff cat-object ls-tree ingest content ingest-stream checkpoints replicate' "$SYNC_ROOT/source-serve.out" >/dev/null ||
    fail "server banner did not print capabilities"

RELATIVE_ROOT="$SYNC_ROOT/relative-store"
mkdir -p "$RELATIVE_ROOT"
(cd "$RELATIVE_ROOT" && "$BIN" init repo/.scribe >/dev/null)
(cd "$RELATIVE_ROOT" && "$BIN" --store repo/.scribe --listen "127.0.0.1:$RELATIVE_PORT" \
    >"$SYNC_ROOT/relative-serve.out" 2>"$SYNC_ROOT/relative-serve.err") &
RELATIVE_PID=$!
SERVE_PIDS="$SERVE_PIDS $RELATIVE_PID"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -F "listening on 127.0.0.1:$RELATIVE_PORT" "$SYNC_ROOT/relative-serve.out" >/dev/null 2>&1 && break
    sleep 0.2
done
RELATIVE_STORE_REAL=$(cd "$RELATIVE_ROOT" && realpath repo/.scribe)
grep -Fx "repository $RELATIVE_STORE_REAL" "$SYNC_ROOT/relative-serve.out" >/dev/null ||
    fail "relative store banner did not print resolved repository path"
grep -Fx "log $RELATIVE_STORE_REAL/scribe_server.log" "$SYNC_ROOT/relative-serve.out" >/dev/null ||
    fail "relative store banner did not print resolved log path"
(cd "$RELATIVE_ROOT" && "$CLI_BIN" --url "http://127.0.0.1:$RELATIVE_PORT" info) >"$SYNC_ROOT/relative-info"
grep -Fx "store $RELATIVE_STORE_REAL" "$SYNC_ROOT/relative-info" >/dev/null ||
    fail "relative store info did not print resolved repository path"
stop_pid "$RELATIVE_PID"
SERVE_PIDS=$(printf '%s\n' "$SERVE_PIDS" | awk -v dead="$RELATIVE_PID" '$1 != dead { print }')
SOURCE_PORT="$SOURCE_PORT" python3 - <<'PY' >"$SYNC_ROOT/not-found-status" || fail "missing endpoint did not return structured error"
import http.client
import json
import os

port = int(os.environ["SOURCE_PORT"])
conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
conn.request("GET", "/scribe/v1/does-not-exist")
res = conn.getresponse()
body = res.read().decode("utf-8")
data = json.loads(body)
print(res.status)
print(body, end="")
if res.status != 404 or data.get("code") != "SCRIBE_ENOT_FOUND" or not data.get("message"):
    raise SystemExit(1)
PY

"$CLI_BIN" --store "$MIRROR_STORE" --remote origin info >"$SYNC_ROOT/remote-info"
grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' "$SYNC_ROOT/remote-info" >/dev/null ||
    fail "remote info did not advertise query capabilities"
grep -Fx 'last_fsck_status ok' "$SYNC_ROOT/remote-info" >/dev/null ||
    fail "remote info did not expose last fsck status"
NO_REPO_DIR="$SYNC_ROOT/no-repo-client"
mkdir -p "$NO_REPO_DIR"
(cd "$NO_REPO_DIR" && "$CLI_BIN" --url "http://127.0.0.1:$SOURCE_PORT" info) >"$SYNC_ROOT/scribe-cli-info"
grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' "$SYNC_ROOT/scribe-cli-info" >/dev/null ||
    fail "scribe-cli --url did not query without a local repository"
if python3 - <<'PY'
import socket

sock = socket.socket()
sock.settimeout(0.2)
try:
    sock.connect(("127.0.0.1", 9323))
except OSError:
    raise SystemExit(1)
finally:
    sock.close()
PY
then
    printf 'default port 9323 already in use; skipping implicit default URL server fixture\n' \
        >"$SYNC_ROOT/default-url-skip"
else
    DEFAULT_STORE="$SYNC_ROOT/default/.scribe"
    "$BIN" init "$DEFAULT_STORE" >/dev/null
    "$BIN" --store "$DEFAULT_STORE" >"$SYNC_ROOT/default-serve.out" 2>"$SYNC_ROOT/default-serve.err" &
    DEFAULT_PID=$!
    SERVE_PIDS="$SERVE_PIDS $DEFAULT_PID"
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if (cd "$NO_REPO_DIR" && "$CLI_BIN" info) >"$SYNC_ROOT/scribe-cli-default-info" \
            2>"$SYNC_ROOT/scribe-cli-default-info.err"; then
            break
        fi
        sleep 0.2
    done
    [ -s "$SYNC_ROOT/scribe-cli-default-info" ] || fail "default server did not become ready"
    grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' \
        "$SYNC_ROOT/scribe-cli-default-info" >/dev/null ||
        fail "scribe-cli info did not query the default server URL"
    printf 'info\nquit\n' | (cd "$NO_REPO_DIR" && "$CLI_BIN") >"$SYNC_ROOT/scribe-cli-default-repl"
    grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' \
        "$SYNC_ROOT/scribe-cli-default-repl" >/dev/null ||
        fail "scribe-cli without arguments did not open the default server URL"
    if ! CLI_BIN="$CLI_BIN" NO_REPO_DIR="$NO_REPO_DIR" python3 - >"$SYNC_ROOT/scribe-cli-default-repl-tty" <<'PY'
import os
import pty
import select
import subprocess
import time

master, slave = pty.openpty()
proc = subprocess.Popen(
    [os.environ["CLI_BIN"]],
    cwd=os.environ["NO_REPO_DIR"],
    stdin=slave,
    stdout=slave,
    stderr=slave,
    close_fds=True,
)
os.close(slave)
data = bytearray()
deadline = time.time() + 5
while time.time() < deadline:
    ready, _, _ = select.select([master], [], [], 0.1)
    if ready:
        chunk = os.read(master, 4096)
        if not chunk:
            break
        data.extend(chunk)
    if b"scribe-cli [http://127.0.0.1:9323]> " in data:
        break
else:
    print(data.decode("utf-8", "replace"), end="")
    raise SystemExit(1)
os.write(master, b"quit\n")
try:
    proc.wait(timeout=2)
except subprocess.TimeoutExpired:
    proc.terminate()
    proc.wait(timeout=2)
text = data.decode("utf-8", "replace")
print(text, end="")
os.close(master)
PY
    then
        fail "scribe-cli default repl prompt did not appear on a tty"
    fi
    grep -F 'scribe-cli [http://127.0.0.1:9323]' "$SYNC_ROOT/scribe-cli-default-repl-tty" >/dev/null ||
        fail "scribe-cli default repl prompt did not show the default server URL"
    printf '%s' '{"source":"default-local-server"}' >"$SYNC_ROOT/default-content.json"
    (cd "$NO_REPO_DIR" &&
        "$CLI_BIN" ingest --path default/content --message "default content ingest" \
            --file "$SYNC_ROOT/default-content.json") >"$SYNC_ROOT/default-content-response" ||
        fail "scribe-cli content ingest did not use the default local server"
    (cd "$NO_REPO_DIR" && "$CLI_BIN" show HEAD:default/content) >"$SYNC_ROOT/default-content-show" ||
        fail "scribe-cli could not read content from the default local server"
    cmp -s "$SYNC_ROOT/default-content.json" "$SYNC_ROOT/default-content-show" ||
        fail "default local content ingest did not preserve exact file bytes"
    stop_pid "$DEFAULT_PID"
    SERVE_PIDS=$(printf '%s\n' "$SERVE_PIDS" | awk -v dead="$DEFAULT_PID" '$1 != dead { print }')
fi
command -v openssl >/dev/null 2>&1 || fail "openssl is required for TLS/HTTP2 integration coverage"
TLS_CERT="$SYNC_ROOT/scribe-test-cert.pem"
TLS_KEY="$SYNC_ROOT/scribe-test-key.pem"
TLS_CONF="$SYNC_ROOT/openssl.cnf"
cat >"$TLS_CONF" <<EOF
[req]
distinguished_name=dn
x509_extensions=v3_req
prompt=no
[dn]
CN=127.0.0.1
[v3_req]
basicConstraints=critical,CA:TRUE
keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign
extendedKeyUsage=serverAuth
subjectAltName=IP:127.0.0.1,DNS:localhost
EOF
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TLS_KEY" -out "$TLS_CERT" -days 1 \
    -config "$TLS_CONF" >/dev/null 2>&1 || fail "failed to generate TLS test certificate"
"$BIN" --store "$STORE" --listen "127.0.0.1:$TLS_PORT" --cert "$TLS_CERT" --key "$TLS_KEY" \
    >"$SYNC_ROOT/tls-serve.out" 2>"$SYNC_ROOT/tls-serve.err" &
TLS_PID=$!
SERVE_PIDS="$SERVE_PIDS $TLS_PID"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if SCRIBE_TLS_CA_FILE="$TLS_CERT" "$CLI_BIN" --url "https://127.0.0.1:$TLS_PORT" info \
        >"$SYNC_ROOT/tls-info" 2>"$SYNC_ROOT/tls-info.err"; then
        break
    fi
    sleep 0.2
done
[ -s "$SYNC_ROOT/tls-info" ] || fail "TLS/HTTP2 server did not become ready"
grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' "$SYNC_ROOT/tls-info" >/dev/null ||
    fail "TLS/HTTP2 info did not advertise query capabilities"
if "$CLI_BIN" --url "https://localhost:$TLS_PORT" info >"$SYNC_ROOT/tls-untrusted.out" \
    2>"$SYNC_ROOT/tls-untrusted.err"; then
    fail "TLS self-signed server unexpectedly succeeded without SCRIBE_TLS_CA_FILE"
fi
grep -F 'set SCRIBE_TLS_CA_FILE=/path/to/ca.pem' "$SYNC_ROOT/tls-untrusted.err" >/dev/null ||
    fail "TLS verification failure did not explain SCRIBE_TLS_CA_FILE"
grep -Fx "transport HTTP/2" "$SYNC_ROOT/tls-serve.out" >/dev/null ||
    fail "TLS server banner did not report HTTP/2 transport"
grep -Fx "tls enabled" "$SYNC_ROOT/tls-serve.out" >/dev/null ||
    fail "TLS server banner did not report TLS enabled"
"$CLI_BIN" --store "$MIRROR_STORE" remote add tlsorigin "https://127.0.0.1:$TLS_PORT" >/dev/null
if ! SCRIBE_TLS_CA_FILE="$TLS_CERT" "$CLI_BIN" --store "$MIRROR_STORE" remote test tlsorigin \
    >"$SYNC_ROOT/tls-remote-test" 2>"$SYNC_ROOT/tls-remote-test.err"; then
    cat "$SYNC_ROOT/tls-remote-test.err" >&2 || true
    fail "TLS named remote test failed"
fi
if ! SCRIBE_TLS_CA_FILE="$TLS_CERT" "$CLI_BIN" --store "$MIRROR_STORE" --remote tlsorigin refs \
    >"$SYNC_ROOT/tls-remote-refs" 2>"$SYNC_ROOT/tls-remote-refs.err"; then
    cat "$SYNC_ROOT/tls-remote-refs.err" >&2 || true
    fail "TLS named remote refs failed"
fi
if ! SCRIBE_TLS_CA_FILE="$TLS_CERT" "$BIN" --store "$MIRROR_STORE" replicate tlsorigin \
    >"$SYNC_ROOT/tls-replicate" 2>"$SYNC_ROOT/tls-replicate.err"; then
    cat "$SYNC_ROOT/tls-replicate.err" >&2 || true
    fail "TLS named remote replication failed"
fi
"$BIN" --store "$MIRROR_STORE" fsck >"$SYNC_ROOT/tls-mirror-fsck"
grep -F 'fsck:' "$SYNC_ROOT/tls-mirror-fsck" >/dev/null || fail "TLS replicated mirror did not fsck"
"$BIN" --store "$MIRROR_STORE" --listen "127.0.0.1:$MIRROR_PORT" \
    >"$SYNC_ROOT/mirror-serve.out" 2>"$SYNC_ROOT/mirror-serve.err" &
MIRROR_PID=$!
SERVE_PIDS="$SERVE_PIDS $MIRROR_PID"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if "$CLI_BIN" --url "http://127.0.0.1:$MIRROR_PORT" refs >"$SYNC_ROOT/tls-mirror-local-refs" \
        2>"$SYNC_ROOT/tls-mirror-local-refs.err"; then
        break
    fi
    sleep 0.2
done
[ -s "$SYNC_ROOT/tls-mirror-local-refs" ] || fail "mirror server did not become ready"
cmp -s "$SYNC_ROOT/tls-remote-refs" "$SYNC_ROOT/tls-mirror-local-refs" ||
    fail "TLS replicated refs differed from remote refs"
stop_pid "$MIRROR_PID"
(cd "$NO_REPO_DIR" && "$CLI_BIN" --remote "http://127.0.0.1:$SOURCE_PORT" info) \
    >"$SYNC_ROOT/scribe-remote-url-info" 2>"$SYNC_ROOT/scribe-remote-url-info.err" &&
    fail "scribe-cli --remote <url> unexpectedly succeeded"
printf 'info\nquit\n' | (cd "$NO_REPO_DIR" && "$CLI_BIN" --url "http://127.0.0.1:$SOURCE_PORT") \
    >"$SYNC_ROOT/scribe-cli-url-repl"
grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' "$SYNC_ROOT/scribe-cli-url-repl" >/dev/null ||
    fail "scribe-cli --url without command did not start the repl"
printf 'info\nquit\n' | "$CLI_BIN" --store "$MIRROR_STORE" --remote origin >"$SYNC_ROOT/scribe-cli-remote-repl"
grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' "$SYNC_ROOT/scribe-cli-remote-repl" >/dev/null ||
    fail "scribe-cli --remote without command did not start the repl after preflight"
if printf 'info\nquit\n' | (cd "$NO_REPO_DIR" && "$CLI_BIN" --url "http://127.0.0.1:1") \
    >"$SYNC_ROOT/scribe-cli-url-repl-bad.out" 2>"$SYNC_ROOT/scribe-cli-url-repl-bad.err"; then
    fail "scribe-cli --url bad endpoint unexpectedly entered the repl"
fi
grep -F 'SCRIBE_EIO' "$SYNC_ROOT/scribe-cli-url-repl-bad.err" >/dev/null ||
    fail "scribe-cli --url bad endpoint did not fail during repl preflight"
"$CLI_BIN" --store "$MIRROR_STORE" remote add dead "http://127.0.0.1:1" >/dev/null
if printf 'info\nquit\n' | "$CLI_BIN" --store "$MIRROR_STORE" --remote dead \
    >"$SYNC_ROOT/scribe-cli-remote-repl-bad.out" 2>"$SYNC_ROOT/scribe-cli-remote-repl-bad.err"; then
    fail "scribe-cli --remote bad endpoint unexpectedly entered the repl"
fi
grep -F 'SCRIBE_EIO' "$SYNC_ROOT/scribe-cli-remote-repl-bad.err" >/dev/null ||
    fail "scribe-cli --remote bad endpoint did not fail during repl preflight"
if "$BIN" --store "$STORE" log --oneline >"$SYNC_ROOT/scribe-server-log.out" 2>"$SYNC_ROOT/scribe-server-log.err"; then
    fail "scribe server binary unexpectedly accepted client query command"
fi
if "$BIN" --store "$STORE" ingest >"$SYNC_ROOT/scribe-server-ingest.out" 2>"$SYNC_ROOT/scribe-server-ingest.err"; then
    fail "scribe server binary unexpectedly accepted client ingest command"
fi
if "$BIN" --store "$STORE" commit-batch >"$SYNC_ROOT/scribe-server-commit-batch.out" \
    2>"$SYNC_ROOT/scribe-server-commit-batch.err"; then
    fail "scribe server binary unexpectedly accepted commit-batch command"
fi
grep -F 'commit-batch moved to' "$SYNC_ROOT/scribe-server-commit-batch.err" >/dev/null ||
    fail "scribe server commit-batch rejection did not point to scribe-cli ingest --pipe"
if "$BIN" --store "$MIRROR_STORE" remote list >"$SYNC_ROOT/scribe-remote-list.out" \
    2>"$SYNC_ROOT/scribe-remote-list.err"; then
    fail "scribe server binary unexpectedly accepted remote config command"
fi
if "$CLI_BIN" --url "$SOURCE_URL" repl >"$SYNC_ROOT/scribe-cli-repl-command.out" 2>"$SYNC_ROOT/scribe-cli-repl-command.err"; then
    fail "scribe-cli repl subcommand unexpectedly succeeded"
fi
"$BIN" --help >"$SYNC_ROOT/scribe-help"
for help_cmd in \
    'init [path]' \
    'branch <name> [<commit>]' \
    'tag [--force] <name> <commit>' \
    'tag -d <name> [--force]' \
    'reflog <ref>' \
    'list-objects [options]' \
    'status' \
    'fsck' \
    'gc [--dry-run] [--prune-now] [--pack] [--consolidate]' \
    'bundle create <file>' \
    'bundle import <file> <target-store>' \
    '--listen <host:port>' \
    'replicate <remote>' \
    'daemon --config <file>'; do
    grep -F -- "$help_cmd" "$SYNC_ROOT/scribe-help" >/dev/null || fail "scribe help did not describe $help_cmd"
done
if grep -F 'commit-batch' "$SYNC_ROOT/scribe-help" >/dev/null; then
    fail "scribe help still advertised commit-batch"
fi
if grep -F 'mongo-watch <uri>' "$SYNC_ROOT/scribe-help" >/dev/null; then
    fail "scribe help still advertised MongoDB watch"
fi
if grep -F 'restore <commit> --target <mongo-uri>' "$SYNC_ROOT/scribe-help" >/dev/null; then
    fail "scribe help still advertised MongoDB restore"
fi
if grep -F 'remote add/list/remove/test' "$SYNC_ROOT/scribe-help" >/dev/null; then
    fail "scribe help still advertised remote config"
fi
if grep -F 'serve --listen' "$SYNC_ROOT/scribe-help" >/dev/null; then
    fail "scribe help still advertised serve subcommand"
fi
"$CLI_BIN" --help >"$SYNC_ROOT/scribe-cli-help"
grep -F 'No command starts the interactive REPL' "$SYNC_ROOT/scribe-cli-help" >/dev/null ||
    fail "scribe-cli help did not describe no-command repl"
for help_cmd in \
    'info' \
    'refs' \
    'log [--oneline] [--paths] [--since <time>] [--until <time>] [-n <N>] [--] [<path>]' \
    'show <commit>|<commit>:<path>|<path>' \
    'diff <commit1> [<commit2>]' \
    'cat-object (-p|-t|-s) <hash-or-revision>' \
    'ls-tree <hash-or-revision>' \
    'ingest [--json] [<json-file>]' \
    'ingest --path <path> [--message <message>] --file <file>' \
    'ingest --path <path> [--message <message>] --delete' \
    'ingest --pipe [<frame-file>]' \
    'remote add <name> <url>' \
    'remote list' \
    'remote remove <name>' \
    'remote test <name>'; do
    grep -F -- "$help_cmd" "$SYNC_ROOT/scribe-cli-help" >/dev/null || fail "scribe-cli help did not describe $help_cmd"
done
if grep -F 'repl [--remote' "$SYNC_ROOT/scribe-cli-help" >/dev/null; then
    fail "scribe-cli help still advertised repl subcommand"
fi
if grep -F 'piero' "$SYNC_ROOT/scribe-cli-help" >/dev/null; then
    fail "scribe-cli help advertised hidden command"
fi
if grep -F 'vito' "$SYNC_ROOT/scribe-cli-help" >/dev/null; then
    fail "scribe-cli help advertised hidden command"
fi
if "$CLI_BIN" piero >"$SYNC_ROOT/piero.out" 2>"$SYNC_ROOT/piero.err"; then
    fail "scribe-cli unexpectedly accepted removed piero command"
fi
if "$CLI_BIN" vito >"$SYNC_ROOT/vito.out" 2>"$SYNC_ROOT/vito.err"; then
    fail "scribe-cli unexpectedly accepted removed vito command"
fi
printf 'piero\nhelp\nquit\n' | "$CLI_BIN" --url "$SOURCE_URL" >"$SYNC_ROOT/piero-repl.out" \
    2>"$SYNC_ROOT/piero-repl.err" || fail "repl did not recover from removed piero command"
printf 'vito\nhelp\nquit\n' | "$CLI_BIN" --url "$SOURCE_URL" >"$SYNC_ROOT/vito-repl.out" \
    2>"$SYNC_ROOT/vito-repl.err" || fail "repl did not recover from removed vito command"
grep -F "unknown repl command 'piero'" "$SYNC_ROOT/piero-repl.err" >/dev/null ||
    fail "repl did not reject removed piero command"
grep -F "unknown repl command 'vito'" "$SYNC_ROOT/vito-repl.err" >/dev/null ||
    fail "repl did not reject removed vito command"
if grep -F 'piero' "$SYNC_ROOT/piero-repl.out" >/dev/null; then
    fail "repl help advertised removed command"
fi
if grep -F 'vito' "$SYNC_ROOT/vito-repl.out" >/dev/null; then
    fail "repl help advertised removed command"
fi
if "$CLI_BIN" --store "$MIRROR_STORE" log --remote origin --oneline >"$SYNC_ROOT/command-local-remote.out" \
    2>"$SYNC_ROOT/command-local-remote.err"; then
    fail "scribe-cli command-local --remote unexpectedly succeeded"
fi
if "$BIN" --url "http://127.0.0.1:$SOURCE_PORT" >"$SYNC_ROOT/scribe-url.out" \
    2>"$SYNC_ROOT/scribe-url.err"; then
    fail "scribe server binary unexpectedly accepted --url"
fi
if "$BIN" --store "$STORE" mongo-watch "mongodb://127.0.0.1:1" >"$SYNC_ROOT/scribe-mongo-watch.out" \
    2>"$SYNC_ROOT/scribe-mongo-watch.err"; then
    fail "scribe server binary unexpectedly accepted MongoDB watch"
fi
if "$BIN" --store "$STORE" restore HEAD --target "mongodb://127.0.0.1:1" >"$SYNC_ROOT/scribe-restore.out" \
    2>"$SYNC_ROOT/scribe-restore.err"; then
    fail "scribe server binary unexpectedly accepted MongoDB restore"
fi
if "$CLI_BIN" --token test-token info >"$SYNC_ROOT/scribe-cli-token.out" 2>"$SYNC_ROOT/scribe-cli-token.err"; then
    fail "scribe-cli unexpectedly accepted --token without --url"
fi

"$CLI_BIN" --url "$SOURCE_URL" refs >"$SYNC_ROOT/local-refs"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin refs >"$SYNC_ROOT/remote-refs"
cmp -s "$SYNC_ROOT/local-refs" "$SYNC_ROOT/remote-refs" || fail "remote refs output differed from local refs"
"$CLI_BIN" --url "$SOURCE_URL" log --oneline --paths -n 3 >"$SYNC_ROOT/local-log"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin log --oneline --paths -n 3 >"$SYNC_ROOT/remote-log"
cmp -s "$SYNC_ROOT/local-log" "$SYNC_ROOT/remote-log" || fail "remote log output differed from local log"
"$CLI_BIN" --url "$SOURCE_URL" log --oneline --since 150 --until 300 >"$SYNC_ROOT/local-log-range"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin log --oneline --since 150 --until 300 >"$SYNC_ROOT/remote-log-range"
cmp -s "$SYNC_ROOT/local-log-range" "$SYNC_ROOT/remote-log-range" ||
    fail "remote timestamp-filtered log output differed from local log"
"$CLI_BIN" --url "$SOURCE_URL" show "$c2" >"$SYNC_ROOT/local-show"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin show "$c2" >"$SYNC_ROOT/remote-show"
cmp -s "$SYNC_ROOT/local-show" "$SYNC_ROOT/remote-show" || fail "remote show output differed from local show"
"$CLI_BIN" --url "$SOURCE_URL" diff "$c2" "$c3" >"$SYNC_ROOT/local-diff"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin diff "$c2" "$c3" >"$SYNC_ROOT/remote-diff"
cmp -s "$SYNC_ROOT/local-diff" "$SYNC_ROOT/remote-diff" || fail "remote diff output differed from local diff"
"$CLI_BIN" --url "$SOURCE_URL" ls-tree "$c2" >"$SYNC_ROOT/local-ls-tree"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin ls-tree "$c2" >"$SYNC_ROOT/remote-ls-tree"
cmp -s "$SYNC_ROOT/local-ls-tree" "$SYNC_ROOT/remote-ls-tree" || fail "remote ls-tree output differed from local ls-tree"
"$CLI_BIN" --url "$SOURCE_URL" ls-tree HEAD >"$SYNC_ROOT/local-ls-tree-head"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin ls-tree HEAD >"$SYNC_ROOT/remote-ls-tree-head"
cmp -s "$SYNC_ROOT/local-ls-tree-head" "$SYNC_ROOT/remote-ls-tree-head" ||
    fail "remote ls-tree HEAD output differed from local ls-tree HEAD"
blob_hash=$(awk '$1 == "blob" && $3 == "db/users/alice" { print $2; exit }' "$SYNC_ROOT/local-ls-tree")
[ -n "$blob_hash" ] || fail "did not find alice blob hash for remote cat-object parity"
"$CLI_BIN" --url "$SOURCE_URL" cat-object -p "$blob_hash" >"$SYNC_ROOT/local-cat-object"
cmp -s "$ROOT/expected-v2" "$SYNC_ROOT/local-cat-object" ||
    fail "cat-object -p blob did not emit exact blob bytes"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin cat-object -p "$blob_hash" >"$SYNC_ROOT/remote-cat-object"
cmp -s "$SYNC_ROOT/local-cat-object" "$SYNC_ROOT/remote-cat-object" ||
    fail "remote cat-object output differed from local cat-object"
"$CLI_BIN" --url "$SOURCE_URL" cat-object -t HEAD >"$SYNC_ROOT/local-cat-head"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin cat-object -t HEAD >"$SYNC_ROOT/remote-cat-head"
cmp -s "$SYNC_ROOT/local-cat-head" "$SYNC_ROOT/remote-cat-head" ||
    fail "remote cat-object HEAD output differed from local cat-object HEAD"
"$CLI_BIN" --url "$SOURCE_URL" show "$c2:db/users/alice" >"$SYNC_ROOT/local-show-path"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin show "$c2:db/users/alice" >"$SYNC_ROOT/remote-show-path"
cmp -s "$SYNC_ROOT/local-show-path" "$SYNC_ROOT/remote-show-path" ||
    fail "remote show commit:path output differed from local show commit:path"

printf 'info\nlog --oneline -n 1\nlog --paths -n 1\nuse %s\nshow\nmore\nquit\n' "$c2" |
    "$CLI_BIN" --url "$SOURCE_URL" >"$SYNC_ROOT/local-repl"
grep -F 'current '"$c2" "$SYNC_ROOT/local-repl" >/dev/null || fail "local repl did not accept use command"
grep -F 'D db/users/alice' "$SYNC_ROOT/local-repl" >/dev/null || fail "local repl did not support log --paths"
grep -F 'more: no paginated query is pending' "$SYNC_ROOT/local-repl" >/dev/null ||
    fail "local repl did not handle more command"
CLI_BIN="$CLI_BIN" SOURCE_URL="$SOURCE_URL" python3 - <<'PY' >"$SYNC_ROOT/repl-cursor" || fail "repl cursor movement failed"
import os
import pty
import re
import select
import subprocess
import time

master, slave = pty.openpty()
proc = subprocess.Popen(
    [os.environ["CLI_BIN"], "--url", os.environ["SOURCE_URL"]],
    stdin=slave,
    stdout=slave,
    stderr=slave,
    close_fds=True,
)
os.close(slave)
data = bytearray()

def pump_until(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                chunk = os.read(master, 4096)
            except OSError:
                return predicate(data.decode("utf-8", "replace"))
            if not chunk:
                return predicate(data.decode("utf-8", "replace"))
            data.extend(chunk)
        text = data.decode("utf-8", "replace")
        if predicate(text):
            return True
        if proc.poll() is not None:
            return predicate(text)
    return False

if not pump_until(lambda text: "scribe-cli [" in text, 5):
    print(data.decode("utf-8", "replace"), end="")
    raise SystemExit("repl prompt did not appear")
os.write(master, b"in\x01\x1b[1;5Cfo\n")
if not pump_until(
    lambda text: "capabilities info refs log show diff cat-object ls-tree ingest" in text
    and "last_fsck_status ok" in text
    and text.rfind("scribe-cli [") > text.rfind("last_fsck_status ok"),
    5,
):
    print(data.decode("utf-8", "replace"), end="")
    raise SystemExit("ctrl-right edited info command did not execute")
log_start = len(data.decode("utf-8", "replace"))
os.write(master, b"log --oneline 1\x1b[1;5D-n \n")
if not pump_until(lambda text: re.search(r"[0-9a-f]{12}(?:\r|\n| )", text[log_start:]) is not None, 5):
    print(data.decode("utf-8", "replace"), end="")
    raise SystemExit("ctrl-left edited log command did not execute")
os.write(master, b"quit\n")
pump_until(lambda text: proc.poll() is not None, 5)
text = data.decode("utf-8", "replace")
if proc.poll() is None:
    proc.terminate()
    try:
        rc = proc.wait(timeout=1)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = proc.wait(timeout=1)
else:
    rc = proc.returncode
os.close(master)
print(text, end="")
if rc != 0:
    raise SystemExit(rc)
if "capabilities info refs log show diff cat-object ls-tree ingest" not in text:
    raise SystemExit("edited info command did not execute")
if "unknown repl command" in text:
    raise SystemExit("cursor-edited command was parsed incorrectly")
if "unknown repl log option" in text:
    raise SystemExit("ctrl-left edited log command was parsed incorrectly")
PY
printf 'info\nlog --oneline -n 1\nshow %s\nquit\n' "$c2" |
    "$CLI_BIN" --store "$MIRROR_STORE" --remote origin >"$SYNC_ROOT/remote-repl"
grep -F 'capabilities info refs log show diff cat-object ls-tree ingest' "$SYNC_ROOT/remote-repl" >/dev/null ||
    fail "remote repl info did not query server"
grep -F "commit $c2" "$SYNC_ROOT/remote-repl" >/dev/null || fail "remote repl show did not query server"

printf '%s' '{"transport":"h2"}' >"$SYNC_ROOT/tls-content.json"
if ! SCRIBE_TLS_CA_FILE="$TLS_CERT" "$CLI_BIN" --store "$MIRROR_STORE" --remote tlsorigin ingest \
    --path tls/content --message "TLS content ingest" --file "$SYNC_ROOT/tls-content.json" \
    >"$SYNC_ROOT/tls-content-response" 2>"$SYNC_ROOT/tls-content-response.err"; then
    cat "$SYNC_ROOT/tls-content-response.err" >&2 || true
    fail "TLS named remote content ingest failed"
fi
SCRIBE_TLS_CA_FILE="$TLS_CERT" "$CLI_BIN" --url "https://127.0.0.1:$TLS_PORT" show HEAD:tls/content \
    >"$SYNC_ROOT/tls-content-show" || fail "TLS content ingest could not be queried"
cmp -s "$SYNC_ROOT/tls-content.json" "$SYNC_ROOT/tls-content-show" ||
    fail "TLS content ingest did not preserve exact file bytes"

c4=$(commit_doc_to_store "$STORE" 400 bob '{"role":"reader"}')
[ -n "$c4" ] || fail "commit while serve was running did not acquire writer lock"
SOURCE_PORT="$SOURCE_PORT" python3 - <<'PY' >"$SYNC_ROOT/core-ingest-response" || fail "core ingest endpoint failed"
import base64
import http.client
import json
import os

port = int(os.environ["SOURCE_PORT"])
payload = {
    "message": "core ingest integration",
    "events": [
        {
            "path": ["http", "core"],
            "op": "put",
            "payload": base64.b64encode(b'{"v":1}').decode("ascii"),
        }
    ],
}
body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
conn.request(
    "POST",
    "/scribe/v1/ingest",
    body,
    {"Content-Type": "application/json", "Content-Length": str(len(body))},
)
res = conn.getresponse()
raw = res.read().decode("utf-8")
print(res.status)
print(raw, end="")
data = json.loads(raw)
if res.status != 200 or len(data.get("commit", "")) != 64:
    raise SystemExit(1)
PY
grep -Fx '200' "$SYNC_ROOT/core-ingest-response" >/dev/null || fail "core ingest did not return HTTP 200"
printf '%s' '{"v":1}' >"$SYNC_ROOT/core-ingest-v1-expected"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/core >"$SYNC_ROOT/core-ingest-show"
cmp -s "$SYNC_ROOT/core-ingest-v1-expected" "$SYNC_ROOT/core-ingest-show" ||
    fail "core ingest did not commit the requested blob"
printf '{"timestamp_unix_nanos":2,"message":"core ingest retry","events":[{"path":["http","core"],"op":"put","payload":"eyJ2IjoxfQ=="}]}\n' \
    >"$SYNC_ROOT/core-ingest-retry.json"
"$CLI_BIN" --url "$SOURCE_URL" ingest "$SYNC_ROOT/core-ingest-retry.json" \
    >"$SYNC_ROOT/core-ingest-retry-response" || fail "unchanged core ingest retry failed"
python3 - "$SYNC_ROOT/core-ingest-response" "$SYNC_ROOT/core-ingest-retry-response" <<'PY' || fail "unchanged core ingest created a new commit"
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    lines = stream.readlines()
initial = json.loads("".join(lines[1:]))
with open(sys.argv[2], "r", encoding="utf-8") as stream:
    retry = json.load(stream)
if retry.get("commit") != initial.get("commit"):
    raise SystemExit(1)
PY
"$CLI_BIN" --url "$SOURCE_URL" log --oneline -- http/core >"$SYNC_ROOT/core-ingest-noop-log"
core_noop_lines=$(awk 'END { print NR }' "$SYNC_ROOT/core-ingest-noop-log")
[ "$core_noop_lines" = "1" ] ||
    fail "unchanged core ingest added a history node; path log has $core_noop_lines entries"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/core >"$SYNC_ROOT/core-ingest-retry-show"
cmp -s "$SYNC_ROOT/core-ingest-v1-expected" "$SYNC_ROOT/core-ingest-retry-show" ||
    fail "unchanged core ingest retry altered the path value"

printf '{"timestamp_unix_nanos":3,"message":"core ingest changed","events":[{"path":["http","core"],"op":"put","payload":"eyJ2IjoyfQ=="}]}\n' \
    >"$SYNC_ROOT/core-ingest-changed.json"
"$CLI_BIN" --url "$SOURCE_URL" ingest "$SYNC_ROOT/core-ingest-changed.json" \
    >"$SYNC_ROOT/core-ingest-changed-response" || fail "changed core ingest failed"
python3 - "$SYNC_ROOT/core-ingest-response" "$SYNC_ROOT/core-ingest-changed-response" <<'PY' || fail "changed core ingest reused the previous commit"
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    lines = stream.readlines()
initial = json.loads("".join(lines[1:]))
with open(sys.argv[2], "r", encoding="utf-8") as stream:
    changed = json.load(stream)
if changed.get("commit") == initial.get("commit"):
    raise SystemExit(1)
PY
"$CLI_BIN" --url "$SOURCE_URL" log --oneline -- http/core >"$SYNC_ROOT/core-ingest-changed-log"
core_changed_lines=$(awk 'END { print NR }' "$SYNC_ROOT/core-ingest-changed-log")
[ "$core_changed_lines" = "2" ] ||
    fail "changed core ingest did not add one history node; path log has $core_changed_lines entries"
printf '%s' '{"v":2}' >"$SYNC_ROOT/core-ingest-changed-expected"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/core >"$SYNC_ROOT/core-ingest-changed-show"
cmp -s "$SYNC_ROOT/core-ingest-changed-expected" "$SYNC_ROOT/core-ingest-changed-show" ||
    fail "changed core ingest did not replace the path value"
printf '{"message":"cli ingest integration","events":[{"path":["http","cli"],"op":"put","payload":"aGVsbG8gZnJvbSBzY3JpYmUtY2xpIGluZ2VzdAo="}]}\n' \
    >"$SYNC_ROOT/cli-ingest.json"
"$CLI_BIN" --url "http://127.0.0.1:$SOURCE_PORT" ingest "$SYNC_ROOT/cli-ingest.json" \
    >"$SYNC_ROOT/cli-ingest-response" || fail "scribe-cli ingest failed"
python3 - "$SYNC_ROOT/cli-ingest-response" <<'PY' || fail "scribe-cli ingest response was not a commit JSON object"
import json
import sys

data = json.load(open(sys.argv[1], "r", encoding="utf-8"))
if len(data.get("commit", "")) != 64:
    raise SystemExit(1)
PY
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/cli >"$SYNC_ROOT/cli-ingest-show"
grep -Fx 'hello from scribe-cli ingest' "$SYNC_ROOT/cli-ingest-show" >/dev/null ||
    fail "scribe-cli ingest did not commit the requested blob"
printf '{"message":"cli ingest delete integration","events":[{"path":["http","cli"],"op":"delete"}]}\n' \
    >"$SYNC_ROOT/cli-ingest-delete.json"
"$CLI_BIN" --url "http://127.0.0.1:$SOURCE_PORT" ingest "$SYNC_ROOT/cli-ingest-delete.json" \
    >"$SYNC_ROOT/cli-ingest-delete-response" || fail "scribe-cli ingest delete failed"
if "$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/cli >"$SYNC_ROOT/cli-ingest-delete-show.out" \
    2>"$SYNC_ROOT/cli-ingest-delete-show.err"; then
    fail "scribe-cli ingest delete did not remove the requested blob"
fi
grep -F 'SCRIBE_ENOT_FOUND' "$SYNC_ROOT/cli-ingest-delete-show.err" >/dev/null ||
    fail "scribe-cli ingest delete did not leave the path absent"
printf '{"message":"named remote cli ingest","events":[{"path":["http","cli-remote"],"op":"put","payload":"aGVsbG8gZnJvbSBuYW1lZCByZW1vdGUgaW5nZXN0Cg=="}]}\n' |
    "$CLI_BIN" --store "$MIRROR_STORE" --remote origin ingest >"$SYNC_ROOT/cli-ingest-remote-response" ||
    fail "scribe-cli --remote ingest failed"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/cli-remote >"$SYNC_ROOT/cli-ingest-remote-show"
grep -Fx 'hello from named remote ingest' "$SYNC_ROOT/cli-ingest-remote-show" >/dev/null ||
    fail "scribe-cli --remote ingest did not commit the requested blob"
"$CLI_BIN" --url "$SOURCE_URL" ingest "$SYNC_ROOT/cli-ingest.json" >"$SYNC_ROOT/cli-ingest-local.out" \
    2>"$SYNC_ROOT/cli-ingest-local.err" || fail "scribe-cli URL ingest retry failed"
python3 - "$SYNC_ROOT/cli-ingest-local.out" <<'PY' || fail "scribe-cli URL ingest retry response was not a commit JSON object"
import json
import sys

data = json.load(open(sys.argv[1], "r", encoding="utf-8"))
if len(data.get("commit", "")) != 64:
    raise SystemExit(1)
PY
printf '%s' '{"name":"Alice","roles":["reader"]}' >"$SYNC_ROOT/content-ingest.json"
"$CLI_BIN" --url "$SOURCE_URL" ingest --path http/content-file --message "raw content & message" \
    --file "$SYNC_ROOT/content-ingest.json" >"$SYNC_ROOT/content-ingest-response" ||
    fail "scribe-cli URL content ingest failed"
grep -E '^\{"commit":"[0-9a-f]{64}"\}$' "$SYNC_ROOT/content-ingest-response" >/dev/null ||
    fail "scribe-cli URL content ingest did not return commit JSON"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/content-file >"$SYNC_ROOT/content-ingest-show"
cmp -s "$SYNC_ROOT/content-ingest.json" "$SYNC_ROOT/content-ingest-show" ||
    fail "scribe-cli URL content ingest did not preserve exact file bytes"
"$CLI_BIN" --url "$SOURCE_URL" log --oneline -- http/content-file >"$SYNC_ROOT/content-ingest-log"
grep -F 'raw content & message' "$SYNC_ROOT/content-ingest-log" >/dev/null ||
    fail "scribe-cli content ingest did not preserve the URL-encoded message"

"$CLI_BIN" --url "$SOURCE_URL" ingest --path http/content-file --message "unchanged retry" \
    --file "$SYNC_ROOT/content-ingest.json" >"$SYNC_ROOT/content-ingest-retry-response" ||
    fail "scribe-cli unchanged content ingest retry failed"
cmp -s "$SYNC_ROOT/content-ingest-response" "$SYNC_ROOT/content-ingest-retry-response" ||
    fail "scribe-cli unchanged content ingest retry created a new commit"
"$CLI_BIN" --url "$SOURCE_URL" log --oneline -- http/content-file >"$SYNC_ROOT/content-ingest-retry-log"
content_log_lines=$(awk 'END { print NR }' "$SYNC_ROOT/content-ingest-retry-log")
[ "$content_log_lines" = "1" ] ||
    fail "scribe-cli unchanged content ingest added a history node"

python3 - "$SYNC_ROOT/content-large.bin" <<'PY'
import sys

with open(sys.argv[1], "wb") as fh:
    fh.write(bytes((i * 31 + 17) & 0xFF for i in range(64 * 1024)))
PY
"$CLI_BIN" --url "$SOURCE_URL" ingest --path http/large-content --message "large binary" \
    --file "$SYNC_ROOT/content-large.bin" >"$SYNC_ROOT/content-large-response" ||
    fail "scribe-cli large content ingest failed"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/large-content >"$SYNC_ROOT/content-large-show"
cmp -s "$SYNC_ROOT/content-large.bin" "$SYNC_ROOT/content-large-show" ||
    fail "scribe-cli large content ingest did not preserve exact bytes after HTTP buffer growth"
"$CLI_BIN" --url "$SOURCE_URL" ingest --path http/large-content --message "large binary retry" \
    --file "$SYNC_ROOT/content-large.bin" >"$SYNC_ROOT/content-large-retry-response" ||
    fail "scribe-cli unchanged large content ingest retry failed"
cmp -s "$SYNC_ROOT/content-large-response" "$SYNC_ROOT/content-large-retry-response" ||
    fail "scribe-cli unchanged large content ingest retry created a new commit"
"$CLI_BIN" --url "$SOURCE_URL" log --oneline -- http/large-content >"$SYNC_ROOT/content-large-log"
large_content_log_lines=$(awk 'END { print NR }' "$SYNC_ROOT/content-large-log")
[ "$large_content_log_lines" = "1" ] ||
    fail "scribe-cli unchanged large content ingest added a history node"

: >"$SYNC_ROOT/content-empty.bin"
"$CLI_BIN" --url "$SOURCE_URL" ingest --path http/empty-content --file "$SYNC_ROOT/content-empty.bin" \
    >"$SYNC_ROOT/content-empty-response" || fail "scribe-cli empty content ingest failed"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/empty-content >"$SYNC_ROOT/content-empty-show" ||
    fail "scribe-cli empty content ingest was treated as a tombstone"
[ ! -s "$SYNC_ROOT/content-empty-show" ] || fail "scribe-cli empty content ingest did not store a zero-byte blob"

printf '%s' 'named remote raw content' >"$SYNC_ROOT/content-remote.txt"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin ingest --path http/content-remote \
    --message "named remote content" --file "$SYNC_ROOT/content-remote.txt" \
    >"$SYNC_ROOT/content-remote-response" || fail "scribe-cli named remote content ingest failed"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/content-remote >"$SYNC_ROOT/content-remote-show"
cmp -s "$SYNC_ROOT/content-remote.txt" "$SYNC_ROOT/content-remote-show" ||
    fail "scribe-cli named remote content ingest did not preserve exact file bytes"
"$CLI_BIN" --store "$MIRROR_STORE" --remote origin ingest --path http/content-remote \
    --message "named remote tombstone" --delete >"$SYNC_ROOT/content-remote-delete-response" ||
    fail "scribe-cli named remote content delete failed"
if "$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/content-remote >"$SYNC_ROOT/content-remote-deleted.out" \
    2>"$SYNC_ROOT/content-remote-deleted.err"; then
    fail "scribe-cli named remote content delete did not create a tombstone"
fi
grep -F 'SCRIBE_ENOT_FOUND' "$SYNC_ROOT/content-remote-deleted.err" >/dev/null ||
    fail "scribe-cli named remote content delete did not leave the path absent"

if "$CLI_BIN" --url "$SOURCE_URL" ingest --path http//bad --file "$SYNC_ROOT/content-ingest.json" \
    >"$SYNC_ROOT/content-invalid-path.out" 2>"$SYNC_ROOT/content-invalid-path.err"; then
    fail "scribe-cli content ingest accepted an empty path component"
fi
grep -F 'SCRIBE_EPATH' "$SYNC_ROOT/content-invalid-path.err" >/dev/null ||
    fail "scribe-cli invalid content path did not report SCRIBE_EPATH"

pipe_payload_len=$(printf 'hello from pipe ingest\n' | wc -c | tr -d ' ')
{
    printf 'BATCH\t2\t1\n'
    printf 'AUTHOR\ttester\t\ttest\n'
    printf 'COMMITTER\tscribe-test\t\tscribe\n'
    printf 'PROCESS\tcli-test\t1\t\tpipe-ingest\n'
    printf 'TIMESTAMP\t500\n'
    printf 'MESSAGE\t0\n'
    printf 'EVENT\tput\t2\t%s\n' "$pipe_payload_len"
    printf 'http\npipe-file\n'
    printf 'hello from pipe ingest\n'
    printf 'END\n'
} >"$SYNC_ROOT/cli-ingest-pipe.frame"
"$CLI_BIN" --url "$SOURCE_URL" ingest --pipe "$SYNC_ROOT/cli-ingest-pipe.frame" \
    >"$SYNC_ROOT/cli-ingest-pipe-response" || fail "scribe-cli ingest --pipe failed"
grep -E '^OK[[:space:]][0-9a-f]{64}$' "$SYNC_ROOT/cli-ingest-pipe-response" >/dev/null ||
    fail "scribe-cli ingest --pipe did not emit an OK commit response"
"$CLI_BIN" --url "$SOURCE_URL" show HEAD:http/pipe-file >"$SYNC_ROOT/cli-ingest-pipe-show"
grep -Fx 'hello from pipe ingest' "$SYNC_ROOT/cli-ingest-pipe-show" >/dev/null ||
    fail "scribe-cli ingest --pipe did not commit the requested blob"
"$CLI_BIN" --url "http://127.0.0.1:$SOURCE_PORT" ingest --pipe "$SYNC_ROOT/cli-ingest-pipe.frame" \
    >"$SYNC_ROOT/cli-ingest-pipe-remote.out" 2>"$SYNC_ROOT/cli-ingest-pipe-remote.err" ||
    fail "scribe-cli ingest --pipe did not accept URL mode"
grep -E '^OK[[:space:]][0-9a-f]{64}$' "$SYNC_ROOT/cli-ingest-pipe-remote.out" >/dev/null ||
    fail "scribe-cli ingest --pipe URL mode did not emit an OK commit response"
if "$BIN" --store "$MIRROR_STORE" fsck --remote origin >"$SYNC_ROOT/remote-fsck.out" \
    2>"$SYNC_ROOT/remote-fsck.err"; then
    fail "fsck unexpectedly accepted --remote"
fi
if "$BIN" --store "$MIRROR_STORE" list-objects --remote origin >"$SYNC_ROOT/remote-list-objects.out" \
    2>"$SYNC_ROOT/remote-list-objects.err"; then
    fail "list-objects unexpectedly accepted --remote"
fi
if "$BIN" --store "$MIRROR_STORE" replicate origin --interval-ms nope >"$SYNC_ROOT/replicate-bad-interval.out" \
    2>"$SYNC_ROOT/replicate-bad-interval.err"; then
    fail "invalid replicate --interval-ms unexpectedly succeeded"
fi
grep -F 'SCRIBE_EINVAL' "$SYNC_ROOT/replicate-bad-interval.err" >/dev/null ||
    fail "invalid replicate --interval-ms did not use SCRIBE_EINVAL"

"$BIN" --store "$MIRROR_STORE" replicate origin >"$SYNC_ROOT/replicate-origin"
"$BIN" --store "$MIRROR_STORE" fsck >"$SYNC_ROOT/mirror-fsck"
grep -F 'fsck:' "$SYNC_ROOT/mirror-fsck" >/dev/null || fail "replicated mirror did not fsck"
"$BIN" --store "$MIRROR_STORE" replicate origin >"$SYNC_ROOT/replicate-origin-noop"
noop_storage=$(
    awk '{ for (i = 1; i <= NF; i++) if ($i == "storage") { print $(i - 1); exit } }' \
        "$SYNC_ROOT/replicate-origin-noop"
)
[ "$noop_storage" = "0" ] || fail "no-op replicate transferred $noop_storage storage entries"
grep -F 'serve negotiate-pull: sent' "$SYNC_ROOT/source-serve.out" >/dev/null ||
    fail "serve output did not log filtered replication send"
grep -F 'INFO serve request started: method=PUT path=/scribe/v1/content?path=http&path=large-content&message=large%20binary request_bytes=65536' \
    "$SYNC_ROOT/source-serve.err" >/dev/null || fail "serve did not log when a request started"
grep -E 'INFO serve request: method=GET path=/scribe/v1/info status=200 duration_ms=[0-9]+ request_bytes=0 response_bytes=[1-9][0-9]*' \
    "$SYNC_ROOT/source-serve.err" >/dev/null || fail "serve did not log request status, duration, and byte sizes"
grep -F 'INFO serve request started: method=PUT path=/scribe/v1/content?path=http&path=large-content&message=large%20binary request_bytes=65536' \
    "$STORE/scribe_server.log" >/dev/null || fail "serve did not persist request-start logs in scribe_server.log"
grep -E 'INFO serve request: method=GET path=/scribe/v1/info status=200 duration_ms=[0-9]+ request_bytes=0 response_bytes=[1-9][0-9]*' \
    "$STORE/scribe_server.log" >/dev/null || fail "serve did not persist request logs in scribe_server.log"
if grep -F 'INFO serve request:' "$STORE/log" >/dev/null 2>&1; then
    fail "serve request logs leaked into default repository log"
fi
if grep -F 'INFO serve request started:' "$STORE/log" >/dev/null 2>&1; then
    fail "serve request-start logs leaked into default repository log"
fi

REPLICA_STORE="$SYNC_ROOT/replica/.scribe"
"$BIN" init "$REPLICA_STORE" >/dev/null
"$CLI_BIN" --store "$REPLICA_STORE" remote add source "http://127.0.0.1:$SOURCE_PORT" >/dev/null
"$BIN" --store "$REPLICA_STORE" replicate source >"$SYNC_ROOT/replicate-source"
"$BIN" --store "$REPLICA_STORE" fsck >"$SYNC_ROOT/replica-fsck"
grep -F 'fsck:' "$SYNC_ROOT/replica-fsck" >/dev/null || fail "replicated repository did not fsck"
"$BIN" init "$ARCHIVE_STORE" >/dev/null
"$CLI_BIN" --store "$ARCHIVE_STORE" remote add source "http://127.0.0.1:$SOURCE_PORT" >/dev/null
"$BIN" --store "$ARCHIVE_STORE" replicate source >"$SYNC_ROOT/replicate-archive"
"$BIN" --store "$ARCHIVE_STORE" fsck >"$SYNC_ROOT/archive-fsck"
grep -F 'fsck:' "$SYNC_ROOT/archive-fsck" >/dev/null || fail "replicated archive did not fsck"

if "$BIN" --store "$MIRROR_STORE" pull origin >"$SYNC_ROOT/pull-removed.out" 2>"$SYNC_ROOT/pull-removed.err"; then
    fail "removed pull command unexpectedly succeeded"
fi
if "$BIN" --store "$MIRROR_STORE" push origin >"$SYNC_ROOT/push-removed.out" 2>"$SYNC_ROOT/push-removed.err"; then
    fail "removed push command unexpectedly succeeded"
fi

DAEMON_PORT=$((54000 + ($$ % 1000)))
printf 'listen\t127.0.0.1:%s\nrepo\tprimary\t%s\nrepo\tmirror\t%s\n' "$DAEMON_PORT" "$STORE" "$MIRROR_STORE" \
    >"$SYNC_ROOT/daemon.conf"
"$BIN" daemon --config "$SYNC_ROOT/daemon.conf" >"$SYNC_ROOT/daemon.out" 2>"$SYNC_ROOT/daemon.err" &
DAEMON_PID=$!
SERVE_PIDS="$SERVE_PIDS $DAEMON_PID"
DAEMON_PORT="$DAEMON_PORT" python3 - <<'PY' >"$SYNC_ROOT/daemon-status" || fail "daemon status endpoint failed"
import http.client
import json
import os
import time

port = int(os.environ["DAEMON_PORT"])
last = None
for _ in range(20):
    try:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
        conn.request("GET", "/scribe/v1/status")
        res = conn.getresponse()
        body = res.read().decode("utf-8")
        if res.status == 200:
            data = json.loads(body)
            names = {repo["name"] for repo in data["repositories"]}
            assert {"primary", "mirror"} <= names
            print(body, end="")
            break
        last = body
    except Exception as exc:
        last = repr(exc)
    time.sleep(0.5)
else:
    raise SystemExit(last)
PY

stop_pid "$DAEMON_PID"
SERVE_PIDS=$(printf '%s\n' "$SERVE_PIDS" | awk -v dead="$DAEMON_PID" '$1 != dead { print }')

"$BIN" --store "$ARCHIVE_STORE" --listen "127.0.0.1:$ARCHIVE_PORT" >"$SYNC_ROOT/archive-serve.out" \
    2>"$SYNC_ROOT/archive-serve.err" &
ARCHIVE_PID=$!
SERVE_PIDS="$SERVE_PIDS $ARCHIVE_PID"
"$CLI_BIN" --store "$MIRROR_STORE" remote add archive "http://127.0.0.1:$ARCHIVE_PORT" >/dev/null
wait_for_remote "$MIRROR_STORE" archive "$SYNC_ROOT/archive-remote-test" || fail "archive serve did not become ready"
ARCHIVE_PORT="$ARCHIVE_PORT" python3 - <<'PY' >"$SYNC_ROOT/corrupt-upload-status" || fail "corrupt receive-pack request failed"
import http.client
import os

port = int(os.environ["ARCHIVE_PORT"])
conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
conn.request("POST", "/scribe/v1/receive-pack", b"not a scribe bundle", {"Content-Length": "19"})
res = conn.getresponse()
body = res.read().decode("utf-8", "replace")
print(res.status)
print(body, end="")
if res.status < 400:
    raise SystemExit(1)
if "SCRIBE_ECORRUPT" not in body:
    raise SystemExit(2)
PY

grep -E '^(400|409|423|500)$' "$SYNC_ROOT/corrupt-upload-status" >/dev/null ||
    fail "corrupt receive-pack did not return a rejection status"
grep -F 'SCRIBE_ECORRUPT' "$SYNC_ROOT/corrupt-upload-status" >/dev/null ||
    fail "corrupt receive-pack did not report SCRIBE_ECORRUPT"

echo "test_cli_features: PASS"
