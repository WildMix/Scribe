/*
 * Unit tests for core Scribe utilities and repository behavior.
 *
 * These tests exercise low-level encoders, hash helpers, arena allocation,
 * canonical tree serialization, the SPSC queue, repository commits, fsck, the
 * pipe protocol, and object iteration without requiring MongoDB.
 */
#include "core/internal.h"
#include "util/arena.h"
#include "util/hex.h"
#include "util/leb128.h"
#include "util/queue.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Verifies representative unsigned integers round-trip through the LEB128
 * encoder/decoder and that the decoder reports exactly the bytes consumed.
 */
void test_leb128_round_trip(void) {
    uint64_t values[] = {0, 1, 127, 128, 255, 16384, UINT64_C(0xffffffff), UINT64_MAX};
    size_t i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        uint8_t buf[10];
        uint64_t decoded = 0;
        size_t used = 0;
        size_t len = scribe_leb128_encode(values[i], buf);
        TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_leb128_decode(buf, len, &decoded, &used));
        TEST_ASSERT_EQUAL_UINT64(values[i], decoded);
        TEST_ASSERT_EQUAL_size_t(len, used);
    }
}

/*
 * Verifies that the LEB128 decoder rejects encodings that exceed the 10-byte
 * maximum for uint64_t values.
 */
void test_leb128_rejects_overlong(void) {
    uint8_t overlong[11];
    uint64_t decoded = 0;
    size_t used = 0;

    memset(overlong, 0x80, sizeof(overlong));
    TEST_ASSERT_EQUAL(SCRIBE_ECORRUPT, scribe_leb128_decode(overlong, sizeof(overlong), &decoded, &used));
}

/*
 * Verifies canonical lowercase hash rendering and parsing, plus rejection of an
 * invalid hash string.
 */
void test_hex_round_trip(void) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    uint8_t parsed[SCRIBE_HASH_SIZE];
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    size_t i;

    for (i = 0; i < SCRIBE_HASH_SIZE; i++) {
        hash[i] = (uint8_t)i;
    }
    scribe_hash_to_hex(hash, hex);
    TEST_ASSERT_EQUAL_STRING("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", hex);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_hash_from_hex(hex, parsed));
    TEST_ASSERT_EQUAL_MEMORY(hash, parsed, SCRIBE_HASH_SIZE);
    TEST_ASSERT_EQUAL(SCRIBE_EHASH, scribe_hash_from_hex("ABC", parsed));
}

void test_hash_set_add_and_has_deduplicates(void) {
    scribe_hash_set set;
    uint8_t hash1[SCRIBE_HASH_SIZE];
    uint8_t hash2[SCRIBE_HASH_SIZE];
    int already = 1;

    memset(&set, 0, sizeof(set));
    memset(hash1, 0x11, sizeof(hash1));
    memset(hash2, 0x22, sizeof(hash2));
    TEST_ASSERT_FALSE(scribe_hash_set_has(&set, hash1));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_hash_set_add(&set, hash1, &already));
    TEST_ASSERT_FALSE(already);
    TEST_ASSERT_TRUE(scribe_hash_set_has(&set, hash1));
    TEST_ASSERT_FALSE(scribe_hash_set_has(&set, hash2));
    TEST_ASSERT_EQUAL_size_t(1, set.count);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_hash_set_add(&set, hash1, &already));
    TEST_ASSERT_TRUE(already);
    TEST_ASSERT_EQUAL_size_t(1, set.count);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_hash_set_add(&set, hash2, &already));
    TEST_ASSERT_FALSE(already);
    TEST_ASSERT_EQUAL_size_t(2, set.count);
    scribe_hash_set_destroy(&set);
}

void test_ndjson_line_event_round_trip_and_rejects_malformed(void) {
    const char text[] = "quote \" slash \\ tab\t";
    char *event = NULL;
    size_t event_len = 0;
    char *parsed = NULL;
    size_t parsed_len = 0;

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ndjson_line_event(text, sizeof(text) - 1u, &event, &event_len));
    TEST_ASSERT_NOT_NULL(event);
    TEST_ASSERT_GREATER_THAN_size_t(0, event_len);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ndjson_parse_line_event(event, event_len, &parsed, &parsed_len));
    TEST_ASSERT_EQUAL_size_t(sizeof(text) - 1u, parsed_len);
    TEST_ASSERT_EQUAL_MEMORY(text, parsed, parsed_len);
    free(event);
    free(parsed);

    TEST_ASSERT_EQUAL(SCRIBE_EPROTOCOL, scribe_ndjson_parse_line_event("{\"line\":", 8u, &parsed, &parsed_len));
}

void test_timestamp_parse_and_format(void) {
    int64_t nanos = 0;
    char formatted[64];

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_parse_time_arg("1779540286000000002", &nanos));
    TEST_ASSERT_EQUAL_INT64(INT64_C(1779540286000000002), nanos);
    scribe_format_time_utc(nanos, formatted, sizeof(formatted));
    TEST_ASSERT_EQUAL_STRING("2026-05-23T12:44:46.000000002Z", formatted);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_parse_time_arg("1970-01-01T00:00:00.000000250Z", &nanos));
    TEST_ASSERT_EQUAL_INT64(250, nanos);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_parse_time_arg("1970-01-01", &nanos));
    TEST_ASSERT_EQUAL_INT64(0, nanos);
    TEST_ASSERT_EQUAL(SCRIBE_EINVAL, scribe_parse_time_arg("2026-02-31T00:00:00Z", &nanos));
    TEST_ASSERT_EQUAL(SCRIBE_EINVAL, scribe_parse_time_arg("2026-05-23T10:10:25.1234567891Z", &nanos));
}

/*
 * Verifies arena allocation alignment and reset behavior. Reset should make the
 * next allocation reuse the same backing space.
 */
void test_arena_alloc_reset(void) {
    scribe_arena arena;
    void *a;
    void *b;

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 64));
    a = scribe_arena_alloc(&arena, 5, 8);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(0, ((uintptr_t)a) % 8u);
    scribe_arena_reset(&arena);
    b = scribe_arena_alloc(&arena, 5, 8);
    TEST_ASSERT_EQUAL_PTR(a, b);
    scribe_arena_destroy(&arena);
}

/*
 * Verifies that tree serialization sorts entries by name and that parsing
 * preserves that canonical order.
 */
