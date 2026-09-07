/*
 * Unity test runner for Scribe unit tests.
 *
 * Individual test functions live in focused test files. This runner registers
 * core tests only; adapter tests live with their adapter targets.
 */
#include "unity.h"

/*
 * Unity setup hook. The current tests create their own repositories/resources,
 * so no shared per-test setup is needed.
 */
void setUp(void) {}

/*
 * Unity teardown hook. Individual tests clean up their own allocations, so this
 * remains intentionally empty.
 */
void tearDown(void) {}

void test_leb128_round_trip(void);
void test_leb128_rejects_overlong(void);
void test_hex_round_trip(void);
void test_hash_set_add_and_has_deduplicates(void);
void test_ndjson_line_event_round_trip_and_rejects_malformed(void);
void test_timestamp_parse_and_format(void);
void test_arena_alloc_reset(void);
void test_tree_serialization_is_sorted(void);
void test_queue_fifo_try_pop(void);
void test_repository_commit_and_fsck(void);
void test_commit_batch_accepts_empty_blob_payload(void);
void test_ingest_json_explicit_put_empty_blob_and_delete(void);
void test_ingest_json_skips_unchanged_payload_at_path(void);
void test_ingest_content_put_empty_blob_and_delete(void);
void test_ingest_content_large_binary_payload(void);
void test_pipe_commit_batch(void);
void test_pipe_v2_put_empty_blob_and_delete(void);
void test_object_iterator_and_compressed_size(void);
void test_pack_only_object_read_iter_and_compressed_size(void);
void test_packed_object_read_uses_ranged_pack_read(void);
void test_list_objects_uses_pack_metadata_without_packed_object_reads(void);
void test_pack_read_after_loose_delete(void);
void test_pack_only_repository_history_reads(void);
void test_pack_corrupt_index_fails_validation(void);
void test_pack_corrupt_pack_fails_fsck(void);
void test_streaming_pack_writer_publishes_objects(void);
void test_commit_batch_auto_pack_threshold_writes_pack_only_objects(void);
void test_status_uses_pack_metadata_without_per_object_pack_reads(void);
void test_gc_consolidate_removes_duplicate_packed_records(void);

/*
 * Runs all compiled Unity tests and returns Unity's aggregate status code to
 * CTest.
 */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_leb128_round_trip);
    RUN_TEST(test_leb128_rejects_overlong);
    RUN_TEST(test_hex_round_trip);
    RUN_TEST(test_hash_set_add_and_has_deduplicates);
    RUN_TEST(test_ndjson_line_event_round_trip_and_rejects_malformed);
    RUN_TEST(test_timestamp_parse_and_format);
    RUN_TEST(test_arena_alloc_reset);
    RUN_TEST(test_tree_serialization_is_sorted);
    RUN_TEST(test_queue_fifo_try_pop);
    RUN_TEST(test_repository_commit_and_fsck);
    RUN_TEST(test_commit_batch_accepts_empty_blob_payload);
    RUN_TEST(test_ingest_json_explicit_put_empty_blob_and_delete);
    RUN_TEST(test_ingest_json_skips_unchanged_payload_at_path);
    RUN_TEST(test_ingest_content_put_empty_blob_and_delete);
    RUN_TEST(test_ingest_content_large_binary_payload);
    RUN_TEST(test_pipe_commit_batch);
    RUN_TEST(test_pipe_v2_put_empty_blob_and_delete);
    RUN_TEST(test_object_iterator_and_compressed_size);
    RUN_TEST(test_pack_only_object_read_iter_and_compressed_size);
    RUN_TEST(test_packed_object_read_uses_ranged_pack_read);
    RUN_TEST(test_list_objects_uses_pack_metadata_without_packed_object_reads);
    RUN_TEST(test_pack_read_after_loose_delete);
    RUN_TEST(test_pack_only_repository_history_reads);
    RUN_TEST(test_pack_corrupt_index_fails_validation);
    RUN_TEST(test_pack_corrupt_pack_fails_fsck);
    RUN_TEST(test_streaming_pack_writer_publishes_objects);
    RUN_TEST(test_commit_batch_auto_pack_threshold_writes_pack_only_objects);
    RUN_TEST(test_status_uses_pack_metadata_without_per_object_pack_reads);
    RUN_TEST(test_gc_consolidate_removes_duplicate_packed_records);
    return UNITY_END();
}
