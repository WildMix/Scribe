/*
 * Scribe - A protocol for Verifiable Data Lineage
 * util/path.c - Path manipulation utilities
 */

#include "scribe/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#define SCRIBE_DIR_NAME ".scribe"

/*
 * Join two path components
 *
 * @param base      Base path
 * @param component Path component to append
 * @return          Newly allocated joined path (caller must free), or NULL on error
 */
char *scribe_path_join(const char *base, const char *component)
{
    if (!base || !component) return NULL;

    size_t base_len = strlen(base);
    size_t comp_len = strlen(component);

    /* Remove trailing slash from base */
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }

    /* Skip leading slash from component */
    while (*component == '/') {
        component++;
        comp_len--;
    }

    /* Allocate: base + '/' + component + '\0' */
    char *result = malloc(base_len + 1 + comp_len + 1);
    if (!result) return NULL;

    memcpy(result, base, base_len);
    result[base_len] = '/';
    memcpy(result + base_len + 1, component, comp_len);
    result[base_len + 1 + comp_len] = '\0';

    return result;
}

/*
 * Check if a path exists
 */
int scribe_path_exists(const char *path)
{
    if (!path) return 0;
    struct stat st;
    return stat(path, &st) == 0;
}

/*
 * Check if a path is a directory
 */
int scribe_path_is_dir(const char *path)
{
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/*
 * Create a directory (and parents if needed)
 */
scribe_error_t scribe_path_mkdir(const char *path)
{
    if (!path) return SCRIBE_ERR_INVALID_ARG;

    char *tmp = strdup(path);
    if (!tmp) return SCRIBE_ERR_NOMEM;

    size_t len = strlen(tmp);

    /* Remove trailing slash */
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    /* Create parent directories */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp);
                return SCRIBE_ERR_IO;
            }
            *p = '/';
        }
    }

    /* Create final directory */
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return SCRIBE_ERR_IO;
    }

    free(tmp);
    return SCRIBE_OK;
}

/*
 * Find the scribe repository root from a given path
 * Searches upward through parent directories
 *
 * @param start_path    Starting path for search
 * @return              Newly allocated path to .scribe directory, or NULL if not found
 */
char *scribe_find_repo_root(const char *start_path)
{
    if (!start_path) return NULL;

    char resolved[PATH_MAX];
    if (!realpath(start_path, resolved)) {
        return NULL;
    }

    char *current = strdup(resolved);
    if (!current) return NULL;

    while (1) {
        char *scribe_path = scribe_path_join(current, SCRIBE_DIR_NAME);
        if (!scribe_path) {
            free(current);
            return NULL;
        }

        if (scribe_path_is_dir(scribe_path)) {
            free(current);
            return scribe_path;
        }

        free(scribe_path);

        /* Move to parent directory */
        char *last_slash = strrchr(current, '/');
        if (!last_slash || last_slash == current) {
            /* Reached root */
            free(current);
            return NULL;
        }
        *last_slash = '\0';
    }
}

/*
 * Get the directory containing a path
 */
char *scribe_path_dirname(const char *path)
{
    if (!path) return NULL;

    char *copy = strdup(path);
    if (!copy) return NULL;

    char *last_slash = strrchr(copy, '/');
    if (!last_slash) {
        free(copy);
        return strdup(".");
    }

    if (last_slash == copy) {
        /* Root directory */
        free(copy);
        return strdup("/");
    }

    *last_slash = '\0';
    return copy;
}

/*
 * Get the filename from a path
 */
const char *scribe_path_basename(const char *path)
{
    if (!path) return NULL;

    const char *last_slash = strrchr(path, '/');
    return last_slash ? last_slash + 1 : path;
}

/*
 * Get the current working directory
 */
char *scribe_path_getcwd(void)
{
    char *cwd = malloc(PATH_MAX);
    if (!cwd) return NULL;

    if (!getcwd(cwd, PATH_MAX)) {
        free(cwd);
        return NULL;
    }

    return cwd;
}