void test_tree_serialization_is_sorted(void) {
    scribe_arena arena;
    scribe_tree_entry entries[2];
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    scribe_tree_entry *parsed = NULL;
    size_t parsed_count = 0;

    memset(entries, 0, sizeof(entries));
    entries[0].type = SCRIBE_OBJECT_BLOB;
    entries[0].name = "z";
    entries[0].name_len = 1;
    entries[1].type = SCRIBE_OBJECT_BLOB;
    entries[1].name = "a";
    entries[1].name_len = 1;
    memset(entries[0].hash, 1, SCRIBE_HASH_SIZE);
    memset(entries[1].hash, 2, SCRIBE_HASH_SIZE);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 1024));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_serialize(entries, 2, &arena, &payload, &payload_len));
    TEST_ASSERT_GREATER_THAN_size_t(0, payload_len);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_parse(payload, payload_len, &arena, &parsed, &parsed_count));
    TEST_ASSERT_EQUAL_size_t(2, parsed_count);
    TEST_ASSERT_EQUAL_STRING("a", parsed[0].name);
    TEST_ASSERT_EQUAL_STRING("z", parsed[1].name);
    scribe_arena_destroy(&arena);
}

/*
 * Verifies FIFO behavior of the queue's nonblocking pop path after two blocking
 * pushes into a small queue.
 */
void test_queue_fifo_try_pop(void) {
    scribe_spsc_queue q;
    int a = 1;
    int b = 2;
    void *out = NULL;

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_spsc_queue_init(&q, 2, 0));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_spsc_queue_push(&q, &a));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_spsc_queue_push(&q, &b));
    TEST_ASSERT_TRUE(scribe_spsc_queue_try_pop(&q, &out));
    TEST_ASSERT_EQUAL_PTR(&a, out);
    TEST_ASSERT_TRUE(scribe_spsc_queue_try_pop(&q, &out));
    TEST_ASSERT_EQUAL_PTR(&b, out);
    TEST_ASSERT_FALSE(scribe_spsc_queue_try_pop(&q, &out));
    scribe_spsc_queue_destroy(&q);
}

/*
 * Creates a temporary repository directory in place from an mkdtemp template.
 */
static void make_temp_repo(char *tmpl) {
    char *created = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(created);
}

/*
 * Creates a repository, writes two commits that modify the same path, and runs
 * fsck to verify the resulting object graph.
 */
void test_repository_commit_and_fsck(void) {
    char tmpl[] = "/tmp/scribe-test-XXXXXX";
    char *repo;
    scribe_ctx *ctx = NULL;
    const char *path1[] = {"db", "users", "\"alice\""};
    const char *path2[] = {"db", "users", "\"alice\""};
    const uint8_t payload1[] = "{\"_id\":\"alice\",\"role\":\"user\"}";
    const uint8_t payload2[] = "{\"_id\":\"alice\",\"role\":\"admin\"}";
    scribe_change_event events1[1];
    scribe_change_event events2[1];
    scribe_change_batch batch;
    uint8_t c1[SCRIBE_HASH_SIZE];
    uint8_t c2[SCRIBE_HASH_SIZE];

    make_temp_repo(tmpl);
    repo = tmpl;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(repo));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(repo, 1, &ctx));

    memset(events1, 0, sizeof(events1));
    events1[0].path = path1;
    events1[0].path_len = 3;
    events1[0].payload = payload1;
    events1[0].payload_len = sizeof(payload1) - 1u;
    memset(&batch, 0, sizeof(batch));
    batch.events = events1;
    batch.event_count = 1;
    batch.author = (scribe_identity){"tester", "", "test"};
    batch.committer = (scribe_identity){"scribe-test", "", "scribe"};
    batch.process = (scribe_process_info){"unit", "1", "", "case-1"};
    batch.timestamp_unix_nanos = 1;
    batch.message = "initial";
    batch.message_len = 7;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, c1));

    memset(events2, 0, sizeof(events2));
    events2[0].path = path2;
    events2[0].path_len = 3;
    events2[0].payload = payload2;
    events2[0].payload_len = sizeof(payload2) - 1u;
    batch.events = events2;
    batch.timestamp_unix_nanos = 2;
    batch.message = "update";
    batch.message_len = 6;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, c2));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(c1, c2, SCRIBE_HASH_SIZE));
    scribe_close(ctx);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(repo, 0, &ctx));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));
    scribe_close(ctx);
}

void test_commit_batch_accepts_empty_blob_payload(void) {
    char tmpl[] = "/tmp/scribe-empty-blob-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const char *path[] = {"empty"};
    const uint8_t empty_payload = 0;
    scribe_change_event event;
    scribe_change_batch batch;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    scribe_arena arena;
    scribe_commit_view view;
    scribe_object tree_obj;
    scribe_tree_entry *entries = NULL;
    size_t count = 0;
    scribe_object blob_obj;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));

    memset(&event, 0, sizeof(event));
    event.path = path;
    event.path_len = 1;
    event.payload = &empty_payload;
    event.payload_len = 0;
    memset(&batch, 0, sizeof(batch));
    batch.events = &event;
    batch.event_count = 1;
    batch.author = (scribe_identity){"tester", "", "test"};
    batch.committer = (scribe_identity){"scribe-test", "", "scribe"};
    batch.process = (scribe_process_info){"unit", "1", "", "empty-blob"};
    batch.timestamp_unix_nanos = 1;
    batch.message = "empty blob";
    batch.message_len = 10;

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, commit_hash));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, commit_hash, &arena, &view));

    memset(&tree_obj, 0, sizeof(tree_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, view.root_tree, &tree_obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_TREE, tree_obj.type);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_parse(tree_obj.payload, tree_obj.payload_len, &arena, &entries, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_BLOB, entries[0].type);
    TEST_ASSERT_EQUAL_STRING("empty", entries[0].name);

    memset(&blob_obj, 0, sizeof(blob_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, entries[0].hash, &blob_obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_BLOB, blob_obj.type);
    TEST_ASSERT_EQUAL_size_t(0, blob_obj.payload_len);

    scribe_object_free(&blob_obj);
    scribe_object_free(&tree_obj);
    scribe_arena_destroy(&arena);
    scribe_close(ctx);
}

void test_ingest_json_explicit_put_empty_blob_and_delete(void) {
    char tmpl[] = "/tmp/scribe-json-ingest-test-XXXXXX";
    const uint8_t put_body[] =
        "{\"message\":\"json put empty\",\"events\":[{\"path\":[\"json\",\"empty\"],\"op\":\"put\",\"payload\":\"\"}]}";
    const uint8_t delete_body[] =
        "{\"message\":\"json delete empty\",\"events\":[{\"path\":[\"json\",\"empty\"],\"op\":\"delete\"}]}";
    scribe_ctx *ctx = NULL;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    scribe_arena arena;
    scribe_commit_view view;
    scribe_path_resolution res;
    scribe_object blob_obj;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_json_commit(ctx, put_body, sizeof(put_body) - 1u, commit_hash));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, commit_hash, &arena, &view));
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "json/empty", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_BLOB, res.state);
    memset(&blob_obj, 0, sizeof(blob_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, res.hash, &blob_obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_BLOB, blob_obj.type);
    TEST_ASSERT_EQUAL_size_t(0, blob_obj.payload_len);
    scribe_object_free(&blob_obj);
    scribe_arena_destroy(&arena);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_json_commit(ctx, delete_body, sizeof(delete_body) - 1u, commit_hash));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, commit_hash, &arena, &view));
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "json/empty", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_ABSENT, res.state);
    scribe_arena_destroy(&arena);

    scribe_close(ctx);
}

