/*
 * Docker-backed MongoDB integration tests for Scribe.
 *
 * These tests use Testcontainers Native to create the MongoDB container and the
 * real Scribe CLI to exercise repository behavior. libmongoc is used only for
 * test setup and MongoDB mutations; Scribe itself observes MongoDB through
 * mongo-watch exactly as it does in production.
 */
#include "testcontainers-c/container.h"

#include <bson/bson.h>
#include <mongoc/mongoc.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int exit_code;
    char *stdout_bytes;
    char *stderr_bytes;
} command_result;

typedef struct {
    pid_t pid;
    char stdout_path[PATH_MAX];
    char stderr_path[PATH_MAX];
} watcher_process;

static const char *g_scribe_bin = NULL;
static int g_container_id = -1;
static char g_mongo_uri[512];
static char g_mongo_hostport[256];

static void fail(const char *fmt, ...) {
    va_list ap;

    fputs("scribe_mongo_testcontainers: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void check(bool cond, const char *fmt, ...) {
    va_list ap;

    if (cond) {
        return;
    }
    fputs("scribe_mongo_testcontainers: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void format_checked(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        fail("formatted string exceeded buffer");
    }
}

static char *read_file_alloc(const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    size_t got;

    check(f != NULL, "failed to open %s: %s", path, strerror(errno));
    check(fseek(f, 0, SEEK_END) == 0, "failed to seek %s", path);
    len = ftell(f);
    check(len >= 0, "failed to tell %s", path);
    check(fseek(f, 0, SEEK_SET) == 0, "failed to rewind %s", path);
    buf = (char *)calloc((size_t)len + 1u, 1u);
    check(buf != NULL, "failed to allocate file buffer");
    got = fread(buf, 1u, (size_t)len, f);
    check(got == (size_t)len, "short read from %s", path);
    fclose(f);
    return buf;
}

static bool file_contains(const char *path, const char *needle) {
    char *bytes = read_file_alloc(path);
    bool found = strstr(bytes, needle) != NULL;

    free(bytes);
    return found;
}

static command_result run_shell(const char *cmd) {
    char tmp[] = "/tmp/scribe-command-XXXXXX";
    char stdout_path[PATH_MAX];
    char stderr_path[PATH_MAX];
    char full_cmd[8192];
    command_result result;
    int status;

    check(mkdtemp(tmp) != NULL, "failed to create command temp dir");
    format_checked(stdout_path, sizeof(stdout_path), "%s/stdout", tmp);
    format_checked(stderr_path, sizeof(stderr_path), "%s/stderr", tmp);
    format_checked(full_cmd, sizeof(full_cmd), "%s > \"%s\" 2> \"%s\"", cmd, stdout_path, stderr_path);

    status = system(full_cmd);
    if (status == -1) {
        result.exit_code = 127;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = 127;
    }
    result.stdout_bytes = read_file_alloc(stdout_path);
    result.stderr_bytes = read_file_alloc(stderr_path);
    unlink(stdout_path);
    unlink(stderr_path);
    rmdir(tmp);
    return result;
}

static void command_result_free(command_result *result) {
    free(result->stdout_bytes);
    free(result->stderr_bytes);
    memset(result, 0, sizeof(*result));
}

static command_result run_scribe(const char *store, const char *args_fmt, ...) {
    char args[4096];
    char cmd[8192];
    va_list ap;
    int n;

    va_start(ap, args_fmt);
    n = vsnprintf(args, sizeof(args), args_fmt, ap);
    va_end(ap);
    check(n >= 0 && (size_t)n < sizeof(args), "scribe arguments exceeded buffer");
    format_checked(cmd, sizeof(cmd), "\"%s\" --store \"%s\" %s", g_scribe_bin, store, args);
    return run_shell(cmd);
}

static command_result run_scribe_no_store(const char *args_fmt, ...) {
    char args[4096];
    char cmd[8192];
    va_list ap;
    int n;

    va_start(ap, args_fmt);
    n = vsnprintf(args, sizeof(args), args_fmt, ap);
    va_end(ap);
    check(n >= 0 && (size_t)n < sizeof(args), "scribe arguments exceeded buffer");
    format_checked(cmd, sizeof(cmd), "\"%s\" %s", g_scribe_bin, args);
    return run_shell(cmd);
}

static void assert_success(command_result *result, const char *label) {
    check(result->exit_code == 0, "%s failed with exit %d\nstdout:\n%s\nstderr:\n%s", label, result->exit_code,
          result->stdout_bytes, result->stderr_bytes);
}

static bool is_lower_hex_char(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }

static void latest_commit_hash(const char *store, char hash[65]) {
    char ref_path[PATH_MAX];
    char *contents;

    format_checked(ref_path, sizeof(ref_path), "%s/refs/heads/main", store);
    contents = read_file_alloc(ref_path);
    for (size_t i = 0; i < 64u; i++) {
        check(is_lower_hex_char(contents[i]), "HEAD ref did not contain a full commit hash: %s", contents);
        hash[i] = contents[i];
    }
    hash[64] = '\0';
    free(contents);
}

static void init_store(const char *store) {
    command_result result = run_scribe_no_store("init \"%s\"", store);

    assert_success(&result, "scribe init");
    command_result_free(&result);
}

static void make_temp_store(char *store, size_t store_cap) {
    char root[] = "/tmp/scribe-tc-store-XXXXXX";

    check(mkdtemp(root) != NULL, "failed to create temp store root");
    format_checked(store, store_cap, "%s/.scribe", root);
    init_store(store);
}

static void db_name(char *out, size_t cap, const char *suffix) {
    format_checked(out, cap, "scribe_tc_%ld_%s", (long)getpid(), suffix);
}

static void db_uri(char *out, size_t cap, const char *database) {
    format_checked(out, cap, "mongodb://%s/%s?directConnection=true", g_mongo_hostport, database);
}

static void cleanup_container(void) {
    if (g_container_id >= 0) {
        char *err = tc_container_terminate(g_container_id);
        (void)err;
        g_container_id = -1;
    }
}

static void parse_testcontainer_uri(const char *uri) {
    const char *hostport = uri;
    const char *slash;

    if (strncmp(uri, "http://", 7u) == 0) {
        hostport = uri + 7;
    }
    slash = strchr(hostport, '/');
    if (slash == NULL) {
        format_checked(g_mongo_hostport, sizeof(g_mongo_hostport), "%s", hostport);
    } else {
        size_t len = (size_t)(slash - hostport);
        check(len < sizeof(g_mongo_hostport), "mapped Mongo host:port too long");
        memcpy(g_mongo_hostport, hostport, len);
        g_mongo_hostport[len] = '\0';
    }
    format_checked(g_mongo_uri, sizeof(g_mongo_uri), "mongodb://%s/?directConnection=true", g_mongo_hostport);
}

static bool mongo_is_primary(void) {
    mongoc_client_t *client = mongoc_client_new(g_mongo_uri);
    bson_t cmd;
    bson_t reply;
    bson_iter_t iter;
    bson_error_t error;
    bool ok = false;

    if (client == NULL) {
        return false;
    }
    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "hello", 1);
    if (mongoc_client_command_simple(client, "admin", &cmd, NULL, &reply, &error)) {
        if (bson_iter_init_find(&iter, &reply, "isWritablePrimary") && BSON_ITER_HOLDS_BOOL(&iter)) {
            ok = bson_iter_bool(&iter);
        }
        bson_destroy(&reply);
    }
    bson_destroy(&cmd);
    mongoc_client_destroy(client);
    return ok;
}

