#!/usr/bin/env sh
set -eu

BIN=$1
ROOT=$(mktemp -d)
STORE="$ROOT/.scribe"

fail() {
    echo "test_cli_features: $*" >&2
    exit 1
}

commit_two_docs() {
    {
        printf 'BATCH\t1\t3\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tcase\n'
        printf 'TIMESTAMP\t100\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\nb\n'
        printf '{"v":9}'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\na\n'
        printf '{"v":1}'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\n{"$oid":"69ee1fb36acc97dcf62258d5"}\n'
        printf '{"v":3}'
        printf 'END\n'
    } | "$BIN" --store "$STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
}

commit_update_a() {
    {
        printf 'BATCH\t1\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tcase\n'
        printf 'TIMESTAMP\t200\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\na\n'
        printf '{"v":2}'
        printf 'END\n'
    } | "$BIN" --store "$STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
}

"$BIN" init "$STORE" >/dev/null

c1=$(commit_two_docs)
[ -n "$c1" ] || fail "first commit was not created"

printf '{"v":1}' >"$ROOT/expected-v1"
"$BIN" --store "$STORE" show "$c1:db/users/a" >"$ROOT/show-v1"
cmp -s "$ROOT/expected-v1" "$ROOT/show-v1" || fail "show commit:path did not emit exact blob bytes"
printf '{"v":3}' >"$ROOT/expected-oid"
"$BIN" --store "$STORE" show "$c1"':db/users/{"$oid":"69ee1fb36acc97dcf62258d5"}' >"$ROOT/show-oid"
cmp -s "$ROOT/expected-oid" "$ROOT/show-oid" || fail "show commit:path did not handle literal Extended JSON _id path"

root_tree=$("$BIN" --store "$STORE" cat-object -p "$c1" | awk '/^tree / { print $2; exit }')
[ -n "$root_tree" ] || fail "could not read root tree"
"$BIN" --store "$STORE" ls-tree "$c1" >"$ROOT/root-by-commit"
"$BIN" --store "$STORE" ls-tree "$root_tree" >"$ROOT/root-by-tree"
cmp -s "$ROOT/root-by-commit" "$ROOT/root-by-tree" || fail "ls-tree commit and tree output differ"
expected_root_paths=$(printf 'db\ndb/users\ndb/users/a\ndb/users/b\ndb/users/{"$oid":"69ee1fb36acc97dcf62258d5"}')
actual_root_paths=$(awk -F '\t' '{ print $3 }' "$ROOT/root-by-commit")
[ "$actual_root_paths" = "$expected_root_paths" ] || fail "ls-tree did not recurse in byte-sorted order"
"$BIN" --store "$STORE" show "$c1:" >"$ROOT/root-by-empty-show"
cmp -s "$ROOT/root-by-commit" "$ROOT/root-by-empty-show" || fail "show commit: did not list root tree"

"$BIN" --store "$STORE" show "$c1:db" >"$ROOT/db-tree"
users_tree=$(awk -F '\t' '$1 == "tree" && $3 == "users" { print $2 }' "$ROOT/db-tree")
[ -n "$users_tree" ] || fail "could not find users tree"
"$BIN" --store "$STORE" ls-tree "$users_tree" >"$ROOT/users-by-ls-tree"
"$BIN" --store "$STORE" show "$c1:db/users" >"$ROOT/users-by-show"
cmp -s "$ROOT/users-by-ls-tree" "$ROOT/users-by-show" || fail "show tree path did not match ls-tree"
expected_names=$(printf 'a\nb\n{"$oid":"69ee1fb36acc97dcf62258d5"}')
actual_names=$(awk -F '\t' '{ print $3 }' "$ROOT/users-by-ls-tree")
[ "$actual_names" = "$expected_names" ] || fail "ls-tree did not preserve byte-sorted tree order"

blob_hash=$(awk -F '\t' '$3 == "a" { print $2 }' "$ROOT/users-by-ls-tree")
[ -n "$blob_hash" ] || fail "could not find blob hash"
if "$BIN" --store "$STORE" ls-tree "$blob_hash" >"$ROOT/blob-ls.out" 2>"$ROOT/blob-ls.err"; then
    fail "ls-tree on a blob unexpectedly succeeded"
fi
grep -F 'SCRIBE_EINVAL' "$ROOT/blob-ls.err" >/dev/null || fail "ls-tree blob error did not use SCRIBE_EINVAL"

if "$BIN" --store "$STORE" show "$c1:db/users/missing" >"$ROOT/bad-path.out" 2>"$ROOT/bad-path.err"; then
    fail "show bad path unexpectedly succeeded"
fi
grep -F 'SCRIBE_ENOT_FOUND' "$ROOT/bad-path.err" >/dev/null || fail "bad path did not use SCRIBE_ENOT_FOUND"

if "$BIN" --store "$STORE" show "$c1:db/users/a/extra" >"$ROOT/blob-descend.out" 2>"$ROOT/blob-descend.err"; then
    fail "show path through blob unexpectedly succeeded"
