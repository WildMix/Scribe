/*
 * Unit tests for MongoDB canonicalization helpers.
 *
 * These tests do not connect to MongoDB. They verify that BSON/Extended JSON
 * conversion produces deterministic bytes and document-id path components.
 */
#include "adapter_mongo/mongo_internal.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

/*
 * Verifies recursive object-key sorting for canonical Extended JSON input,
 * including keys inside objects nested in arrays.
 */
void test_mongo_canonical_json_sorts_keys(void) {
    char *out = NULL;
    size_t out_len = 0;
    const char *json = "{\"z\":1,\"a\":{\"b\":2,\"a\":1},\"m\":[{\"y\":2,\"x\":1}]}";

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_mongo_canonicalize_json(json, &out, &out_len));
    TEST_ASSERT_EQUAL_STRING("{\"a\":{\"a\":1,\"b\":2},\"m\":[{\"x\":1,\"y\":2}],\"z\":1}", out);
    TEST_ASSERT_EQUAL_size_t(strlen(out), out_len);
    free(out);
}

/*
 * Verifies BSON-to-canonical-JSON conversion and extraction of the canonical
 * `_id` value used as a Mongo document tree leaf.
 */
void test_mongo_canonical_bson_and_id(void) {
    bson_t doc;
    bson_t child;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    char *id = NULL;

    bson_init(&doc);
    BSON_APPEND_UTF8(&doc, "z", "last");
    BSON_APPEND_UTF8(&doc, "_id", "alice");
    BSON_APPEND_DOCUMENT_BEGIN(&doc, "a", &child);
    BSON_APPEND_INT32(&child, "b", 2);
    BSON_APPEND_INT32(&child, "a", 1);
    bson_append_document_end(&doc, &child);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_mongo_canonicalize_bson(&doc, &payload, &payload_len));
    TEST_ASSERT_EQUAL_STRING(
        "{\"_id\":\"alice\",\"a\":{\"a\":{\"$numberInt\":\"1\"},\"b\":{\"$numberInt\":\"2\"}},\"z\":\"last\"}",
        (char *)payload);
    TEST_ASSERT_EQUAL_size_t(strlen((char *)payload), payload_len);

    TEST_ASSERT_EQUAL(SCRIBE_OK, scribe_mongo_canonicalize_id(&doc, &id));
    TEST_ASSERT_EQUAL_STRING("\"alice\"", id);

    free(payload);
    free(id);
    bson_destroy(&doc);
}