static void start_mongo_container(void) {
    const char *image = getenv("SCRIBE_TEST_MONGO_IMAGE");
    int request_id;
    int container_id;
    char error[1024];
    char *uri;
    int i;

    if (image == NULL || image[0] == '\0') {
        image = "scribe-mongo-rs-test:7";
    }
    request_id = tc_container_create(image);
    check(request_id >= 0, "failed to create Testcontainers request for %s", image);
    tc_container_with_exposed_tcp_port(request_id, 27017);
    memset(error, 0, sizeof(error));
    container_id = tc_container_run(request_id, error);
    check(container_id >= 0, "failed to start Testcontainers MongoDB: %s", error);
    g_container_id = container_id;
    atexit(cleanup_container);

    uri = tc_container_get_uri(container_id, 27017, error);
    check(uri != NULL, "failed to resolve MongoDB mapped URI");
    parse_testcontainer_uri(uri);

    for (i = 0; i < 90; i++) {
        if (mongo_is_primary()) {
            return;
        }
        sleep(1u);
    }
    fail("MongoDB replica set did not become PRIMARY at %s", g_mongo_uri);
}

static mongoc_client_t *new_client(void) {
    mongoc_client_t *client = mongoc_client_new(g_mongo_uri);

    check(client != NULL, "failed to create MongoDB client for %s", g_mongo_uri);
    return client;
}