void test_ingest_content_put_empty_blob_and_delete(void) {
    char tmpl[] = "/tmp/scribe-content-ingest-test-XXXXXX";
    const char *record_path[] = {"content", "record"};
    const char *empty_path[] = {"content", "empty"};
    const char *invalid_path[] = {"content", ""};
    const uint8_t payload[] = "{\"name\":\"Alice\"}\n";
    const char message[] = "raw content import";
    scribe_ctx *ctx = NULL;
    uint8_t put_commit[SCRIBE_HASH_SIZE];
    uint8_t retry_commit[SCRIBE_HASH_SIZE];
    uint8_t empty_commit[SCRIBE_HASH_SIZE];
    uint8_t delete_commit[SCRIBE_HASH_SIZE];
    scribe_arena arena;
    scribe_commit_view view;
    scribe_path_resolution res;
    scribe_object blob_obj;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_content_commit(ctx, record_path, 2u, SCRIBE_INGEST_OP_PUT, payload,
                                                              sizeof(payload) - 1u, message, put_commit));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, put_commit, &arena, &view));
    TEST_ASSERT_EQUAL_size_t(sizeof(message) - 1u, view.message_len);
    TEST_ASSERT_EQUAL_MEMORY(message, view.message, sizeof(message) - 1u);
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "content/record", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_BLOB, res.state);
    memset(&blob_obj, 0, sizeof(blob_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, res.hash, &blob_obj));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload) - 1u, blob_obj.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, blob_obj.payload, sizeof(payload) - 1u);
    scribe_object_free(&blob_obj);
    scribe_arena_destroy(&arena);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_content_commit(ctx, record_path, 2u, SCRIBE_INGEST_OP_PUT, payload,
                                                              sizeof(payload) - 1u, "retry", retry_commit));
    TEST_ASSERT_EQUAL_MEMORY(put_commit, retry_commit, SCRIBE_HASH_SIZE);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_content_commit(ctx, empty_path, 2u, SCRIBE_INGEST_OP_PUT, NULL, 0u, NULL,
                                                              empty_commit));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, empty_commit, &arena, &view));
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "content/empty", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_BLOB, res.state);
    memset(&blob_obj, 0, sizeof(blob_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, res.hash, &blob_obj));
    TEST_ASSERT_EQUAL_size_t(0u, blob_obj.payload_len);
    scribe_object_free(&blob_obj);
    scribe_arena_destroy(&arena);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_content_commit(ctx, record_path, 2u, SCRIBE_INGEST_OP_DELETE, NULL, 0u,
                                                              "delete record", delete_commit));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, delete_commit, &arena, &view));
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "content/record", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_ABSENT, res.state);
    scribe_arena_destroy(&arena);

    TEST_ASSERT_EQUAL(SCRIBE_EPATH, scribe_ingest_content_commit(ctx, invalid_path, 2u, SCRIBE_INGEST_OP_PUT, payload,
                                                                 sizeof(payload) - 1u, NULL, retry_commit));
    TEST_ASSERT_EQUAL(SCRIBE_EINVAL, scribe_ingest_content_commit(ctx, record_path, 2u, SCRIBE_INGEST_OP_DELETE,
                                                                  payload, sizeof(payload) - 1u, NULL, retry_commit));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));
    scribe_close(ctx);
}

void test_ingest_content_large_binary_payload(void) {
    enum { PAYLOAD_LEN = 64 * 1024 };
    char tmpl[] = "/tmp/scribe-content-large-test-XXXXXX";
    const char *path[] = {"content", "large"};
    uint8_t *payload = (uint8_t *)malloc(PAYLOAD_LEN);
    scribe_ctx *ctx = NULL;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    uint8_t retry_hash[SCRIBE_HASH_SIZE];
    scribe_arena arena;
    scribe_commit_view view;
    scribe_path_resolution res;
    scribe_object blob_obj;
    size_t i;

    TEST_ASSERT_NOT_NULL(payload);
    for (i = 0u; i < PAYLOAD_LEN; i++) {
        payload[i] = (uint8_t)((i * 31u + 17u) & 0xffu);
    }
    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_content_commit(ctx, path, 2u, SCRIBE_INGEST_OP_PUT, payload, PAYLOAD_LEN,
                                                              "large binary", commit_hash));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, commit_hash, &arena, &view));
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "content/large", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_BLOB, res.state);
    memset(&blob_obj, 0, sizeof(blob_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, res.hash, &blob_obj));
    TEST_ASSERT_EQUAL_size_t(PAYLOAD_LEN, blob_obj.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, blob_obj.payload, PAYLOAD_LEN);
    scribe_object_free(&blob_obj);
    scribe_arena_destroy(&arena);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_content_commit(ctx, path, 2u, SCRIBE_INGEST_OP_PUT, payload, PAYLOAD_LEN,
                                                              "large binary retry", retry_hash));
    TEST_ASSERT_EQUAL_MEMORY(commit_hash, retry_hash, SCRIBE_HASH_SIZE);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));

    scribe_close(ctx);
    free(payload);
}

/*
 * Feeds a real pipe protocol BATCH frame through scribe_pipe_commit_batch() and
 * checks that the legacy v1 command protocol remains accepted.
 */
void test_pipe_commit_batch(void) {
    char tmpl[] = "/tmp/scribe-pipe-test-XXXXXX";
    const char input[] = "BATCH\t1\t1\n"
                         "AUTHOR\ttester\t\ttest\n"
                         "COMMITTER\tscribe-test\t\tscribe\n"
                         "PROCESS\tpipe\t1\t\tcase\n"
                         "TIMESTAMP\t100\n"
                         "MESSAGE\t0\n"
                         "EVENT\t3\t13\n"
                         "db\n"
                         "users\n"
                         "\"bob\"\n"
                         "{\"_id\":\"bob\"}"
                         "END\n";
    FILE *in;
    FILE *out;
    char *out_buf = NULL;
    size_t out_len = 0;
    scribe_ctx *ctx = NULL;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    in = fmemopen((void *)input, sizeof(input) - 1u, "rb");
    TEST_ASSERT_NOT_NULL(in);
    out = open_memstream(&out_buf, &out_len);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pipe_commit_batch(ctx, in, out));
    fclose(in);
    fclose(out);
    TEST_ASSERT_NOT_NULL(out_buf);
    TEST_ASSERT_GREATER_THAN_size_t(3, out_len);
    TEST_ASSERT_EQUAL_MEMORY("OK\t", out_buf, 3);
    free(out_buf);
    scribe_close(ctx);
}