fi
grep -F "path component 'a' resolved to a blob; cannot descend further" "$ROOT/blob-descend.err" >/dev/null ||
    fail "blob descent error text changed"

c2=$(commit_update_a)
[ -n "$c2" ] || fail "second commit was not created"
printf '{"v":2}' >"$ROOT/expected-v2"
"$BIN" --store "$STORE" show "$c2:db/users/a" >"$ROOT/show-v2"
cmp -s "$ROOT/expected-v2" "$ROOT/show-v2" || fail "second version was not byte exact"

printf '%s\n' "$c1" >"$STORE/refs/heads/main"
"$BIN" --store "$STORE" list-objects >"$ROOT/list-all"
"$BIN" --store "$STORE" list-objects --reachable >"$ROOT/list-reachable"
grep -F "$c2 commit" "$ROOT/list-all" >/dev/null || fail "default list-objects missed unreachable commit"
if grep -F "$c2 commit" "$ROOT/list-reachable" >/dev/null; then
    fail "reachable list-objects included unreachable commit"
fi

"$BIN" --store "$STORE" list-objects --type=commit >"$ROOT/list-commit"
awk '{ if ($2 != "commit") exit 1 }' "$ROOT/list-commit" || fail "commit type filter included other types"
grep -F "$c2 commit" "$ROOT/list-commit" >/dev/null || fail "commit type filter missed unreachable commit"

"$BIN" --store "$STORE" list-objects --type=blob --type=tree >"$ROOT/list-blob-tree"
awk '{ if ($2 == "commit") exit 1 }' "$ROOT/list-blob-tree" || fail "multi-type filter included commits"
grep -F ' blob ' "$ROOT/list-blob-tree" >/dev/null || fail "multi-type filter missed blobs"
grep -F ' tree ' "$ROOT/list-blob-tree" >/dev/null || fail "multi-type filter missed trees"

"$BIN" --store "$STORE" list-objects --type=commit --format='%H:%T:%S:%C' >"$ROOT/list-format"
grep -E "^[0-9a-f]{64}:commit:[0-9]+:[0-9]+$" "$ROOT/list-format" >/dev/null ||
    fail "custom format did not include all placeholders"

if "$BIN" --store "$STORE" list-objects --format='%X' >"$ROOT/bad-format.out" 2>"$ROOT/bad-format.err"; then
    fail "invalid list-objects format unexpectedly succeeded"
fi
grep -F 'SCRIBE_EINVAL' "$ROOT/bad-format.err" >/dev/null || fail "invalid format did not use SCRIBE_EINVAL"

LOG_ROOT=$(mktemp -d)
LOG_STORE="$LOG_ROOT/.scribe"
"$BIN" init "$LOG_STORE" >/dev/null

log_commit_initial=$(
    {
        printf 'BATCH\t1\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tlog-initial\n'
        printf 'TIMESTAMP\t500\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\nalice\n'
        printf '{"v":1}'
        printf 'END\n'
    } | "$BIN" --store "$LOG_STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
)
[ -n "$log_commit_initial" ] || fail "log initial commit was not created"

log_commit_add_bob=$(
    {
        printf 'BATCH\t1\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tlog-add-bob\n'
        printf 'TIMESTAMP\t600\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\nbob\n'
        printf '{"v":1}'
        printf 'END\n'
    } | "$BIN" --store "$LOG_STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
)
[ -n "$log_commit_add_bob" ] || fail "log add-bob commit was not created"

log_commit_update_both=$(
    {
        printf 'BATCH\t1\t2\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tlog-update-both\n'
        printf 'TIMESTAMP\t700\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\nalice\n'
        printf '{"v":2}'
        printf 'EVENT\t3\t7\n'
        printf 'db\nusers\nbob\n'
        printf '{"v":2}'
        printf 'END\n'
    } | "$BIN" --store "$LOG_STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
)
[ -n "$log_commit_update_both" ] || fail "log update-both commit was not created"

log_commit_delete_bob=$(
    {
        printf 'BATCH\t1\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tlog-delete-bob\n'
        printf 'TIMESTAMP\t800\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t0\n'
        printf 'db\nusers\nbob\n'
        printf 'END\n'
    } | "$BIN" --store "$LOG_STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
)
[ -n "$log_commit_delete_bob" ] || fail "log delete-bob commit was not created"

log_commit_unrelated=$(
    {
        printf 'BATCH\t1\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tlog-unrelated\n'
        printf 'TIMESTAMP\t900\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t7\n'
        printf 'db\naudit\nmarker\n'
        printf '{"v":1}'
        printf 'END\n'
    } | "$BIN" --store "$LOG_STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
)
[ -n "$log_commit_unrelated" ] || fail "log unrelated commit was not created"

"$BIN" --store "$LOG_STORE" log --oneline -- db/users/alice >"$LOG_ROOT/alice-history"
alice_lines=$(awk 'END { print NR }' "$LOG_ROOT/alice-history")
[ "$alice_lines" = "2" ] || fail "path filter for path present from commit 1 emitted $alice_lines commits"
grep -F "$(printf '%.12s' "$log_commit_initial")" "$LOG_ROOT/alice-history" | grep -F '(added)' >/dev/null ||
    fail "path filter did not annotate initial alice appearance as added"