static void drop_database(const char *database) {
    mongoc_client_t *client = new_client();
    bson_t *cmd;
    bson_error_t error;
    bool ok;

    cmd = BCON_NEW("dropDatabase", BCON_INT32(1));
    ok = mongoc_client_command_simple(client, database, cmd, NULL, NULL, &error);
    bson_destroy(cmd);
    check(ok, "failed to drop database %s: %s", database, error.message);
    mongoc_client_destroy(client);
}

static void enable_pre_images(const char *database, const char *collection_name) {
    mongoc_client_t *client = new_client();
    bson_t cmd;
    bson_t opts;
    bson_error_t error;

    bson_init(&cmd);
    BSON_APPEND_UTF8(&cmd, "collMod", collection_name);
    BSON_APPEND_DOCUMENT_BEGIN(&cmd, "changeStreamPreAndPostImages", &opts);
    BSON_APPEND_BOOL(&opts, "enabled", true);
    bson_append_document_end(&cmd, &opts);
    check(mongoc_client_command_simple(client, database, &cmd, NULL, NULL, &error),
          "failed to enable pre/post images on %s.%s: %s", database, collection_name, error.message);
    bson_destroy(&cmd);
    mongoc_client_destroy(client);
}

static void insert_user_role(const char *database, const char *id, const char *role) {
    mongoc_client_t *client = new_client();
    mongoc_collection_t *collection = mongoc_client_get_collection(client, database, "users");
    bson_t doc;
    bson_error_t error;

    bson_init(&doc);
    BSON_APPEND_UTF8(&doc, "_id", id);
    BSON_APPEND_UTF8(&doc, "role", role);
    check(mongoc_collection_insert_one(collection, &doc, NULL, NULL, &error), "failed to insert user %s: %s", id,
          error.message);
    bson_destroy(&doc);
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
}

static void insert_oid_user(const char *database, const char *oid_text) {
    mongoc_client_t *client = new_client();
    mongoc_collection_t *collection = mongoc_client_get_collection(client, database, "users");
    bson_oid_t oid;
    bson_t doc;
    bson_t nested;
    bson_error_t error;

    bson_oid_init_from_string(&oid, oid_text);
    bson_init(&doc);
    BSON_APPEND_OID(&doc, "_id", &oid);
    BSON_APPEND_DOCUMENT_BEGIN(&doc, "profile", &nested);
    BSON_APPEND_UTF8(&nested, "z", "last");
    BSON_APPEND_UTF8(&nested, "a", "first");
    bson_append_document_end(&doc, &nested);
    check(mongoc_collection_insert_one(collection, &doc, NULL, NULL, &error), "failed to insert oid user: %s",
          error.message);
    bson_destroy(&doc);
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
}

