#include "core/internal.h"

#include "util/error.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

scribe_error_t scribe_default_config(scribe_config *cfg) {
    if (cfg == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "config is NULL");
    }
    cfg->scribe_format_version = 1;
    cfg->compression_level = 3;
    cfg->worker_threads = 0;
    cfg->event_queue_capacity = 64;
    cfg->queue_stall_warn_seconds = 30;
    cfg->adapter_require_pre_post_images = false;
    cfg->adapter_coalesce_window_ms = 0;
    strcpy(cfg->adapter_excluded_databases, "admin,local,config");
    return SCRIBE_OK;
}

scribe_error_t scribe_write_config(const char *repo_path, const scribe_config *cfg) {
    char *path;
    char buf[512];
    int n;
    scribe_error_t err;

    path = scribe_path_join(repo_path, "config");
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    n = snprintf(buf, sizeof(buf),
                 "scribe_format_version = %d\n"
                 "hash_algorithm = blake3-256\n"
                 "compression = zstd\n"
                 "compression_level = %d\n"
                 "worker_threads = %d\n"
                 "event_queue_capacity = %zu\n"
                 "queue_stall_warn_seconds = %d\n"
                 "\n"
                 "adapter.name = mongodb\n"
                 "adapter.mongodb.excluded_databases = %s\n"
                 "adapter.mongodb.require_pre_post_images = %s\n"
                 "adapter.mongodb.coalesce_window_ms = %d\n",
                 cfg->scribe_format_version, cfg->compression_level, cfg->worker_threads, cfg->event_queue_capacity,
                 cfg->queue_stall_warn_seconds, cfg->adapter_excluded_databases,
                 cfg->adapter_require_pre_post_images ? "true" : "false", cfg->adapter_coalesce_window_ms);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        free(path);
        return scribe_set_error(SCRIBE_ECONFIG, "config is too large");
    }
    err = scribe_write_file_atomic(path, (const uint8_t *)buf, (size_t)n);
    free(path);
    return err;
}

static char *trim(char *s) {
    char *end;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static scribe_error_t parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 2147483647L) {
        return scribe_set_error(SCRIBE_ECONFIG, "invalid integer value '%s'", s);
    }
    *out = (int)v;
    return SCRIBE_OK;
}

static scribe_error_t parse_size(const char *s, size_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') {
        return scribe_set_error(SCRIBE_ECONFIG, "invalid size value '%s'", s);
    }
    *out = (size_t)v;
    return SCRIBE_OK;
}

scribe_error_t scribe_read_config(const char *repo_path, scribe_config *cfg) {
    char *path;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char *save = NULL;
    char *line;
    unsigned seen = 0;
    scribe_error_t err;

    err = scribe_default_config(cfg);
    if (err != SCRIBE_OK) {
        return err;
    }
    path = scribe_path_join(repo_path, "config");
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &bytes, &len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    (void)len;
    for (line = strtok_r((char *)bytes, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char *eq;
        char *key;
        char *value;

        key = trim(line);
        if (key[0] == '\0' || key[0] == '#') {
            continue;
        }
        eq = strchr(key, '=');
        if (eq == NULL) {
            free(bytes);
            return scribe_set_error(SCRIBE_ECONFIG, "malformed config line");
        }
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);
        if (strcmp(key, "scribe_format_version") == 0) {
            if ((err = parse_int(value, &cfg->scribe_format_version)) != SCRIBE_OK) {
                free(bytes);
                return err;
            }
            seen |= 1u << 0;
        } else if (strcmp(key, "hash_algorithm") == 0) {
            if (strcmp(value, "blake3-256") != 0) {
                free(bytes);
                return scribe_set_error(SCRIBE_EHASH, "unsupported hash algorithm '%s'", value);
            }
            seen |= 1u << 1;
        } else if (strcmp(key, "compression") == 0) {
            if (strcmp(value, "zstd") != 0) {
                free(bytes);
                return scribe_set_error(SCRIBE_ECONFIG, "unsupported compression '%s'", value);
            }
            seen |= 1u << 2;
        } else if (strcmp(key, "compression_level") == 0) {
            if ((err = parse_int(value, &cfg->compression_level)) != SCRIBE_OK) {
                free(bytes);
                return err;
            }
            seen |= 1u << 3;
        } else if (strcmp(key, "worker_threads") == 0) {
            if ((err = parse_int(value, &cfg->worker_threads)) != SCRIBE_OK) {
                free(bytes);
                return err;
            }
            seen |= 1u << 4;
        } else if (strcmp(key, "event_queue_capacity") == 0) {
            if ((err = parse_size(value, &cfg->event_queue_capacity)) != SCRIBE_OK) {
                free(bytes);
                return err;
            }
            seen |= 1u << 5;
        } else if (strcmp(key, "queue_stall_warn_seconds") == 0) {
            if ((err = parse_int(value, &cfg->queue_stall_warn_seconds)) != SCRIBE_OK) {
                free(bytes);
                return err;
            }
            seen |= 1u << 6;
        } else if (strcmp(key, "adapter.name") == 0) {
            if (strcmp(value, "mongodb") != 0) {
                free(bytes);
                return scribe_set_error(SCRIBE_ECONFIG, "unsupported adapter '%s'", value);
            }
            seen |= 1u << 7;
        } else if (strcmp(key, "adapter.mongodb.excluded_databases") == 0) {
            if (strlen(value) >= sizeof(cfg->adapter_excluded_databases)) {
                free(bytes);
                return scribe_set_error(SCRIBE_ECONFIG, "excluded database list too long");
            }
            strcpy(cfg->adapter_excluded_databases, value);
            seen |= 1u << 8;
        } else if (strcmp(key, "adapter.mongodb.require_pre_post_images") == 0) {
            if (strcmp(value, "true") == 0) {
                cfg->adapter_require_pre_post_images = true;
            } else if (strcmp(value, "false") == 0) {
                cfg->adapter_require_pre_post_images = false;
            } else {
                free(bytes);
                return scribe_set_error(SCRIBE_ECONFIG, "invalid boolean '%s'", value);
            }
            seen |= 1u << 9;
        } else if (strcmp(key, "adapter.mongodb.coalesce_window_ms") == 0) {
            if ((err = parse_int(value, &cfg->adapter_coalesce_window_ms)) != SCRIBE_OK) {
                free(bytes);
                return err;
            }
            seen |= 1u << 10;
        } else {
            free(bytes);
            return scribe_set_error(SCRIBE_ECONFIG, "unknown config key '%s'", key);
        }
    }
    free(bytes);
    if ((seen & ((1u << 11) - 1u)) != ((1u << 11) - 1u)) {
        return scribe_set_error(SCRIBE_ECONFIG, "config missing required v1 keys");
    }
    if (cfg->scribe_format_version != 1) {
        return scribe_set_error(SCRIBE_ECONFIG, "unsupported repository format %d", cfg->scribe_format_version);
    }
    return SCRIBE_OK;
}
