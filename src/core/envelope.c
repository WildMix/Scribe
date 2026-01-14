/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/envelope.c - Commit envelope implementation
 */

#include "scribe/core/envelope.h"
#include "scribe/core/hash.h"
#include "scribe/core/merkle.h"
#include "scribe/error.h"

#include "yyjson.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_CHANGE_CAPACITY 8

static char *safe_strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void free_author(scribe_author_t *author)
{
    if (!author) return;
    free(author->id);
    free(author->role);
    free(author->email);
}

static void free_process(scribe_process_t *process)
{
    if (!process) return;
    free(process->name);
    free(process->version);
    free(process->params);
    free(process->source);
}

static void free_change(scribe_change_t *change)
{
    if (!change) return;
    free(change->table_name);
    free(change->operation);
    free(change->primary_key);
}

scribe_envelope_t *scribe_envelope_new(void)
{
    scribe_envelope_t *env = calloc(1, sizeof(scribe_envelope_t));
    if (!env) return NULL;

    env->changes = calloc(INITIAL_CHANGE_CAPACITY, sizeof(scribe_change_t));
    if (!env->changes) {
        free(env);
        return NULL;
    }
    env->change_capacity = INITIAL_CHANGE_CAPACITY;
    env->timestamp = time(NULL);

    return env;
}

void scribe_envelope_free(scribe_envelope_t *env)
{
    if (!env) return;

    free_author(&env->author);
    free_process(&env->process);
    free(env->message);

    for (size_t i = 0; i < env->change_count; i++) {
        free_change(&env->changes[i]);
    }
    free(env->changes);

    free(env);
}

scribe_envelope_t *scribe_envelope_clone(const scribe_envelope_t *env)
{
    if (!env) return NULL;

    scribe_envelope_t *clone = scribe_envelope_new();
    if (!clone) return NULL;

    scribe_hash_copy(&clone->commit_id, &env->commit_id);
    scribe_hash_copy(&clone->parent_id, &env->parent_id);
    scribe_hash_copy(&clone->tree_hash, &env->tree_hash);

    scribe_envelope_set_author(clone, env->author.id, env->author.role);
    if (env->author.email) scribe_envelope_set_author_email(clone, env->author.email);

    scribe_envelope_set_process(clone, env->process.name, env->process.version, env->process.params);
    if (env->process.source) scribe_envelope_set_process_source(clone, env->process.source);

    if (env->message) scribe_envelope_set_message(clone, env->message);
    clone->timestamp = env->timestamp;

    for (size_t i = 0; i < env->change_count; i++) {
        scribe_envelope_add_change(clone,
                                   env->changes[i].table_name,
                                   env->changes[i].operation,
                                   env->changes[i].primary_key,
                                   &env->changes[i].before_hash,
                                   &env->changes[i].after_hash);
    }

    return clone;
}

