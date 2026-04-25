#include "core/internal.h"
#include "util/arena.h"
#include "util/hex.h"
#include "util/leb128.h"
#include "util/queue.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

void test_leb128_rejects_overlong(void) {
    uint8_t overlong[11];
    uint64_t decoded = 0;
    size_t used = 0;

    memset(overlong, 0x80, sizeof(overlong));
    TEST_ASSERT_EQUAL(SCRIBE_ECORRUPT, scribe_leb128_decode(overlong, sizeof(overlong), &decoded, &used));
}

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

static void make_temp_repo(char *tmpl) {
    char *created = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(created);
}

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