static void update_user_role(const char *database, const char *id, const char *role) {
    mongoc_client_t *client = new_client();
    mongoc_collection_t *collection = mongoc_client_get_collection(client, database, "users");
    bson_t selector;
    bson_t update;
    bson_t set;
    bson_error_t error;

    bson_init(&selector);
    BSON_APPEND_UTF8(&selector, "_id", id);
    bson_init(&update);
    BSON_APPEND_DOCUMENT_BEGIN(&update, "$set", &set);
    BSON_APPEND_UTF8(&set, "role", role);
    bson_append_document_end(&update, &set);
    check(mongoc_collection_update_one(collection, &selector, &update, NULL, NULL, &error),
          "failed to update user %s: %s", id, error.message);
    bson_destroy(&selector);
    bson_destroy(&update);
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
}

static void replace_user_role(const char *database, const char *id, const char *role) {
    mongoc_client_t *client = new_client();
    mongoc_collection_t *collection = mongoc_client_get_collection(client, database, "users");
    bson_t selector;
    bson_t replacement;
    bson_error_t error;

    bson_init(&selector);
    BSON_APPEND_UTF8(&selector, "_id", id);
    bson_init(&replacement);
    BSON_APPEND_UTF8(&replacement, "_id", id);
    BSON_APPEND_UTF8(&replacement, "role", role);
    check(mongoc_collection_replace_one(collection, &selector, &replacement, NULL, NULL, &error),
          "failed to replace user %s: %s", id, error.message);
    bson_destroy(&selector);
    bson_destroy(&replacement);
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
}

static void delete_user(const char *database, const char *id) {
    mongoc_client_t *client = new_client();
    mongoc_collection_t *collection = mongoc_client_get_collection(client, database, "users");
    bson_t selector;
    bson_error_t error;

    bson_init(&selector);
    BSON_APPEND_UTF8(&selector, "_id", id);
    check(mongoc_collection_delete_one(collection, &selector, NULL, NULL, &error), "failed to delete user %s: %s", id,
          error.message);
    bson_destroy(&selector);
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
}

static void create_collection(const char *database, const char *collection_name) {
    mongoc_client_t *client = new_client();
    mongoc_database_t *db = mongoc_client_get_database(client, database);
    bson_error_t error;
    mongoc_collection_t *collection = mongoc_database_create_collection(db, collection_name, NULL, &error);

    check(collection != NULL, "failed to create %s collection in %s: %s", collection_name, database, error.message);
    mongoc_collection_destroy(collection);
    mongoc_database_destroy(db);
    mongoc_client_destroy(client);
}

static void create_index_on_role(const char *database) {
    mongoc_client_t *client = new_client();
    bson_t cmd;
    bson_t indexes;
    bson_t index;
    bson_t key;
    bson_t keys;
    bson_error_t error;

    bson_init(&cmd);
    BSON_APPEND_UTF8(&cmd, "createIndexes", "users");
    BSON_APPEND_ARRAY_BEGIN(&cmd, "indexes", &indexes);
    BSON_APPEND_DOCUMENT_BEGIN(&indexes, "0", &index);
    BSON_APPEND_DOCUMENT_BEGIN(&index, "key", &key);
    bson_init(&keys);
    BSON_APPEND_INT32(&keys, "role", 1);
    bson_concat(&key, &keys);
    bson_append_document_end(&index, &key);
    BSON_APPEND_UTF8(&index, "name", "role_1");
    bson_append_document_end(&indexes, &index);
    bson_append_array_end(&cmd, &indexes);
    check(mongoc_client_command_simple(client, database, &cmd, NULL, NULL, &error), "failed to create role index: %s",
          error.message);
    bson_destroy(&keys);
    bson_destroy(&cmd);
    mongoc_client_destroy(client);
}

static void drop_users_collection(const char *database) {
    mongoc_client_t *client = new_client();
    mongoc_collection_t *collection = mongoc_client_get_collection(client, database, "users");
    bson_error_t error;

    check(mongoc_collection_drop(collection, &error), "failed to drop users collection: %s", error.message);
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
}