scribe_error_t scribe_envelope_set_author(scribe_envelope_t *env,
                                          const char *id,
                                          const char *role)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    free(env->author.id);
    free(env->author.role);

    env->author.id = safe_strdup(id);
    env->author.role = safe_strdup(role);

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_set_author_email(scribe_envelope_t *env,
                                                const char *email)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    free(env->author.email);
    env->author.email = safe_strdup(email);

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_set_process(scribe_envelope_t *env,
                                           const char *name,
                                           const char *version,
                                           const char *params)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    free(env->process.name);
    free(env->process.version);
    free(env->process.params);

    env->process.name = safe_strdup(name);
    env->process.version = safe_strdup(version);
    env->process.params = safe_strdup(params);

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_set_process_source(scribe_envelope_t *env,
                                                   const char *source)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    free(env->process.source);
    env->process.source = safe_strdup(source);

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_set_parent(scribe_envelope_t *env,
                                          const scribe_hash_t *parent)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    if (parent) {
        scribe_hash_copy(&env->parent_id, parent);
    } else {
        memset(&env->parent_id, 0, sizeof(scribe_hash_t));
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_set_message(scribe_envelope_t *env,
                                           const char *message)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    free(env->message);
    env->message = safe_strdup(message);

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_set_tree_hash(scribe_envelope_t *env,
                                             const scribe_hash_t *tree_hash)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    if (tree_hash) {
        scribe_hash_copy(&env->tree_hash, tree_hash);
    } else {
        memset(&env->tree_hash, 0, sizeof(scribe_hash_t));
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_add_change(scribe_envelope_t *env,
                                          const char *table_name,
                                          const char *operation,
                                          const char *primary_key,
                                          const scribe_hash_t *before_hash,
                                          const scribe_hash_t *after_hash)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    /* Expand if needed */
    if (env->change_count >= env->change_capacity) {
        size_t new_cap = env->change_capacity * 2;
        scribe_change_t *new_changes = realloc(env->changes, new_cap * sizeof(scribe_change_t));
        if (!new_changes) return SCRIBE_ERR_NOMEM;
        env->changes = new_changes;
        env->change_capacity = new_cap;
    }

    scribe_change_t *change = &env->changes[env->change_count];
    memset(change, 0, sizeof(scribe_change_t));

    change->table_name = safe_strdup(table_name);
    change->operation = safe_strdup(operation);
    change->primary_key = safe_strdup(primary_key);

    if (before_hash) scribe_hash_copy(&change->before_hash, before_hash);
    if (after_hash) scribe_hash_copy(&change->after_hash, after_hash);

    env->change_count++;
    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_finalize(scribe_envelope_t *env)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    /* Compute tree hash from changes if not already set */
    if (scribe_hash_is_zero(&env->tree_hash) && env->change_count > 0) {
        scribe_merkle_tree_t *tree = scribe_merkle_tree_new();
        if (!tree) return SCRIBE_ERR_NOMEM;

        for (size_t i = 0; i < env->change_count; i++) {
            /* Add both before and after hashes to the tree */
            if (!scribe_hash_is_zero(&env->changes[i].before_hash)) {
                scribe_merkle_add_hash(tree, "before", &env->changes[i].before_hash);
            }
            if (!scribe_hash_is_zero(&env->changes[i].after_hash)) {
                scribe_merkle_add_hash(tree, "after", &env->changes[i].after_hash);
            }
        }

        scribe_merkle_build(tree);
        const scribe_hash_t *root = scribe_merkle_root(tree);
        if (root) {
            scribe_hash_copy(&env->tree_hash, root);
        }
        scribe_merkle_tree_free(tree);
    }

    /* Compute commit_id from envelope content */
    char *json = scribe_envelope_to_json(env);
    if (!json) return SCRIBE_ERR_NOMEM;

    /* Temporarily zero the commit_id for hashing */
    scribe_hash_t saved_id;
    scribe_hash_copy(&saved_id, &env->commit_id);
    memset(&env->commit_id, 0, sizeof(scribe_hash_t));

    /* Re-serialize without commit_id for hashing */
    free(json);
    json = scribe_envelope_to_json(env);
    if (json) {
        scribe_hash_bytes(json, strlen(json), &env->commit_id);
        free(json);
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_envelope_verify(const scribe_envelope_t *env)
{
    if (!env) return SCRIBE_ERR_INVALID_ARG;

    /* Verify by recomputing commit_id */
    scribe_envelope_t *copy = scribe_envelope_clone(env);
    if (!copy) return SCRIBE_ERR_NOMEM;

    memset(&copy->commit_id, 0, sizeof(scribe_hash_t));

    char *json = scribe_envelope_to_json(copy);
    scribe_envelope_free(copy);

    if (!json) return SCRIBE_ERR_NOMEM;

    scribe_hash_t computed;
    scribe_hash_bytes(json, strlen(json), &computed);
    free(json);

    if (!scribe_hash_equal(&computed, &env->commit_id)) {
        return SCRIBE_ERR_HASH_MISMATCH;
    }

    return SCRIBE_OK;
}

char *scribe_envelope_to_json(const scribe_envelope_t *env)
{
    if (!env) return NULL;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    /* commit_id */
    if (!scribe_hash_is_zero(&env->commit_id)) {
        char hex[SCRIBE_HASH_HEX_SIZE];
        scribe_hash_to_hex(&env->commit_id, hex);
        yyjson_mut_obj_add_str(doc, root, "commit_id", hex);
    }

    /* parent_id */
    if (!scribe_hash_is_zero(&env->parent_id)) {
        char hex[SCRIBE_HASH_HEX_SIZE];
        scribe_hash_to_hex(&env->parent_id, hex);
        yyjson_mut_obj_add_str(doc, root, "parent_id", hex);
    }

    /* tree_hash */
    if (!scribe_hash_is_zero(&env->tree_hash)) {
        char hex[SCRIBE_HASH_HEX_SIZE];
        scribe_hash_to_hex(&env->tree_hash, hex);
        yyjson_mut_obj_add_str(doc, root, "tree_hash", hex);
    }

    /* author */
    yyjson_mut_val *author = yyjson_mut_obj(doc);
    if (env->author.id) yyjson_mut_obj_add_str(doc, author, "id", env->author.id);
    if (env->author.role) yyjson_mut_obj_add_str(doc, author, "role", env->author.role);
    if (env->author.email) yyjson_mut_obj_add_str(doc, author, "email", env->author.email);
    yyjson_mut_obj_add_val(doc, root, "author", author);

    /* process */
    yyjson_mut_val *process = yyjson_mut_obj(doc);
    if (env->process.name) yyjson_mut_obj_add_str(doc, process, "name", env->process.name);
    if (env->process.version) yyjson_mut_obj_add_str(doc, process, "version", env->process.version);
    if (env->process.params) yyjson_mut_obj_add_str(doc, process, "params", env->process.params);
    if (env->process.source) yyjson_mut_obj_add_str(doc, process, "source", env->process.source);
    yyjson_mut_obj_add_val(doc, root, "process", process);

    /* timestamp */
    yyjson_mut_obj_add_int(doc, root, "timestamp", (int64_t)env->timestamp);

    /* message */
    if (env->message) {
        yyjson_mut_obj_add_str(doc, root, "message", env->message);
    }

    /* changes */
    if (env->change_count > 0) {
        yyjson_mut_val *changes = yyjson_mut_arr(doc);
        for (size_t i = 0; i < env->change_count; i++) {
            yyjson_mut_val *change = yyjson_mut_obj(doc);

            if (env->changes[i].table_name)
                yyjson_mut_obj_add_str(doc, change, "table", env->changes[i].table_name);
            if (env->changes[i].operation)
                yyjson_mut_obj_add_str(doc, change, "operation", env->changes[i].operation);
            if (env->changes[i].primary_key)
                yyjson_mut_obj_add_str(doc, change, "pk", env->changes[i].primary_key);

            if (!scribe_hash_is_zero(&env->changes[i].before_hash)) {
                char hex[SCRIBE_HASH_HEX_SIZE];
                scribe_hash_to_hex(&env->changes[i].before_hash, hex);
                yyjson_mut_obj_add_str(doc, change, "before_hash", hex);
            }

            if (!scribe_hash_is_zero(&env->changes[i].after_hash)) {
                char hex[SCRIBE_HASH_HEX_SIZE];
                scribe_hash_to_hex(&env->changes[i].after_hash, hex);
                yyjson_mut_obj_add_str(doc, change, "after_hash", hex);
            }

            yyjson_mut_arr_add_val(changes, change);
        }
        yyjson_mut_obj_add_val(doc, root, "changes", changes);
    }

    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(doc);

    return json;
}

scribe_envelope_t *scribe_envelope_from_json(const char *json)
{
    if (!json) return NULL;

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return NULL;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        return NULL;
    }

    scribe_envelope_t *env = scribe_envelope_new();
    if (!env) {
        yyjson_doc_free(doc);
        return NULL;
    }

    /* Parse commit_id */
    yyjson_val *val = yyjson_obj_get(root, "commit_id");
    if (val && yyjson_is_str(val)) {
        scribe_hash_from_hex(yyjson_get_str(val), &env->commit_id);
    }

    /* Parse parent_id */
    val = yyjson_obj_get(root, "parent_id");
    if (val && yyjson_is_str(val)) {
        scribe_hash_from_hex(yyjson_get_str(val), &env->parent_id);
    }

    /* Parse tree_hash */
    val = yyjson_obj_get(root, "tree_hash");
    if (val && yyjson_is_str(val)) {
        scribe_hash_from_hex(yyjson_get_str(val), &env->tree_hash);
    }

    /* Parse author */
    yyjson_val *author = yyjson_obj_get(root, "author");
    if (author) {
        const char *id = yyjson_get_str(yyjson_obj_get(author, "id"));
        const char *role = yyjson_get_str(yyjson_obj_get(author, "role"));
        scribe_envelope_set_author(env, id, role);

        const char *email = yyjson_get_str(yyjson_obj_get(author, "email"));
        if (email) scribe_envelope_set_author_email(env, email);
    }

    /* Parse process */
    yyjson_val *process = yyjson_obj_get(root, "process");
    if (process) {
        const char *name = yyjson_get_str(yyjson_obj_get(process, "name"));
        const char *version = yyjson_get_str(yyjson_obj_get(process, "version"));
        const char *params = yyjson_get_str(yyjson_obj_get(process, "params"));
        scribe_envelope_set_process(env, name, version, params);

        const char *source = yyjson_get_str(yyjson_obj_get(process, "source"));
        if (source) scribe_envelope_set_process_source(env, source);
    }

    /* Parse timestamp */
    val = yyjson_obj_get(root, "timestamp");
    if (val && yyjson_is_int(val)) {
        env->timestamp = (time_t)yyjson_get_int(val);
    }

    /* Parse message */
    val = yyjson_obj_get(root, "message");
    if (val && yyjson_is_str(val)) {
        scribe_envelope_set_message(env, yyjson_get_str(val));
    }

    /* Parse changes */
    yyjson_val *changes = yyjson_obj_get(root, "changes");
    if (changes && yyjson_is_arr(changes)) {
        size_t idx, max;
        yyjson_val *change;
        yyjson_arr_foreach(changes, idx, max, change) {
            const char *table = yyjson_get_str(yyjson_obj_get(change, "table"));
            const char *op = yyjson_get_str(yyjson_obj_get(change, "operation"));
            const char *pk = yyjson_get_str(yyjson_obj_get(change, "pk"));

            scribe_hash_t before = {0}, after = {0};
            const char *before_str = yyjson_get_str(yyjson_obj_get(change, "before_hash"));
            const char *after_str = yyjson_get_str(yyjson_obj_get(change, "after_hash"));
            if (before_str) scribe_hash_from_hex(before_str, &before);
            if (after_str) scribe_hash_from_hex(after_str, &after);

            scribe_envelope_add_change(env, table, op, pk, &before, &after);
        }
    }

    yyjson_doc_free(doc);
    return env;
}
