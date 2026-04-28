/*
 * Unity test runner for Scribe unit tests.
 *
 * Individual test functions live in focused test files. This runner registers
 * them with Unity and conditionally includes Mongo canonicalization tests when
 * the Mongo adapter is compiled.
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
void test_arena_alloc_reset(void);
void test_tree_serialization_is_sorted(void);
void test_queue_fifo_try_pop(void);
void test_repository_commit_and_fsck(void);
void test_pipe_commit_batch(void);
void test_object_iterator_and_compressed_size(void);
#ifdef SCRIBE_HAVE_MONGO_ADAPTER
void test_mongo_canonical_json_sorts_keys(void);
void test_mongo_canonical_bson_and_id(void);
#endif

/*
 * Runs all compiled Unity tests and returns Unity's aggregate status code to
 * CTest.
 */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_leb128_round_trip);
    RUN_TEST(test_leb128_rejects_overlong);
    RUN_TEST(test_hex_round_trip);
    RUN_TEST(test_arena_alloc_reset);
    RUN_TEST(test_tree_serialization_is_sorted);
    RUN_TEST(test_queue_fifo_try_pop);
    RUN_TEST(test_repository_commit_and_fsck);
    RUN_TEST(test_pipe_commit_batch);
    RUN_TEST(test_object_iterator_and_compressed_size);
#ifdef SCRIBE_HAVE_MONGO_ADAPTER
    RUN_TEST(test_mongo_canonical_json_sorts_keys);
    RUN_TEST(test_mongo_canonical_bson_and_id);
#endif
    return UNITY_END();
}