static void run_transaction_two_writes(const char *database) {
    mongoc_client_t *client = new_client();
    mongoc_collection_t *users = mongoc_client_get_collection(client, database, "users");
    mongoc_collection_t *orders = mongoc_client_get_collection(client, database, "orders");
    mongoc_client_session_t *session;
    bson_t opts;
    bson_t user_doc;
    bson_t order_doc;
    bson_t reply;
    bson_error_t error;

    session = mongoc_client_start_session(client, NULL, &error);
    check(session != NULL, "failed to start transaction session: %s", error.message);
    check(mongoc_client_session_start_transaction(session, NULL, &error), "failed to start transaction: %s",
          error.message);
    bson_init(&opts);
    check(mongoc_client_session_append(session, &opts, &error), "failed to append session: %s", error.message);

    bson_init(&user_doc);
    BSON_APPEND_UTF8(&user_doc, "_id", "alice");
    BSON_APPEND_UTF8(&user_doc, "role", "buyer");
    check(mongoc_collection_insert_one(users, &user_doc, &opts, NULL, &error), "failed to insert transaction user: %s",
          error.message);

    bson_init(&order_doc);
    BSON_APPEND_UTF8(&order_doc, "_id", "order-1");
    BSON_APPEND_UTF8(&order_doc, "user", "alice");
    check(mongoc_collection_insert_one(orders, &order_doc, &opts, NULL, &error),
          "failed to insert transaction order: %s", error.message);

    check(mongoc_client_session_commit_transaction(session, &reply, &error), "failed to commit transaction: %s",
          error.message);
    bson_destroy(&reply);
    bson_destroy(&user_doc);
    bson_destroy(&order_doc);
    bson_destroy(&opts);
    mongoc_client_session_destroy(session);
    mongoc_collection_destroy(users);
    mongoc_collection_destroy(orders);
    mongoc_client_destroy(client);
}

