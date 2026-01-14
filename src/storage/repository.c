/*
 * Scribe - A protocol for Verifiable Data Lineage
 * storage/repository.c - Repository management
 */

#include "scribe/storage/repository.h"
#include "scribe/storage/database.h"
#include "scribe/core/hash.h"
#include "scribe/error.h"

#include "yyjson.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define SCRIBE_DIR_NAME ".scribe"
#define DB_FILE_NAME "scribe.db"
#define CONFIG_FILE_NAME "config.json"
#define OBJECTS_DIR_NAME "objects"

/* External path functions */
extern char *scribe_path_join(const char *base, const char *component);
extern scribe_error_t scribe_path_mkdir(const char *path);
extern int scribe_path_exists(const char *path);
extern int scribe_path_is_dir(const char *path);
extern char *scribe_find_repo_root(const char *start_path);
extern char *scribe_path_getcwd(void);

struct scribe_repo {
    char *root_path;      /* Path to .scribe directory */
    char *db_path;        /* Path to scribe.db */
    char *objects_path;   /* Path to objects/ directory */
    char *config_path;    /* Path to config.json */
    scribe_db_t *db;      /* Database handle */
    scribe_config_t *config;
};

static scribe_repo_t *repo_alloc(void)
{
    return calloc(1, sizeof(scribe_repo_t));
}

scribe_repo_t *scribe_repo_open(const char *path)
{
    char *scribe_path = NULL;

    if (path) {
        scribe_path = scribe_find_repo_root(path);
    } else {
        char *cwd = scribe_path_getcwd();
        if (cwd) {
            scribe_path = scribe_find_repo_root(cwd);
            free(cwd);
        }
    }

    if (!scribe_path) {
        scribe_set_error_detail("Not a scribe repository (or any parent)");
        return NULL;
    }

    scribe_repo_t *repo = repo_alloc();
    if (!repo) {
        free(scribe_path);
        return NULL;
    }

    repo->root_path = scribe_path;
    repo->db_path = scribe_path_join(scribe_path, DB_FILE_NAME);
    repo->objects_path = scribe_path_join(scribe_path, OBJECTS_DIR_NAME);
    repo->config_path = scribe_path_join(scribe_path, CONFIG_FILE_NAME);

    /* Open database */
    repo->db = scribe_db_open(repo->db_path);
    if (!repo->db) {
        scribe_repo_close(repo);
        return NULL;
    }

    return repo;
}

scribe_repo_t *scribe_repo_init(const char *path)
{
    char *base_path = NULL;

    if (path) {
        base_path = strdup(path);
    } else {
        base_path = scribe_path_getcwd();
    }

    if (!base_path) return NULL;

    /* Check if already a repo */
    char *existing = scribe_find_repo_root(base_path);
    if (existing) {
        scribe_set_error_detail("Repository already exists at %s", existing);
        free(existing);
        free(base_path);
        return NULL;
    }

    /* Create .scribe directory */
    char *scribe_path = scribe_path_join(base_path, SCRIBE_DIR_NAME);
    free(base_path);

    if (!scribe_path) return NULL;

    scribe_error_t err = scribe_path_mkdir(scribe_path);
    if (err != SCRIBE_OK) {
        free(scribe_path);
        return NULL;
    }

    /* Create objects directory */
    char *objects_path = scribe_path_join(scribe_path, OBJECTS_DIR_NAME);
    if (objects_path) {
        scribe_path_mkdir(objects_path);
        free(objects_path);
    }

    /* Initialize repository struct */
    scribe_repo_t *repo = repo_alloc();
    if (!repo) {
        free(scribe_path);
        return NULL;
    }

    repo->root_path = scribe_path;
    repo->db_path = scribe_path_join(scribe_path, DB_FILE_NAME);
    repo->objects_path = scribe_path_join(scribe_path, OBJECTS_DIR_NAME);
    repo->config_path = scribe_path_join(scribe_path, CONFIG_FILE_NAME);

    /* Create and initialize database */
    repo->db = scribe_db_open(repo->db_path);
    if (!repo->db) {
        scribe_repo_close(repo);
        return NULL;
    }

    err = scribe_db_init_schema(repo->db);
    if (err != SCRIBE_OK) {
        scribe_repo_close(repo);
        return NULL;
    }

    /* Create default config */
    scribe_config_t config = {0};
    config.author_id = "user:anonymous";
    config.author_role = "developer";
    scribe_config_save(repo, &config);

    return repo;
}

void scribe_repo_close(scribe_repo_t *repo)
{
    if (!repo) return;

    if (repo->db) scribe_db_close(repo->db);
    scribe_config_free(repo->config);

    free(repo->root_path);
    free(repo->db_path);
    free(repo->objects_path);
    free(repo->config_path);
    free(repo);
}

int scribe_repo_exists(const char *path)
{
    char *scribe_path = scribe_find_repo_root(path);
    if (scribe_path) {
        free(scribe_path);
        return 1;
    }
    return 0;
}

const char *scribe_repo_get_root(const scribe_repo_t *repo)
{
    return repo ? repo->root_path : NULL;
}

const char *scribe_repo_get_db_path(const scribe_repo_t *repo)
{
    return repo ? repo->db_path : NULL;
}

const char *scribe_repo_get_objects_path(const scribe_repo_t *repo)
{
    return repo ? repo->objects_path : NULL;
}