void test_pipe_v2_put_empty_blob_and_delete(void) {
    char tmpl[] = "/tmp/scribe-pipe-v2-test-XXXXXX";
    const char input[] = "BATCH\t2\t1\n"
                         "AUTHOR\ttester\t\ttest\n"
                         "COMMITTER\tscribe-test\t\tscribe\n"
                         "PROCESS\tpipe\t2\t\tput-empty\n"
                         "TIMESTAMP\t100\n"
                         "MESSAGE\t0\n"
                         "EVENT\tput\t1\t0\n"
                         "empty\n"
                         "END\n"
                         "BATCH\t2\t1\n"
                         "AUTHOR\ttester\t\ttest\n"
                         "COMMITTER\tscribe-test\t\tscribe\n"
                         "PROCESS\tpipe\t2\t\tdelete-empty\n"
                         "TIMESTAMP\t101\n"
                         "MESSAGE\t0\n"
                         "EVENT\tdelete\t1\n"
                         "empty\n"
                         "END\n";
    FILE *in;
    FILE *out;
    char *out_buf = NULL;
    size_t out_len = 0;
    char first_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char second_hex[SCRIBE_HEX_HASH_SIZE + 1];
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    const char *second_ok;
    scribe_ctx *ctx = NULL;
    scribe_arena arena;
    scribe_commit_view view;
    scribe_path_resolution res;
    scribe_object blob_obj;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    in = fmemopen((void *)input, sizeof(input) - 1u, "rb");
    TEST_ASSERT_NOT_NULL(in);
    out = open_memstream(&out_buf, &out_len);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pipe_commit_batch(ctx, in, out));
    fclose(in);
    fclose(out);
    TEST_ASSERT_NOT_NULL(out_buf);
    TEST_ASSERT_GREATER_THAN_size_t(3u + SCRIBE_HEX_HASH_SIZE, out_len);
    TEST_ASSERT_EQUAL_MEMORY("OK\t", out_buf, 3u);
    memcpy(first_hex, out_buf + 3u, SCRIBE_HEX_HASH_SIZE);
    first_hex[SCRIBE_HEX_HASH_SIZE] = '\0';
    second_ok = strstr(out_buf + 3u + SCRIBE_HEX_HASH_SIZE, "OK\t");
    TEST_ASSERT_NOT_NULL(second_ok);
    memcpy(second_hex, second_ok + 3u, SCRIBE_HEX_HASH_SIZE);
    second_hex[SCRIBE_HEX_HASH_SIZE] = '\0';
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_hash_from_hex(first_hex, commit_hash));

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, commit_hash, &arena, &view));
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "empty", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_BLOB, res.state);
    memset(&blob_obj, 0, sizeof(blob_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, res.hash, &blob_obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_BLOB, blob_obj.type);
    TEST_ASSERT_EQUAL_size_t(0, blob_obj.payload_len);
    scribe_object_free(&blob_obj);
    scribe_arena_destroy(&arena);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_hash_from_hex(second_hex, commit_hash));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, commit_hash, &arena, &view));
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "empty", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_ABSENT, res.state);
    scribe_arena_destroy(&arena);

    free(out_buf);
    scribe_close(ctx);
}

typedef struct {
    uint8_t expected[SCRIBE_HASH_SIZE];
    size_t count;
    int saw_expected;
} object_iter_test_state;

/*
 * Object-iterator callback used by the iterator test. It counts all visited
 * hashes and records whether the expected loose object appeared.
 */
static scribe_error_t count_object_visit(const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    object_iter_test_state *state = (object_iter_test_state *)user;

    state->count++;
    if (memcmp(hash, state->expected, SCRIBE_HASH_SIZE) == 0) {
        state->saw_expected = 1;
    }
    return SCRIBE_OK;
}

/*
 * Re-ingests the same JSON bytes at the same path and verifies that the retry
 * returns the existing commit without adding any object or history node. A
 * changed payload must still create a child commit and replace the path value.
 */
void test_ingest_json_skips_unchanged_payload_at_path(void) {
    char tmpl[] = "/tmp/scribe-json-ingest-noop-test-XXXXXX";
    const uint8_t initial_body[] =
        "{\"timestamp_unix_nanos\":1,\"message\":\"initial\",\"events\":[{\"path\":[\"json\",\"record\"],"
        "\"op\":\"put\",\"payload\":\"eyJ2IjoxfQ==\"}]}";
    const uint8_t retry_body[] =
        "{\"timestamp_unix_nanos\":2,\"message\":\"retry\",\"events\":[{\"path\":[\"json\",\"record\"],"
        "\"op\":\"put\",\"payload\":\"eyJ2IjoxfQ==\"}]}";
    const uint8_t changed_body[] =
        "{\"timestamp_unix_nanos\":3,\"message\":\"changed\",\"events\":[{\"path\":[\"json\",\"record\"],"
        "\"op\":\"put\",\"payload\":\"eyJ2IjoyfQ==\"}]}";
    const uint8_t expected_changed[] = "{\"v\":2}";
    scribe_ctx *ctx = NULL;
    uint8_t initial_commit[SCRIBE_HASH_SIZE];
    uint8_t retry_commit[SCRIBE_HASH_SIZE];
    uint8_t changed_commit[SCRIBE_HASH_SIZE];
    uint8_t head[SCRIBE_HASH_SIZE];
    object_iter_test_state before_retry;
    object_iter_test_state after_retry;
    scribe_arena arena;
    scribe_commit_view view;
    scribe_path_resolution res;
    scribe_object blob_obj;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));

    TEST_ASSERT_EQUAL(SCRIBE_OK,
                      scribe_ingest_json_commit(ctx, initial_body, sizeof(initial_body) - 1u, initial_commit));
    memset(&before_retry, 0, sizeof(before_retry));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, count_object_visit, &before_retry));

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_ingest_json_commit(ctx, retry_body, sizeof(retry_body) - 1u, retry_commit));
    TEST_ASSERT_EQUAL_MEMORY(initial_commit, retry_commit, SCRIBE_HASH_SIZE);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_refs_read(ctx, "refs/heads/main", head));
    TEST_ASSERT_EQUAL_MEMORY(initial_commit, head, SCRIBE_HASH_SIZE);
    memset(&after_retry, 0, sizeof(after_retry));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, count_object_visit, &after_retry));
    TEST_ASSERT_EQUAL_size_t(before_retry.count, after_retry.count);

    TEST_ASSERT_EQUAL(SCRIBE_OK,
                      scribe_ingest_json_commit(ctx, changed_body, sizeof(changed_body) - 1u, changed_commit));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(initial_commit, changed_commit, SCRIBE_HASH_SIZE));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_refs_read(ctx, "refs/heads/main", head));
    TEST_ASSERT_EQUAL_MEMORY(changed_commit, head, SCRIBE_HASH_SIZE);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_arena_init(&arena, 4096u));
    memset(&view, 0, sizeof(view));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_read_commit_view(ctx, changed_commit, &arena, &view));
    TEST_ASSERT_TRUE(view.has_parent);
    TEST_ASSERT_EQUAL_MEMORY(initial_commit, view.parent, SCRIBE_HASH_SIZE);
    memset(&res, 0, sizeof(res));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_tree_resolve_path(ctx, view.root_tree, "json/record", &res));
    TEST_ASSERT_EQUAL(SCRIBE_PATH_BLOB, res.state);
    memset(&blob_obj, 0, sizeof(blob_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, res.hash, &blob_obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_BLOB, blob_obj.type);
    TEST_ASSERT_EQUAL_size_t(sizeof(expected_changed) - 1u, blob_obj.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(expected_changed, blob_obj.payload, sizeof(expected_changed) - 1u);
    scribe_object_free(&blob_obj);
    scribe_arena_destroy(&arena);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));
    scribe_close(ctx);
}

