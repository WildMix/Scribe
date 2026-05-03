#!/usr/bin/env bash
set -euo pipefail

mongod --replSet scribe-rs --bind_ip_all --port 27017 --dbpath /data/db &
mongod_pid=$!

cleanup() {
    kill -TERM "$mongod_pid" 2>/dev/null || true
    wait "$mongod_pid" 2>/dev/null || true
}
trap cleanup TERM INT

for _ in $(seq 1 60); do
    if mongosh --quiet --eval 'db.adminCommand({ping: 1})' >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

mongosh --quiet --eval '
try {
  rs.initiate({_id: "scribe-rs", members: [{_id: 0, host: "localhost:27017"}]});
} catch (e) {
  if (!String(e).match(/already initialized|already been initialized/)) {
    throw e;
  }
}
' >/dev/null

for _ in $(seq 1 60); do
    if mongosh --quiet --eval 'quit(rs.status().myState === 1 ? 0 : 1)' >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

wait "$mongod_pid"