"$BIN" --store "$LOG_STORE" log --oneline -- db/users/bob >"$LOG_ROOT/bob-history"
bob_lines=$(awk 'END { print NR }' "$LOG_ROOT/bob-history")
[ "$bob_lines" = "3" ] || fail "path filter for added/deleted path emitted $bob_lines commits"
head -n 1 "$LOG_ROOT/bob-history" | grep -F "$(printf '%.12s' "$log_commit_delete_bob")" | grep -F '(deleted)' >/dev/null ||
    fail "path filter did not annotate bob deletion"
tail -n 1 "$LOG_ROOT/bob-history" | grep -F "$(printf '%.12s' "$log_commit_add_bob")" | grep -F '(added)' >/dev/null ||
    fail "path filter did not annotate bob addition"

"$BIN" --store "$LOG_STORE" log --oneline -- db/users >"$LOG_ROOT/users-tree-history"
users_tree_lines=$(awk 'END { print NR }' "$LOG_ROOT/users-tree-history")
[ "$users_tree_lines" = "4" ] || fail "tree-level path filter emitted $users_tree_lines commits"
if grep -F "$(printf '%.12s' "$log_commit_unrelated")" "$LOG_ROOT/users-tree-history" >/dev/null; then
    fail "tree-level path filter included unrelated commit"
fi

"$BIN" --store "$LOG_STORE" log --oneline --paths -n 1 -- db/users/alice >"$LOG_ROOT/alice-oneline-paths"
alice_oneline_paths=$(cat "$LOG_ROOT/alice-oneline-paths")
printf '%s\n' "$alice_oneline_paths" | grep -F "$(printf '%.12s' "$log_commit_update_both")" >/dev/null ||
    fail "-n with path filter did not skip unrelated commits before counting"
printf '%s\n' "$alice_oneline_paths" | grep -F '[2 changed]' >/dev/null ||
    fail "--oneline --paths did not append total changed-path count"

"$BIN" --store "$LOG_STORE" log --paths -- db/users/alice >"$LOG_ROOT/alice-full-paths"
grep -F '  M db/users/alice' "$LOG_ROOT/alice-full-paths" >/dev/null ||
    fail "--paths omitted filtered path modification"
grep -F '  M db/users/bob' "$LOG_ROOT/alice-full-paths" >/dev/null ||
    fail "--paths with a path filter did not list all changes in the commit"
grep -F '  A db/users/alice' "$LOG_ROOT/alice-full-paths" >/dev/null ||
    fail "initial commit with --paths did not list alice as added"

LARGE_ROOT=$(mktemp -d)
LARGE_STORE="$LARGE_ROOT/.scribe"
LARGE_COUNT=5000
"$BIN" init "$LARGE_STORE" >/dev/null
large_c1=$(
    {
        printf 'BATCH\t1\t%s\n' "$LARGE_COUNT"
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tlarge-tree\n'
        printf 'TIMESTAMP\t300\n'
        printf 'MESSAGE\t0\n'
        i=0
        while [ "$i" -lt "$LARGE_COUNT" ]; do
            id=$(printf 'id%05d' "$i")
            printf 'EVENT\t3\t7\n'
            printf 'db\nbig\n%s\n' "$id"
            printf '{"v":0}'
            i=$((i + 1))
        done
        printf 'END\n'
    } | "$BIN" --store "$LARGE_STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
)
[ -n "$large_c1" ] || fail "large tree commit was not created"
"$BIN" --store "$LARGE_STORE" ls-tree "$large_c1" >"$LARGE_ROOT/large-ls-tree"
large_lines=$(awk 'END { print NR }' "$LARGE_ROOT/large-ls-tree")
[ "$large_lines" = "5002" ] || fail "large ls-tree did not traverse every object"
large_c2=$(
    {
        printf 'BATCH\t1\t1\n'
        printf 'AUTHOR\ttester\t\ttest\n'
        printf 'COMMITTER\tscribe-test\t\tscribe\n'
        printf 'PROCESS\tcli-test\t1\t\tlarge-update\n'
        printf 'TIMESTAMP\t400\n'
        printf 'MESSAGE\t0\n'
        printf 'EVENT\t3\t7\n'
        printf 'db\nbig\nid02500\n'
        printf '{"v":1}'
        printf 'END\n'
    } | "$BIN" --store "$LARGE_STORE" commit-batch | awk -F '\t' '$1 == "OK" { print $2 }'
)
[ -n "$large_c2" ] || fail "large tree update commit was not created"
printf '{"v":1}' >"$LARGE_ROOT/expected-large-v1"
"$BIN" --store "$LARGE_STORE" show "$large_c2:db/big/id02500" >"$LARGE_ROOT/show-large-v1"
cmp -s "$LARGE_ROOT/expected-large-v1" "$LARGE_ROOT/show-large-v1" ||
    fail "large tree update did not preserve updated blob"

echo "test_cli_features: passed"
