#include "core/internal.h"

#include "util/error.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

char *scribe_path_join(const char *a, const char *b) {
    size_t alen;
    size_t blen;
    char *out;
    int need_slash;

    if (a == NULL || b == NULL) {
        (void)scribe_set_error(SCRIBE_EINVAL, "path component is NULL");
        return NULL;
    }
    alen = strlen(a);
    blen = strlen(b);
    need_slash = alen > 0 && a[alen - 1u] != '/';
    out = (char *)malloc(alen + (need_slash ? 1u : 0u) + blen + 1u);
    if (out == NULL) {
        (void)scribe_set_error(SCRIBE_ENOMEM, "failed to allocate path");
        return NULL;
    }
    memcpy(out, a, alen);
    if (need_slash) {
        out[alen++] = '/';
    }
    memcpy(out + alen, b, blen);
    out[alen + blen] = '\0';
    return out;
}

static scribe_error_t fsync_parent_dir(const char *path) {
    char *copy;
    char *slash;
    int fd;

    copy = strdup(path);
    if (copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate parent path");
    }
    slash = strrchr(copy, '/');
    if (slash == NULL) {
        strcpy(copy, ".");
    } else if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    fd = open(copy, O_RDONLY | O_DIRECTORY);
    free(copy);
    if (fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to open parent directory for fsync");
    }
    if (fsync(fd) != 0) {
        close(fd);
        return scribe_set_error(SCRIBE_EIO, "failed to fsync parent directory");
    }
    close(fd);
    return SCRIBE_OK;
}

scribe_error_t scribe_mkdir_p(const char *path) {
    char *copy;
    char *p;

    if (path == NULL || path[0] == '\0') {
        return scribe_set_error(SCRIBE_EPATH, "empty directory path");
    }
    copy = strdup(path);
    if (copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate path");
    }
    for (p = copy + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
                char failed[PATH_MAX];
                snprintf(failed, sizeof(failed), "%s", copy);
                free(copy);
                return scribe_set_error(SCRIBE_EIO, "failed to create directory '%s'", failed);
            }
            *p = '/';
        }
    }
    if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
        free(copy);
        return scribe_set_error(SCRIBE_EIO, "failed to create directory '%s'", path);
    }
    free(copy);
    return SCRIBE_OK;
}

scribe_error_t scribe_write_file_atomic(const char *path, const uint8_t *bytes, size_t len) {
    char tmp[PATH_MAX];
    int fd;
    size_t off = 0;

    if (path == NULL || bytes == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid file write");
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) {
        return scribe_set_error(SCRIBE_EPATH, "temporary path too long");
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to create temporary file '%s'", tmp);
    }
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);
        if (n < 0) {
            close(fd);
            unlink(tmp);
            return scribe_set_error(SCRIBE_EIO, "failed to write temporary file");
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return scribe_set_error(SCRIBE_EIO, "failed to fsync temporary file");
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return scribe_set_error(SCRIBE_EIO, "failed to close temporary file");
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return scribe_set_error(SCRIBE_EIO, "failed to rename temporary file");
    }
    return fsync_parent_dir(path);
}

scribe_error_t scribe_read_file(const char *path, uint8_t **out, size_t *out_len) {
    struct stat st;
    FILE *f;
    uint8_t *buf;
    size_t nread;

    if (path == NULL || out == NULL || out_len == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid file read");
    }
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? scribe_set_error(SCRIBE_ENOT_FOUND, "file not found '%s'", path)
                               : scribe_set_error(SCRIBE_EIO, "failed to stat '%s'", path);
    }
    if (st.st_size < 0) {
        return scribe_set_error(SCRIBE_EIO, "invalid file size");
    }
    buf = (uint8_t *)malloc((size_t)st.st_size + 1u);
    if (buf == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate file buffer");
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        free(buf);
        return scribe_set_error(SCRIBE_EIO, "failed to open '%s'", path);
    }
    nread = fread(buf, 1, (size_t)st.st_size, f);
    if (nread != (size_t)st.st_size || ferror(f)) {
        fclose(f);
        free(buf);
        return scribe_set_error(SCRIBE_EIO, "failed to read '%s'", path);
    }
    fclose(f);
    buf[nread] = 0;
    *out = buf;
    *out_len = nread;
    return SCRIBE_OK;
}

bool scribe_file_exists(const char *path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

scribe_error_t scribe_list_dir(const char *path, scribe_error_t (*visit)(const char *name, void *ctx), void *ctx) {
    DIR *dir;
    struct dirent *ent;

    dir = opendir(path);
    if (dir == NULL) {
        return errno == ENOENT ? scribe_set_error(SCRIBE_ENOT_FOUND, "directory not found '%s'", path)
                               : scribe_set_error(SCRIBE_EIO, "failed to open directory '%s'", path);
    }
    while ((ent = readdir(dir)) != NULL) {
        scribe_error_t err;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        err = visit(ent->d_name, ctx);
        if (err != SCRIBE_OK) {
            closedir(dir);
            return err;
        }
    }
    closedir(dir);
    return SCRIBE_OK;
}