/*
 * Writes one loose blob without publishing a commit and verifies that the object
 * iterator and compressed-size query can see it.
 */
void test_object_iterator_and_compressed_size(void) {
    char tmpl[] = "/tmp/scribe-object-iter-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const uint8_t payload[] = "dangling";
    uint8_t hash[SCRIBE_HASH_SIZE];
    size_t compressed_size = 0;
    object_iter_test_state state;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_write(ctx, SCRIBE_OBJECT_BLOB, payload, sizeof(payload) - 1u, hash));
    memset(&state, 0, sizeof(state));
    scribe_hash_copy(state.expected, hash);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, count_object_visit, &state));
    TEST_ASSERT_EQUAL_size_t(1, state.count);
    TEST_ASSERT_TRUE(state.saw_expected);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_compressed_size(ctx, hash, &compressed_size));
    TEST_ASSERT_GREATER_THAN_size_t(0, compressed_size);
    scribe_close(ctx);
}

static void pack_id_hex(const uint8_t id[16], char out[33]) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < 16u; i++) {
        out[i * 2u] = digits[id[i] >> 4u];
        out[i * 2u + 1u] = digits[id[i] & 0x0fu];
    }
    out[32] = '\0';
}

static char *test_pack_path(scribe_ctx *ctx, const uint8_t pack_id[16], const char *suffix) {
    char id_hex[33];
    char name[40];
    char *packs;
    char *path;

    pack_id_hex(pack_id, id_hex);
    snprintf(name, sizeof(name), "%s%s", id_hex, suffix);
    packs = scribe_path_join(ctx->repo_path, "packs");
    TEST_ASSERT_NOT_NULL(packs);
    path = scribe_path_join(packs, name);
    free(packs);
    TEST_ASSERT_NOT_NULL(path);
    return path;
}

/*
 * Writes an object directly to a pack and verifies the normal object-store API
 * can read, iterate, size, and fsck it even though no loose object exists.
 */
void test_pack_only_object_read_iter_and_compressed_size(void) {
    char tmpl[] = "/tmp/scribe-pack-only-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const uint8_t payload[] = "packed";
    scribe_pack_object obj;
    scribe_object read_obj;
    size_t compressed_size = 0;
    object_iter_test_state state;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    memset(&obj, 0, sizeof(obj));
    obj.type = SCRIBE_OBJECT_BLOB;
    obj.payload = payload;
    obj.payload_len = sizeof(payload) - 1u;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, &obj, 1, NULL));

    memset(&read_obj, 0, sizeof(read_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, obj.hash, &read_obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_BLOB, read_obj.type);
    TEST_ASSERT_EQUAL_size_t(sizeof(payload) - 1u, read_obj.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, read_obj.payload, sizeof(payload) - 1u);
    scribe_object_free(&read_obj);

    memset(&state, 0, sizeof(state));
    scribe_hash_copy(state.expected, obj.hash);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, count_object_visit, &state));
    TEST_ASSERT_EQUAL_size_t(1, state.count);
    TEST_ASSERT_TRUE(state.saw_expected);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_compressed_size(ctx, obj.hash, &compressed_size));
    TEST_ASSERT_GREATER_THAN_size_t(0, compressed_size);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));
    scribe_close(ctx);
}

void test_packed_object_read_uses_ranged_pack_read(void) {
    char tmpl[] = "/tmp/scribe-pack-ranged-read-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const uint8_t payload1[] = "packed-ranged-one";
    const uint8_t payload2[] = "packed-ranged-two";
    scribe_pack_object objects[2];
    scribe_object read_obj;
    scribe_perf_trace trace;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    memset(objects, 0, sizeof(objects));
    objects[0].type = SCRIBE_OBJECT_BLOB;
    objects[0].payload = payload1;
    objects[0].payload_len = sizeof(payload1) - 1u;
    objects[1].type = SCRIBE_OBJECT_BLOB;
    objects[1].payload = payload2;
    objects[1].payload_len = sizeof(payload2) - 1u;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, objects, 2u, NULL));

    scribe_perf_trace_reset();
    memset(&read_obj, 0, sizeof(read_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, objects[0].hash, &read_obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_BLOB, read_obj.type);
    TEST_ASSERT_EQUAL_MEMORY(payload1, read_obj.payload, sizeof(payload1) - 1u);
    scribe_object_free(&read_obj);
    scribe_perf_trace_snapshot(&trace);

    TEST_ASSERT_EQUAL_UINT64(1, trace.packed_object_reads);
    TEST_ASSERT_EQUAL_UINT64(0, trace.full_pack_reads);
    TEST_ASSERT_EQUAL_UINT64(1, trace.pack_file_opens);
    TEST_ASSERT_GREATER_THAN_UINT64(0, trace.pack_file_bytes_read);
    TEST_ASSERT_GREATER_THAN_UINT64(0, trace.pack_decompressed_bytes);
    scribe_close(ctx);
}

