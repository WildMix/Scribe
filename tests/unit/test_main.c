#include "unity.h"

void setUp(void) {}

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