scribe_config_t *scribe_config_load(const scribe_repo_t *repo)
{
    if (!repo || !repo->config_path) return NULL;

    FILE *fp = fopen(repo->config_path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return NULL;
    }

    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(fp);
        return NULL;
    }

    size_t read = fread(json, 1, (size_t)size, fp);
    fclose(fp);
    json[read] = '\0';

    yyjson_doc *doc = yyjson_read(json, read, 0);
    free(json);

    if (!doc) return NULL;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        return NULL;
    }

    scribe_config_t *config = calloc(1, sizeof(scribe_config_t));
    if (!config) {
        yyjson_doc_free(doc);
        return NULL;
    }

    const char *val;
    val = yyjson_get_str(yyjson_obj_get(root, "author_id"));
    if (val) config->author_id = strdup(val);

    val = yyjson_get_str(yyjson_obj_get(root, "author_role"));
    if (val) config->author_role = strdup(val);

    val = yyjson_get_str(yyjson_obj_get(root, "pg_connection_string"));
    if (val) config->pg_connection_string = strdup(val);

    /* Parse watched_tables array */
    yyjson_val *tables = yyjson_obj_get(root, "watched_tables");
    if (tables && yyjson_is_arr(tables)) {
        size_t count = yyjson_arr_size(tables);
        config->watched_tables = calloc(count, sizeof(char *));
        config->watched_table_count = count;

        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(tables, idx, max, item) {
            const char *table = yyjson_get_str(item);
            if (table) {
                config->watched_tables[idx] = strdup(table);
            }
        }
    }

    yyjson_doc_free(doc);
    return config;
}

scribe_error_t scribe_config_save(const scribe_repo_t *repo, const scribe_config_t *config)
{
    if (!repo || !repo->config_path || !config) return SCRIBE_ERR_INVALID_ARG;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return SCRIBE_ERR_NOMEM;

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (config->author_id)
        yyjson_mut_obj_add_str(doc, root, "author_id", config->author_id);
    if (config->author_role)
        yyjson_mut_obj_add_str(doc, root, "author_role", config->author_role);
    if (config->pg_connection_string)
        yyjson_mut_obj_add_str(doc, root, "pg_connection_string", config->pg_connection_string);

    if (config->watched_tables && config->watched_table_count > 0) {
        yyjson_mut_val *tables = yyjson_mut_arr(doc);
        for (size_t i = 0; i < config->watched_table_count; i++) {
            if (config->watched_tables[i]) {
                yyjson_mut_arr_add_str(doc, tables, config->watched_tables[i]);
            }
        }
        yyjson_mut_obj_add_val(doc, root, "watched_tables", tables);
    }

    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(doc);

    if (!json) return SCRIBE_ERR_NOMEM;

    FILE *fp = fopen(repo->config_path, "w");
    if (!fp) {
        free(json);
        return SCRIBE_ERR_IO;
    }

    fputs(json, fp);
    fclose(fp);
    free(json);

    return SCRIBE_OK;
}

void scribe_config_free(scribe_config_t *config)
{
    if (!config) return;

    free(config->author_id);
    free(config->author_role);
    free(config->pg_connection_string);

    for (size_t i = 0; i < config->watched_table_count; i++) {
        free(config->watched_tables[i]);
    }
    free(config->watched_tables);

    free(config);
}

scribe_error_t scribe_repo_get_head(const scribe_repo_t *repo, scribe_hash_t *out_hash)
{
    if (!repo || !repo->db || !out_hash) return SCRIBE_ERR_INVALID_ARG;
    return scribe_db_get_ref(repo->db, "HEAD", out_hash);
}

scribe_error_t scribe_repo_set_head(scribe_repo_t *repo, const scribe_hash_t *hash)
{
    if (!repo || !repo->db || !hash) return SCRIBE_ERR_INVALID_ARG;
    return scribe_db_set_ref(repo->db, "HEAD", hash);
}

scribe_error_t scribe_repo_store_commit(scribe_repo_t *repo, const scribe_envelope_t *env)
{
    if (!repo || !repo->db || !env) return SCRIBE_ERR_INVALID_ARG;
    return scribe_db_store_commit(repo->db, env);
}

scribe_envelope_t *scribe_repo_load_commit(scribe_repo_t *repo, const scribe_hash_t *hash)
{
    if (!repo || !repo->db || !hash) return NULL;
    return scribe_db_load_commit(repo->db, hash);
}

scribe_commit_list_t *scribe_repo_get_history(scribe_repo_t *repo,
                                               const scribe_hash_t *from,
                                               size_t limit)
{
    if (!repo || !repo->db) return NULL;

    scribe_db_commit_list_t *db_list = scribe_db_get_history(repo->db, from, limit);
    if (!db_list) return NULL;

    /* Convert to public type (same structure) */
    scribe_commit_list_t *list = calloc(1, sizeof(scribe_commit_list_t));
    if (!list) {
        scribe_db_commit_list_free(db_list);
        return NULL;
    }

    list->hashes = db_list->hashes;
    list->count = db_list->count;

    /* Free the container but not the hashes array */
    free(db_list);

    return list;
}

void scribe_commit_list_free(scribe_commit_list_t *list)
{
    if (!list) return;
    free(list->hashes);
    free(list);
}
