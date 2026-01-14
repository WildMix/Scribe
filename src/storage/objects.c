/*
 * Scribe - A protocol for Verifiable Data Lineage
 * storage/objects.c - Filesystem object storage
 */

#include "scribe/types.h"
#include "scribe/error.h"
#include "scribe/core/hash.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

/* External path functions */
extern char *scribe_path_join(const char *base, const char *component);
extern scribe_error_t scribe_path_mkdir(const char *path);
extern int scribe_path_exists(const char *path);

/*
 * Get the object path for a given hash
 * Format: objects/XX/YYYYYYYY... (first 2 chars as directory)
 */
static char *get_object_path(const char *objects_dir, const scribe_hash_t *hash)
{
    char hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(hash, hex);

    /* Create subdirectory name (first 2 chars) */
    char subdir[3] = {hex[0], hex[1], '\0'};

    /* Build path: objects_dir/XX/YYY... */
    char *dir_path = scribe_path_join(objects_dir, subdir);
    if (!dir_path) return NULL;

    char *file_path = scribe_path_join(dir_path, hex + 2);
    free(dir_path);

    return file_path;
}

/*
 * Store an object by its hash
 */
scribe_error_t scribe_objects_store(const char *objects_dir,
                                    const scribe_hash_t *hash,
                                    const void *content,
                                    size_t size)
{
    if (!objects_dir || !hash || !content) return SCRIBE_ERR_INVALID_ARG;

    char *path = get_object_path(objects_dir, hash);
    if (!path) return SCRIBE_ERR_NOMEM;

    /* Check if already exists */
    if (scribe_path_exists(path)) {
        free(path);
        return SCRIBE_OK;  /* Already stored */
    }

    /* Create parent directory */
    char hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(hash, hex);
    char subdir[3] = {hex[0], hex[1], '\0'};
    char *dir_path = scribe_path_join(objects_dir, subdir);
    if (dir_path) {
        scribe_path_mkdir(dir_path);
        free(dir_path);
    }

    /* Write to temporary file first, then rename */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        scribe_set_error_detail("Failed to open %s: %s", tmp_path, strerror(errno));
        free(path);
        return SCRIBE_ERR_IO;
    }

    size_t written = fwrite(content, 1, size, fp);
    if (written != size) {
        fclose(fp);
        unlink(tmp_path);
        free(path);
        return SCRIBE_ERR_IO;
    }

    if (fclose(fp) != 0) {
        unlink(tmp_path);
        free(path);
        return SCRIBE_ERR_IO;
    }

    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        free(path);
        return SCRIBE_ERR_IO;
    }

    free(path);
    return SCRIBE_OK;
}

/*
 * Load an object by its hash
 */
scribe_error_t scribe_objects_load(const char *objects_dir,
                                   const scribe_hash_t *hash,
                                   void **out_content,
                                   size_t *out_size)
{
    if (!objects_dir || !hash || !out_content || !out_size) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    char *path = get_object_path(objects_dir, hash);
    if (!path) return SCRIBE_ERR_NOMEM;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        free(path);
        return SCRIBE_ERR_OBJECT_MISSING;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(fp);
        free(path);
        return SCRIBE_ERR_IO;
    }

    /* Allocate and read */
    void *content = malloc((size_t)file_size);
    if (!content) {
        fclose(fp);
        free(path);
        return SCRIBE_ERR_NOMEM;
    }

    size_t read_size = fread(content, 1, (size_t)file_size, fp);
    fclose(fp);
    free(path);

    if (read_size != (size_t)file_size) {
        free(content);
        return SCRIBE_ERR_IO;
    }

    *out_content = content;
    *out_size = read_size;

    return SCRIBE_OK;
}

/*
 * Check if an object exists
 */
int scribe_objects_exists(const char *objects_dir, const scribe_hash_t *hash)
{
    if (!objects_dir || !hash) return 0;

    char *path = get_object_path(objects_dir, hash);
    if (!path) return 0;

    int exists = scribe_path_exists(path);
    free(path);

    return exists;
}

/*
 * Delete an object
 */
scribe_error_t scribe_objects_delete(const char *objects_dir, const scribe_hash_t *hash)
{
    if (!objects_dir || !hash) return SCRIBE_ERR_INVALID_ARG;

    char *path = get_object_path(objects_dir, hash);
    if (!path) return SCRIBE_ERR_NOMEM;

    int result = unlink(path);
    free(path);

    return (result == 0) ? SCRIBE_OK : SCRIBE_ERR_IO;
}