void test_list_objects_uses_pack_metadata_without_packed_object_reads(void) {
    char tmpl[] = "/tmp/scribe-list-pack-meta-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const uint8_t payloads[3][8] = {"alpha", "beta", "gamma"};
    scribe_pack_object objects[3];
    scribe_perf_trace trace;
    size_t i;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    memset(objects, 0, sizeof(objects));
    for (i = 0; i < 3u; i++) {
        objects[i].type = SCRIBE_OBJECT_BLOB;
        objects[i].payload = payloads[i];
        objects[i].payload_len = strlen((const char *)payloads[i]);
    }
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, objects, 3u, NULL));

    scribe_perf_trace_reset();
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_list_objects(ctx, 0, 0, "%H %T %S %C"));
    scribe_perf_trace_snapshot(&trace);

    TEST_ASSERT_EQUAL_UINT64(0, trace.packed_object_reads);
    TEST_ASSERT_EQUAL_UINT64(0, trace.full_pack_reads);
    TEST_ASSERT_EQUAL_UINT64(1, trace.pack_index_parses);
    TEST_ASSERT_EQUAL_UINT64(0, trace.pack_file_bytes_read);
    scribe_close(ctx);
}

/*
 * Writes the same object through the loose and pack paths, deletes the loose
 * file, and verifies reads fall through to the packed copy.
 */
void test_pack_read_after_loose_delete(void) {
    char tmpl[] = "/tmp/scribe-pack-loose-delete-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const uint8_t payload[] = "loose-to-pack";
    uint8_t loose_hash[SCRIBE_HASH_SIZE];
    scribe_pack_object obj;
    scribe_object read_obj;
    char *loose_path;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    TEST_ASSERT_EQUAL(SCRIBE_OK,
                      scribe_object_write(ctx, SCRIBE_OBJECT_BLOB, payload, sizeof(payload) - 1u, loose_hash));
    memset(&obj, 0, sizeof(obj));
    obj.type = SCRIBE_OBJECT_BLOB;
    obj.payload = payload;
    obj.payload_len = sizeof(payload) - 1u;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, &obj, 1, NULL));
    TEST_ASSERT_EQUAL_MEMORY(loose_hash, obj.hash, SCRIBE_HASH_SIZE);

    loose_path = scribe_object_path(ctx, loose_hash);
    TEST_ASSERT_NOT_NULL(loose_path);
    TEST_ASSERT_EQUAL_INT(0, unlink(loose_path));
    free(loose_path);

    memset(&read_obj, 0, sizeof(read_obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, loose_hash, &read_obj));
    TEST_ASSERT_EQUAL_MEMORY(payload, read_obj.payload, sizeof(payload) - 1u);
    scribe_object_free(&read_obj);
    scribe_close(ctx);
}

typedef struct {
    uint8_t *hashes;
    size_t count;
    size_t cap;
} hash_collect_state;

static scribe_error_t collect_hash_visit(const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    hash_collect_state *state = (hash_collect_state *)user;
    uint8_t *grown;

    if (state->count == state->cap) {
        size_t new_cap = state->cap == 0u ? 16u : state->cap * 2u;
        grown = (uint8_t *)realloc(state->hashes, new_cap * SCRIBE_HASH_SIZE);
        if (grown == NULL) {
            return SCRIBE_ENOMEM;
        }
        state->hashes = grown;
        state->cap = new_cap;
    }
    memcpy(state->hashes + state->count * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE);
    state->count++;
    return SCRIBE_OK;
}

static void free_pack_objects(scribe_pack_object *objects, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        free((void *)objects[i].payload);
    }
    free(objects);
}

/*
 * Converts a real committed repository into pack-only storage by packing every
 * loose object, deleting the loose files, and then running fsck and object reads
 * through the normal APIs. This protects the core v2 invariant that commands do
 * not care whether an object is loose or packed.
 */
void test_pack_only_repository_history_reads(void) {
    char tmpl[] = "/tmp/scribe-pack-history-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const char *path[] = {"db", "users", "\"pack-user\""};
    const uint8_t payload[] = "{\"_id\":\"pack-user\"}";
    scribe_change_event event;
    scribe_change_batch batch;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    hash_collect_state collected;
    scribe_pack_object *objects = NULL;
    size_t i;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    memset(&event, 0, sizeof(event));
    event.path = path;
    event.path_len = 3;
    event.payload = payload;
    event.payload_len = sizeof(payload) - 1u;
    memset(&batch, 0, sizeof(batch));
    batch.events = &event;
    batch.event_count = 1;
    batch.author = (scribe_identity){"tester", "", "test"};
    batch.committer = (scribe_identity){"scribe-test", "", "scribe"};
    batch.process = (scribe_process_info){"unit", "1", "", "pack-history"};
    batch.timestamp_unix_nanos = 1;
    batch.message = "pack history";
    batch.message_len = 12;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, commit_hash));

    memset(&collected, 0, sizeof(collected));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, collect_hash_visit, &collected));
    TEST_ASSERT_GREATER_THAN_size_t(0, collected.count);
    objects = (scribe_pack_object *)calloc(collected.count, sizeof(*objects));
    TEST_ASSERT_NOT_NULL(objects);
    for (i = 0; i < collected.count; i++) {
        scribe_object obj;
        memset(&obj, 0, sizeof(obj));
        TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, collected.hashes + i * SCRIBE_HASH_SIZE, &obj));
        objects[i].type = obj.type;
        objects[i].payload_len = obj.payload_len;
        objects[i].payload = (uint8_t *)malloc(obj.payload_len == 0u ? 1u : obj.payload_len);
        TEST_ASSERT_NOT_NULL(objects[i].payload);
        if (obj.payload_len != 0u) {
            memcpy((void *)objects[i].payload, obj.payload, obj.payload_len);
        }
        scribe_object_free(&obj);
    }
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, objects, collected.count, NULL));
    for (i = 0; i < collected.count; i++) {
        char *loose_path;
        TEST_ASSERT_EQUAL_MEMORY(collected.hashes + i * SCRIBE_HASH_SIZE, objects[i].hash, SCRIBE_HASH_SIZE);
        loose_path = scribe_object_path(ctx, collected.hashes + i * SCRIBE_HASH_SIZE);
        TEST_ASSERT_NOT_NULL(loose_path);
        TEST_ASSERT_EQUAL_INT(0, unlink(loose_path));
        free(loose_path);
    }

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));
    {
        scribe_object commit_obj;
        memset(&commit_obj, 0, sizeof(commit_obj));
        TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, commit_hash, &commit_obj));
        TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_COMMIT, commit_obj.type);
        scribe_object_free(&commit_obj);
    }
    free_pack_objects(objects, collected.count);
    free(collected.hashes);
    scribe_close(ctx);
}

/*
 * Corrupts a published pack index and verifies validation fails. This exercises
 * the `.idx` publication boundary rather than the loose-object path.
 */