static watcher_process start_watcher(const char *store, const char *uri) {
    char tmp[] = "/tmp/scribe-watch-XXXXXX";
    watcher_process watcher;
    int out_fd;
    int err_fd;
    pid_t pid;

    memset(&watcher, 0, sizeof(watcher));
    check(mkdtemp(tmp) != NULL, "failed to create watcher temp dir");
    format_checked(watcher.stdout_path, sizeof(watcher.stdout_path), "%s/stdout", tmp);
    format_checked(watcher.stderr_path, sizeof(watcher.stderr_path), "%s/stderr", tmp);
    out_fd = open(watcher.stdout_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    err_fd = open(watcher.stderr_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    check(out_fd >= 0 && err_fd >= 0, "failed to open watcher output files");
    pid = fork();
    check(pid >= 0, "failed to fork mongo-watch");
    if (pid == 0) {
        dup2(out_fd, STDOUT_FILENO);
        dup2(err_fd, STDERR_FILENO);
        close(out_fd);
        close(err_fd);
        execl(g_scribe_bin, g_scribe_bin, "mongo-watch", uri, "--store", store, (char *)NULL);
        _exit(127);
    }
    close(out_fd);
    close(err_fd);
    watcher.pid = pid;
    return watcher;
}

static void wait_for_log(const char *path, const char *needle, int timeout_seconds) {
    int i;

    for (i = 0; i < timeout_seconds; i++) {
        if (file_contains(path, needle)) {
            return;
        }
        sleep(1u);
    }
    fail("timed out waiting for log text '%s'\ncurrent log:\n%s", needle, read_file_alloc(path));
}

static void stop_watcher(watcher_process *watcher) {
    int status;

    if (watcher->pid <= 0) {
        return;
    }
    kill(watcher->pid, SIGTERM);
    check(waitpid(watcher->pid, &status, 0) == watcher->pid, "failed to wait for mongo-watch");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "mongo-watch exited uncleanly; stderr:\n%s",
          read_file_alloc(watcher->stderr_path));
    watcher->pid = 0;
}

static void assert_output_contains(command_result *result, const char *needle, const char *label) {
    check(strstr(result->stdout_bytes, needle) != NULL, "%s missing '%s'\nstdout:\n%s\nstderr:\n%s", label, needle,
          result->stdout_bytes, result->stderr_bytes);
}

static void test_bootstrap_tree_and_canonical_json(void) {
    char database[96];
    char uri[512];
    char store[PATH_MAX];
    char head_hash[65];
    watcher_process watcher;
    command_result result;

    db_name(database, sizeof(database), "bootstrap");
    drop_database(database);
    insert_user_role(database, "alice", "admin");
    insert_oid_user(database, "507f1f77bcf86cd799439011");
    enable_pre_images(database, "users");
    db_uri(uri, sizeof(uri), database);
    make_temp_store(store, sizeof(store));

    watcher = start_watcher(store, uri);
    wait_for_log(watcher.stderr_path, "bootstrap commit", 60);
    wait_for_log(watcher.stderr_path, "with 2 document(s)", 60);
    wait_for_log(watcher.stderr_path, "watching MongoDB change stream", 60);
    stop_watcher(&watcher);

    result = run_scribe(store, "show 'HEAD:%s/users/\"alice\"'", database);
    assert_success(&result, "show bootstrap alice");
    check(strcmp(result.stdout_bytes, "{\"_id\":\"alice\",\"role\":\"admin\"}") == 0, "unexpected alice JSON: %s",
          result.stdout_bytes);
    command_result_free(&result);

    result = run_scribe(store, "show 'HEAD:%s/users/{\"$oid\":\"507f1f77bcf86cd799439011\"}'", database);
    assert_success(&result, "show bootstrap oid");
    check(strcmp(result.stdout_bytes,
                 "{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"profile\":{\"a\":\"first\",\"z\":\"last\"}}") == 0,
          "unexpected oid JSON: %s", result.stdout_bytes);
    command_result_free(&result);

    latest_commit_hash(store, head_hash);
    result = run_scribe(store, "ls-tree %s", head_hash);
    assert_success(&result, "ls-tree bootstrap");
    assert_output_contains(&result, database, "ls-tree bootstrap");
    assert_output_contains(&result, "users/\"alice\"", "ls-tree bootstrap");
    command_result_free(&result);

    result = run_scribe(store, "fsck");
    assert_success(&result, "fsck bootstrap");
    assert_output_contains(&result, "dangling objects", "fsck bootstrap");
    command_result_free(&result);
}

static void test_change_stream_document_lifecycle(void) {
    char database[96];
    char uri[512];
    char store[PATH_MAX];
    watcher_process watcher;
    command_result result;

    db_name(database, sizeof(database), "lifecycle");
    drop_database(database);
    create_collection(database, "users");
    enable_pre_images(database, "users");
    db_uri(uri, sizeof(uri), database);
    make_temp_store(store, sizeof(store));

    watcher = start_watcher(store, uri);
    wait_for_log(watcher.stderr_path, "watching MongoDB change stream", 60);

    insert_user_role(database, "alice", "user");
    wait_for_log(watcher.stderr_path, "insert ", 60);
    wait_for_log(watcher.stderr_path, "users/\"alice\"", 60);
    update_user_role(database, "alice", "admin");
    wait_for_log(watcher.stderr_path, "update ", 60);
    replace_user_role(database, "alice", "owner");
    wait_for_log(watcher.stderr_path, "replace ", 60);
    delete_user(database, "alice");
    wait_for_log(watcher.stderr_path, "delete ", 60);
    stop_watcher(&watcher);

    result = run_scribe(store, "log --oneline -- '%s/users/\"alice\"'", database);
    assert_success(&result, "path-filtered lifecycle log");
    assert_output_contains(&result, "(deleted)", "path-filtered lifecycle log");
    assert_output_contains(&result, "(added)", "path-filtered lifecycle log");
    command_result_free(&result);

    result = run_scribe(store, "diff HEAD~1 HEAD");
    assert_success(&result, "delete diff");
    assert_output_contains(&result, "D ", "delete diff");
    assert_output_contains(&result, "users/\"alice\"", "delete diff");
    command_result_free(&result);

    result = run_scribe(store, "show 'HEAD:%s/users/\"alice\"'", database);
    check(result.exit_code != 0 && strstr(result.stderr_bytes, "SCRIBE_ENOT_FOUND") != NULL,
          "deleted document still resolved\nstdout:\n%s\nstderr:\n%s", result.stdout_bytes, result.stderr_bytes);
    command_result_free(&result);
}

static void test_transaction_becomes_one_commit(void) {
    char database[96];
    char uri[512];
    char store[PATH_MAX];
    watcher_process watcher;
    command_result result;

    db_name(database, sizeof(database), "txn");
    drop_database(database);
    create_collection(database, "users");
    create_collection(database, "orders");
    enable_pre_images(database, "users");
    db_uri(uri, sizeof(uri), database);
    make_temp_store(store, sizeof(store));

    watcher = start_watcher(store, uri);
    wait_for_log(watcher.stderr_path, "watching MongoDB change stream", 60);
    run_transaction_two_writes(database);
    wait_for_log(watcher.stderr_path, "transaction 2 events", 60);
    wait_for_log(watcher.stderr_path, "users/\"alice\"", 60);
    wait_for_log(watcher.stderr_path, "orders/\"order-1\"", 60);
    stop_watcher(&watcher);

    result = run_scribe(store, "log --oneline -n 1");
    assert_success(&result, "transaction log");
    assert_output_contains(&result, "mongo transaction", "transaction log");
    command_result_free(&result);

    result = run_scribe(store, "diff HEAD~1 HEAD");
    assert_success(&result, "transaction diff");
    assert_output_contains(&result, "A ", "transaction diff");
    assert_output_contains(&result, "users/\"alice\"", "transaction diff");
    assert_output_contains(&result, "orders/\"order-1\"", "transaction diff");
    command_result_free(&result);
}

static void test_ddl_policy_and_bootstrap_recovery(void) {
    char database[96];
    char uri[512];
    char store[PATH_MAX];
    watcher_process watcher;
    command_result before;
    command_result after;
    command_result diff;

    db_name(database, sizeof(database), "ddl");
    drop_database(database);
    insert_user_role(database, "alice", "user");
    enable_pre_images(database, "users");
    db_uri(uri, sizeof(uri), database);
    make_temp_store(store, sizeof(store));

    watcher = start_watcher(store, uri);
    wait_for_log(watcher.stderr_path, "watching MongoDB change stream", 60);

    before = run_scribe(store, "log --oneline");
    assert_success(&before, "log before ignored DDL");
    create_index_on_role(database);
    wait_for_log(watcher.stderr_path, "ignored MongoDB change stream event 'createIndexes'", 60);
    sleep(1u);
    after = run_scribe(store, "log --oneline");
    assert_success(&after, "log after ignored DDL");
    check(strcmp(before.stdout_bytes, after.stdout_bytes) == 0,
          "createIndexes should not create a commit\nbefore:\n%s\nafter:\n%s", before.stdout_bytes, after.stdout_bytes);
    command_result_free(&before);
    command_result_free(&after);

    drop_users_collection(database);
    wait_for_log(watcher.stderr_path, "change stream resume token is unusable; restarting bootstrap", 60);
    wait_for_log(watcher.stderr_path, "bootstrap commit", 60);
    stop_watcher(&watcher);

    diff = run_scribe(store, "diff HEAD~1 HEAD");
    assert_success(&diff, "drop recovery diff");
    assert_output_contains(&diff, "D ", "drop recovery diff");
    assert_output_contains(&diff, "users/\"alice\"", "drop recovery diff");
    command_result_free(&diff);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scribe-binary>\n", argv[0]);
        return 2;
    }
    g_scribe_bin = argv[1];
    mongoc_init();
    start_mongo_container();
    test_bootstrap_tree_and_canonical_json();
    test_change_stream_document_lifecycle();
    test_transaction_becomes_one_commit();
    test_ddl_policy_and_bootstrap_recovery();
    cleanup_container();
    mongoc_cleanup();
    puts("scribe_mongo_testcontainers: passed");
    return 0;
}