void test_pack_corrupt_index_fails_validation(void) {
    char tmpl[] = "/tmp/scribe-pack-corrupt-idx-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const uint8_t payload[] = "corrupt-index";
    scribe_pack_object obj;
    uint8_t pack_id[16];
    char *idx_path;
    FILE *f;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    memset(&obj, 0, sizeof(obj));
    obj.type = SCRIBE_OBJECT_BLOB;
    obj.payload = payload;
    obj.payload_len = sizeof(payload) - 1u;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, &obj, 1, pack_id));
    idx_path = test_pack_path(ctx, pack_id, ".idx");
    f = fopen(idx_path, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT('X', fputc('X', f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    free(idx_path);
    TEST_ASSERT_EQUAL(SCRIBE_ECORRUPT, scribe_pack_validate_all(ctx, NULL));
    scribe_close(ctx);
}

/*
 * Corrupts a published pack header and verifies fsck reports repository
 * corruption before trusting any object named by the pack index.
 */
void test_pack_corrupt_pack_fails_fsck(void) {
    char tmpl[] = "/tmp/scribe-pack-corrupt-pack-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const uint8_t payload[] = "corrupt-pack";
    scribe_pack_object obj;
    uint8_t pack_id[16];
    char *pack_path;
    FILE *f;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    memset(&obj, 0, sizeof(obj));
    obj.type = SCRIBE_OBJECT_BLOB;
    obj.payload = payload;
    obj.payload_len = sizeof(payload) - 1u;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, &obj, 1, pack_id));
    pack_path = test_pack_path(ctx, pack_id, ".pack");
    f = fopen(pack_path, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT('X', fputc('X', f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    free(pack_path);
    TEST_ASSERT_EQUAL(SCRIBE_ECORRUPT, scribe_cli_fsck(ctx));
    scribe_close(ctx);
}

/*
 * Exercises the streaming pack writer that Mongo bootstrap uses. The write
 * publishes no index until finish(), so successful reads after finish prove the
 * pack and idx publication path agree with the normal object-store API.
 */
void test_streaming_pack_writer_publishes_objects(void) {
    char tmpl[] = "/tmp/scribe-pack-stream-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    scribe_pack_writer *writer = NULL;
    const uint8_t payload1[] = "stream-one";
    const uint8_t payload2[] = "stream-two";
    uint8_t hash1[SCRIBE_HASH_SIZE];
    uint8_t hash2[SCRIBE_HASH_SIZE];
    scribe_object obj;
    size_t packed_objects = 0;
    uint64_t packed_bytes = 0;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_writer_begin(ctx, &writer));
    TEST_ASSERT_EQUAL(SCRIBE_OK,
                      scribe_pack_writer_add(writer, SCRIBE_OBJECT_BLOB, payload1, sizeof(payload1) - 1u, hash1));
    TEST_ASSERT_EQUAL(SCRIBE_OK,
                      scribe_pack_writer_add(writer, SCRIBE_OBJECT_BLOB, payload2, sizeof(payload2) - 1u, hash2));
    TEST_ASSERT_EQUAL(SCRIBE_OK,
                      scribe_pack_writer_add(writer, SCRIBE_OBJECT_BLOB, payload1, sizeof(payload1) - 1u, hash1));
    scribe_pack_writer_stats(writer, &packed_objects, &packed_bytes);
    TEST_ASSERT_EQUAL_size_t(2, packed_objects);
    TEST_ASSERT_TRUE(packed_bytes > 0u);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_writer_finish(&writer, NULL));
    TEST_ASSERT_NULL(writer);

    memset(&obj, 0, sizeof(obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, hash1, &obj));
    TEST_ASSERT_EQUAL_MEMORY(payload1, obj.payload, sizeof(payload1) - 1u);
    scribe_object_free(&obj);
    memset(&obj, 0, sizeof(obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, hash2, &obj));
    TEST_ASSERT_EQUAL_MEMORY(payload2, obj.payload, sizeof(payload2) - 1u);
    scribe_object_free(&obj);
    scribe_close(ctx);
}

typedef struct {
    size_t count;
} count_state;

static scribe_error_t count_loose_cb(const uint8_t hash[SCRIBE_HASH_SIZE], const char *path, time_t mtime,
                                     size_t compressed_size, void *user) {
    count_state *state = (count_state *)user;

    (void)hash;
    (void)path;
    (void)mtime;
    (void)compressed_size;
    state->count++;
    return SCRIBE_OK;
}

static scribe_error_t count_pack_file_cb(const uint8_t pack_id[16], void *user) {
    count_state *state = (count_state *)user;

    (void)pack_id;
    state->count++;
    return SCRIBE_OK;
}

void test_commit_batch_auto_pack_threshold_writes_pack_only_objects(void) {
    char tmpl[] = "/tmp/scribe-auto-pack-commit-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const char *path[] = {"db", "users", "\"auto-pack\""};
    const uint8_t payload[] = "{\"_id\":\"auto-pack\"}";
    scribe_change_event event;
    scribe_change_batch batch;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    uint8_t retry_hash[SCRIBE_HASH_SIZE];
    scribe_object obj;
    count_state loose;
    count_state packs;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    ctx->config.commit_storage = SCRIBE_COMMIT_STORAGE_AUTO;
    ctx->config.commit_pack_event_threshold = 1u;
    ctx->config.commit_pack_payload_threshold = 0u;

    memset(&event, 0, sizeof(event));
    event.path = path;
    event.path_len = 3;
    event.payload = payload;
    event.payload_len = sizeof(payload) - 1u;
    memset(&batch, 0, sizeof(batch));
    batch.events = &event;
    batch.event_count = 1;
    batch.author = (scribe_identity){"tester", "", "test"};
    batch.committer = (scribe_identity){"scribe-test", "", "scribe"};
    batch.process = (scribe_process_info){"unit", "1", "", "auto-pack"};
    batch.timestamp_unix_nanos = 1;
    batch.message = "auto pack";
    batch.message_len = 9;

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, commit_hash));
    memset(&loose, 0, sizeof(loose));
    memset(&packs, 0, sizeof(packs));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_loose_object_iter(ctx, count_loose_cb, &loose));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_file_iter(ctx, count_pack_file_cb, &packs));
    TEST_ASSERT_EQUAL_size_t(0, loose.count);
    TEST_ASSERT_EQUAL_size_t(1, packs.count);

    batch.timestamp_unix_nanos = 2;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, retry_hash));
    TEST_ASSERT_EQUAL_MEMORY(commit_hash, retry_hash, SCRIBE_HASH_SIZE);
    memset(&loose, 0, sizeof(loose));
    memset(&packs, 0, sizeof(packs));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_loose_object_iter(ctx, count_loose_cb, &loose));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_file_iter(ctx, count_pack_file_cb, &packs));
    TEST_ASSERT_EQUAL_size_t(0, loose.count);
    TEST_ASSERT_EQUAL_size_t(1, packs.count);

    memset(&obj, 0, sizeof(obj));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, commit_hash, &obj));
    TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_COMMIT, obj.type);
    scribe_object_free(&obj);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));
    scribe_close(ctx);
}

void test_status_uses_pack_metadata_without_per_object_pack_reads(void) {
    char tmpl[] = "/tmp/scribe-status-pack-trace-test-XXXXXX";
    enum { EVENT_COUNT = 16 };
    scribe_ctx *ctx = NULL;
    scribe_change_event events[EVENT_COUNT];
    const char *paths[EVENT_COUNT][3];
    char ids[EVENT_COUNT][32];
    char payloads[EVENT_COUNT][64];
    scribe_change_batch batch;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    scribe_perf_trace trace;
    size_t i;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    ctx->config.commit_storage = SCRIBE_COMMIT_STORAGE_PACK;

    memset(events, 0, sizeof(events));
    memset(paths, 0, sizeof(paths));
    for (i = 0; i < EVENT_COUNT; i++) {
        snprintf(ids[i], sizeof(ids[i]), "doc-%02zu", i);
        snprintf(payloads[i], sizeof(payloads[i]), "{\"n\":%zu}", i);
        paths[i][0] = "db";
        paths[i][1] = "status-pack";
        paths[i][2] = ids[i];
        events[i].path = paths[i];
        events[i].path_len = 3;
        events[i].payload = (const uint8_t *)payloads[i];
        events[i].payload_len = strlen(payloads[i]);
    }

    memset(&batch, 0, sizeof(batch));
    batch.events = events;
    batch.event_count = EVENT_COUNT;
    batch.author = (scribe_identity){"tester", "", "test"};
    batch.committer = (scribe_identity){"scribe-test", "", "scribe"};
    batch.process = (scribe_process_info){"unit", "1", "", "status-pack-trace"};
    batch.timestamp_unix_nanos = 1;
    batch.message = "status pack trace";
    batch.message_len = 17;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, commit_hash));

    TEST_ASSERT_EQUAL_INT(0, setenv("SCRIBE_TRACE_PERF", "1", 1));
    scribe_perf_trace_reset();
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_status(ctx));
    scribe_perf_trace_snapshot(&trace);
    unsetenv("SCRIBE_TRACE_PERF");

    TEST_ASSERT_EQUAL_UINT64(1, trace.pack_validations);
    TEST_ASSERT_EQUAL_UINT64(1, trace.packed_object_reads);
    TEST_ASSERT_LESS_THAN_UINT64((uint64_t)EVENT_COUNT, trace.full_pack_reads);
    TEST_ASSERT_LESS_THAN_UINT64((uint64_t)EVENT_COUNT, trace.pack_index_parses);
    TEST_ASSERT_GREATER_THAN_UINT64(0, trace.pack_file_bytes_read);
    TEST_ASSERT_GREATER_THAN_UINT64(0, trace.pack_decompressed_bytes);
    TEST_ASSERT_GREATER_THAN_UINT64(0, trace.object_type_count_ns);

    scribe_close(ctx);
}

/*
 * Writes every reachable object into two separate packs, removes loose copies,
 * then runs gc --consolidate. The result must remain readable and must expose
 * each reachable hash through the object iterator only once.
 */
void test_gc_consolidate_removes_duplicate_packed_records(void) {
    char tmpl[] = "/tmp/scribe-gc-consolidate-test-XXXXXX";
    scribe_ctx *ctx = NULL;
    const char *path[] = {"db", "users", "\"gc-user\""};
    const uint8_t payload[] = "{\"_id\":\"gc-user\"}";
    scribe_change_event event;
    scribe_change_batch batch;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    hash_collect_state collected;
    hash_collect_state before;
    hash_collect_state after;
    scribe_pack_object *objects = NULL;
    size_t i;

    make_temp_repo(tmpl);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_init_repository(tmpl));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_open(tmpl, 1, &ctx));
    memset(&event, 0, sizeof(event));
    event.path = path;
    event.path_len = 3;
    event.payload = payload;
    event.payload_len = sizeof(payload) - 1u;
    memset(&batch, 0, sizeof(batch));
    batch.events = &event;
    batch.event_count = 1;
    batch.author = (scribe_identity){"tester", "", "test"};
    batch.committer = (scribe_identity){"scribe-test", "", "scribe"};
    batch.process = (scribe_process_info){"unit", "1", "", "gc-consolidate"};
    batch.timestamp_unix_nanos = 1;
    batch.message = "gc consolidate";
    batch.message_len = 14;
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_commit_batch(ctx, &batch, commit_hash));

    memset(&collected, 0, sizeof(collected));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, collect_hash_visit, &collected));
    TEST_ASSERT_GREATER_THAN_size_t(0, collected.count);
    objects = (scribe_pack_object *)calloc(collected.count, sizeof(*objects));
    TEST_ASSERT_NOT_NULL(objects);
    for (i = 0; i < collected.count; i++) {
        scribe_object obj;
        memset(&obj, 0, sizeof(obj));
        TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, collected.hashes + i * SCRIBE_HASH_SIZE, &obj));
        objects[i].type = obj.type;
        objects[i].payload_len = obj.payload_len;
        objects[i].payload = (uint8_t *)malloc(obj.payload_len == 0u ? 1u : obj.payload_len);
        TEST_ASSERT_NOT_NULL(objects[i].payload);
        if (obj.payload_len != 0u) {
            memcpy((void *)objects[i].payload, obj.payload, obj.payload_len);
        }
        scribe_object_free(&obj);
    }
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, objects, collected.count, NULL));
    usleep(1000u);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_pack_write_objects(ctx, objects, collected.count, NULL));
    for (i = 0; i < collected.count; i++) {
        TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_loose_object_delete(ctx, collected.hashes + i * SCRIBE_HASH_SIZE));
    }

    memset(&before, 0, sizeof(before));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, collect_hash_visit, &before));
    TEST_ASSERT_EQUAL_size_t(collected.count * 2u, before.count);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_gc(ctx, 0, 0, 0, 1));
    memset(&after, 0, sizeof(after));
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_iter(ctx, collect_hash_visit, &after));
    TEST_ASSERT_EQUAL_size_t(collected.count, after.count);
    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_cli_fsck(ctx));
    {
        scribe_object commit_obj;
        memset(&commit_obj, 0, sizeof(commit_obj));
        TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_object_read(ctx, commit_hash, &commit_obj));
        TEST_ASSERT_EQUAL_UINT8(SCRIBE_OBJECT_COMMIT, commit_obj.type);
        scribe_object_free(&commit_obj);
    }

    free_pack_objects(objects, collected.count);
    free(collected.hashes);
    free(before.hashes);
    free(after.hashes);
    scribe_close(ctx);
}
