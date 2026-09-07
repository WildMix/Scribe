/*
 * Remote synchronization commands.
 *
 * M6 uses the existing bundle format as the network payload. That keeps object
 * transfer storage-level and self-checking: receivers import loose objects,
 * packs, indexes, and refs exactly as bundle import does, but with an optional
 * fast-forward policy for visible refs. The HTTP layer here is deliberately
 * small and blocking; it is an operator tool, not the future daemon.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"
#include "util/log.h"

#ifdef SCRIBE_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *url;
    char *token;
} remote_config;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} byte_buf;

typedef struct {
    uint8_t (*items)[SCRIBE_HASH_SIZE];
    size_t count;
    size_t cap;
    int sorted;
} sync_hash_list;

typedef struct {
    char *host;
    char *port;
    char *base_path;
    int tls;
} parsed_url;

typedef struct {
    int status;
    char status_text[64];
    char content_type[96];
    uint8_t *body;
    size_t body_len;
} serve_response;

typedef struct {
    scribe_ctx *ctx;
    byte_buf refs;
} refs_text_state;

#define SCRIBE_INGEST_MAX_BODY (16u * 1024u * 1024u)
#define SCRIBE_INGEST_STREAM_MAX_BODY (128u * 1024u * 1024u)

static volatile sig_atomic_t g_serve_stop = 0;
static int g_last_response_status = 0;
static size_t g_last_response_bytes = 0;

static int sync_hash_compare(const void *a, const void *b) { return memcmp(a, b, SCRIBE_HASH_SIZE); }

static scribe_error_t sync_hash_list_add(sync_hash_list *list, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    uint8_t(*grown)[SCRIBE_HASH_SIZE];
    size_t new_cap;

    if (list->count == list->cap) {
        new_cap = list->cap == 0u ? 1024u : list->cap * 2u;
        grown = (uint8_t(*)[SCRIBE_HASH_SIZE])realloc(list->items, sizeof(*list->items) * new_cap);
        if (grown == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow sync hash inventory");
        }
        list->items = grown;
        list->cap = new_cap;
    }
    memcpy(list->items[list->count], hash, SCRIBE_HASH_SIZE);
    list->count++;
    list->sorted = 0;
    return SCRIBE_OK;
}

static void sync_hash_list_sort_unique(sync_hash_list *list) {
    size_t read_i;
    size_t write_i;

    if (list->count == 0u) {
        list->sorted = 1;
        return;
    }
    qsort(list->items, list->count, sizeof(*list->items), sync_hash_compare);
    write_i = 1u;
    for (read_i = 1u; read_i < list->count; read_i++) {
        if (memcmp(list->items[write_i - 1u], list->items[read_i], SCRIBE_HASH_SIZE) != 0) {
            if (write_i != read_i) {
                memcpy(list->items[write_i], list->items[read_i], SCRIBE_HASH_SIZE);
            }
            write_i++;
        }
    }
    list->count = write_i;
    list->sorted = 1;
}

static int sync_hash_list_contains(const sync_hash_list *list, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t lo = 0;
    size_t hi = list->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = memcmp(list->items[mid], hash, SCRIBE_HASH_SIZE);

        if (cmp == 0) {
            return 1;
        }
        if (cmp < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static void sync_hash_list_destroy(sync_hash_list *list) {
    if (list != NULL) {
        free(list->items);
        memset(list, 0, sizeof(*list));
    }
}

/*
 * Appends arbitrary bytes to a growable heap buffer. The buffer always carries
 * one spare NUL byte so callers that build textual HTTP bodies can use it as a
 * C string after appending.
 */
static scribe_error_t buf_append(byte_buf *buf, const void *data, size_t len) {
    uint8_t *grown;
    size_t new_cap;

    if (len == 0u) {
        return SCRIBE_OK;
    }
    if (buf->len > SIZE_MAX - len - 1u) {
        return scribe_set_error(SCRIBE_ENOMEM, "sync buffer is too large");
    }
    if (buf->len + len + 1u <= buf->cap) {
        memcpy(buf->data + buf->len, data, len);
        buf->len += len;
        buf->data[buf->len] = '\0';
        return SCRIBE_OK;
    }
    new_cap = buf->cap == 0u ? 4096u : buf->cap;
    while (new_cap < buf->len + len + 1u) {
        if (new_cap > SIZE_MAX / 2u) {
            return scribe_set_error(SCRIBE_ENOMEM, "sync buffer is too large");
        }
        new_cap *= 2u;
    }
    grown = (uint8_t *)realloc(buf->data, new_cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow sync buffer");
    }
    buf->data = grown;
    buf->cap = new_cap;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return SCRIBE_OK;
}

/*
 * printf-style append into a byte buffer. This is used for small text bodies
 * such as refs and status responses.
 */
static scribe_error_t buf_appendf(byte_buf *buf, const char *fmt, ...) {
    char stack[512];
    char *heap = NULL;
    va_list ap;
    va_list ap2;
    int n;
    scribe_error_t err;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return scribe_set_error(SCRIBE_EIO, "failed to format sync text");
    }
    if ((size_t)n < sizeof(stack)) {
        va_end(ap2);
        return buf_append(buf, stack, (size_t)n);
    }
    heap = (char *)malloc((size_t)n + 1u);
    if (heap == NULL) {
        va_end(ap2);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate sync text");
    }
    (void)vsnprintf(heap, (size_t)n + 1u, fmt, ap2);
    va_end(ap2);
    err = buf_append(buf, heap, (size_t)n);
    free(heap);
    return err;
}

static void buf_destroy(byte_buf *buf) {
    if (buf != NULL) {
        free(buf->data);
        memset(buf, 0, sizeof(*buf));
    }
}

static scribe_error_t inventory_visit_object(const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    return sync_hash_list_add((sync_hash_list *)user, hash);
}

static scribe_error_t object_inventory_text(scribe_ctx *ctx, byte_buf *out, size_t *out_count) {
    sync_hash_list hashes = {0};
    size_t i;
    scribe_error_t err;

    err = scribe_object_iter(ctx, inventory_visit_object, &hashes);
    if (err == SCRIBE_OK) {
        sync_hash_list_sort_unique(&hashes);
        for (i = 0; err == SCRIBE_OK && i < hashes.count; i++) {
            char hex[SCRIBE_HEX_HASH_SIZE + 1];

            scribe_hash_to_hex(hashes.items[i], hex);
            err = buf_appendf(out, "%s\n", hex);
        }
    }
    if (err == SCRIBE_OK && out_count != NULL) {
        *out_count = hashes.count;
    }
    sync_hash_list_destroy(&hashes);
    return err;
}

static scribe_error_t parse_inventory_text(const uint8_t *bytes, size_t len, sync_hash_list *out) {
    size_t pos = 0;
    scribe_error_t err = SCRIBE_OK;

    memset(out, 0, sizeof(*out));
    while (pos < len && err == SCRIBE_OK) {
        size_t start = pos;
        size_t line_len;
        char hex[SCRIBE_HEX_HASH_SIZE + 1];
        uint8_t hash[SCRIBE_HASH_SIZE];

        while (pos < len && bytes[pos] != '\n') {
            pos++;
        }
        line_len = pos - start;
        if (pos < len && bytes[pos] == '\n') {
            pos++;
        }
        if (line_len == 0u) {
            continue;
        }
        if (line_len != SCRIBE_HEX_HASH_SIZE) {
            err = scribe_set_error(SCRIBE_ECORRUPT, "malformed sync inventory line");
            break;
        }
        memcpy(hex, bytes + start, SCRIBE_HEX_HASH_SIZE);
        hex[SCRIBE_HEX_HASH_SIZE] = '\0';
        err = scribe_hash_from_hex(hex, hash);
        if (err == SCRIBE_OK) {
            err = sync_hash_list_add(out, hash);
        }
    }
    if (err == SCRIBE_OK) {
        sync_hash_list_sort_unique(out);
    } else {
        sync_hash_list_destroy(out);
    }
    return err;
}

/*
 * Validates the short remote name used as a filename under `.scribe/remotes`.
 * Keep this tighter than ref names: remote names are labels, not paths.
 */
static int remote_name_is_valid(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_' || ch == '.')) {
            return 0;
        }
    }
    return 1;
}

static char *remote_path(scribe_ctx *ctx, const char *name) {
    char *dir;
    char *path;

    dir = scribe_path_join(ctx->repo_path, "remotes");
    if (dir == NULL) {
        return NULL;
    }
    path = scribe_path_join(dir, name);
    free(dir);
    return path;
}

static scribe_error_t ensure_remotes_dir(scribe_ctx *ctx) {
    char *dir = scribe_path_join(ctx->repo_path, "remotes");
    scribe_error_t err;

    if (dir == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_mkdir_p(dir);
    free(dir);
    return err;
}

static void remote_config_destroy(remote_config *cfg) {
    if (cfg != NULL) {
        free(cfg->url);
        free(cfg->token);
        memset(cfg, 0, sizeof(*cfg));
    }
}

/*
 * Reads one remote file. The format is intentionally tiny:
 *   url <url>
 *   token <token-or-empty>
 */
static scribe_error_t remote_read(scribe_ctx *ctx, const char *name, remote_config *out) {
    char *path;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char *text;
    char *line;
    char *save = NULL;
    scribe_error_t err;

    if (!remote_name_is_valid(name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid remote name '%s'", name == NULL ? "" : name);
    }
    memset(out, 0, sizeof(*out));
    path = remote_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &bytes, &len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    text = (char *)bytes;
    for (line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "url ", 4u) == 0) {
            out->url = strdup(line + 4);
        } else if (strncmp(line, "token ", 6u) == 0) {
            out->token = strdup(line + 6);
        } else {
            free(bytes);
            remote_config_destroy(out);
            return scribe_set_error(SCRIBE_ECORRUPT, "malformed remote '%s'", name);
        }
        if ((strncmp(line, "url ", 4u) == 0 && out->url == NULL) ||
            (strncmp(line, "token ", 6u) == 0 && out->token == NULL)) {
            free(bytes);
            remote_config_destroy(out);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate remote config");
        }
    }
    free(bytes);
    if (out->url == NULL || out->url[0] == '\0') {
        remote_config_destroy(out);
        return scribe_set_error(SCRIBE_ECORRUPT, "remote '%s' has no url", name);
    }
    if (out->token == NULL) {
        out->token = strdup("");
        if (out->token == NULL) {
            remote_config_destroy(out);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate remote token");
        }
    }
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_remote_add(scribe_ctx *ctx, const char *name, const char *url, const char *token) {
    char *path;
    byte_buf body = {0};
    scribe_error_t err;

    if (!remote_name_is_valid(name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid remote name '%s'", name == NULL ? "" : name);
    }
    if (url == NULL || url[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "remote url is required");
    }
    err = ensure_remotes_dir(ctx);
    if (err != SCRIBE_OK) {
        return err;
    }
    path = remote_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = buf_appendf(&body, "url %s\ntoken %s\n", url, token == NULL ? "" : token);
    if (err == SCRIBE_OK) {
        err = scribe_write_file_atomic(path, body.data, body.len);
    }
    if (err == SCRIBE_OK) {
        printf("remote %s %s\n", name, url);
    }
    buf_destroy(&body);
    free(path);
    return err;
}

static scribe_error_t print_remote_name(const char *name, void *vctx) {
    scribe_ctx *ctx = (scribe_ctx *)vctx;
    remote_config cfg;
    scribe_error_t err;

    if (name[0] == '.') {
        return SCRIBE_OK;
    }
    err = remote_read(ctx, name, &cfg);
    if (err != SCRIBE_OK) {
        return err;
    }
    printf("%s %s\n", name, cfg.url);
    remote_config_destroy(&cfg);
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_remote_list(scribe_ctx *ctx) {
    char *dir = scribe_path_join(ctx->repo_path, "remotes");
    scribe_error_t err;

    if (dir == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (!scribe_file_exists(dir)) {
        free(dir);
        return SCRIBE_OK;
    }
    err = scribe_list_dir(dir, print_remote_name, ctx);
    free(dir);
    return err;
}

scribe_error_t scribe_cli_remote_get_url(scribe_ctx *ctx, const char *name, char **out_url) {
    remote_config cfg;
    scribe_error_t err;

    if (out_url == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid remote url output");
    }
    *out_url = NULL;
    err = remote_read(ctx, name, &cfg);
    if (err != SCRIBE_OK) {
        return err;
    }
    *out_url = strdup(cfg.url);
    remote_config_destroy(&cfg);
    if (*out_url == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate remote url");
    }
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_remote_remove(scribe_ctx *ctx, const char *name) {
    char *path;

    if (!remote_name_is_valid(name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid remote name '%s'", name == NULL ? "" : name);
    }
    path = remote_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (unlink(path) != 0) {
        free(path);
        return errno == ENOENT ? scribe_set_error(SCRIBE_ENOT_FOUND, "remote '%s' not found", name)
                               : scribe_set_error(SCRIBE_EIO, "failed to remove remote '%s'", name);
    }
    free(path);
    printf("removed remote %s\n", name);
    return SCRIBE_OK;
}

static void parsed_url_destroy(parsed_url *url) {
    if (url != NULL) {
        free(url->host);
        free(url->port);
        free(url->base_path);
        memset(url, 0, sizeof(*url));
    }
}

static scribe_error_t parse_http_url(const char *text, parsed_url *out) {
    const char *p;
    const char *slash;
    const char *colon;
    const char *scheme;
    const char *default_port;
    size_t host_len;
    size_t port_len;

    memset(out, 0, sizeof(*out));
    if (strncmp(text, "https://", 8u) == 0) {
        out->tls = 1;
        scheme = text + 8;
        default_port = "443";
    } else if (strncmp(text, "http://", 7u) == 0) {
        out->tls = 0;
        scheme = text + 7;
        default_port = "80";
    } else {
        return scribe_set_error(SCRIBE_EINVAL, "remote url must start with http:// or https://");
    }
    p = scheme;
    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (slash != NULL && colon != NULL && colon > slash) {
        colon = NULL;
    }
    host_len = colon != NULL ? (size_t)(colon - p) : (slash == NULL ? strlen(p) : (size_t)(slash - p));
    if (host_len == 0u) {
        return scribe_set_error(SCRIBE_EINVAL, "remote url host is empty");
    }
    out->host = (char *)malloc(host_len + 1u);
    if (out->host == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate remote host");
    }
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    if (colon != NULL) {
        const char *port_start = colon + 1;
        const char *port_end = slash == NULL ? text + strlen(text) : slash;
        port_len = (size_t)(port_end - port_start);
        if (port_len == 0u) {
            parsed_url_destroy(out);
            return scribe_set_error(SCRIBE_EINVAL, "remote url port is empty");
        }
        out->port = (char *)malloc(port_len + 1u);
        if (out->port == NULL) {
            parsed_url_destroy(out);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate remote port");
        }
        memcpy(out->port, port_start, port_len);
        out->port[port_len] = '\0';
    } else {
        out->port = strdup(default_port);
        if (out->port == NULL) {
            parsed_url_destroy(out);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate remote port");
        }
    }
    out->base_path = strdup(slash == NULL ? "" : slash);
    if (out->base_path == NULL) {
        parsed_url_destroy(out);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate remote path");
    }
    return SCRIBE_OK;
}

static scribe_error_t connect_url(const parsed_url *url, int *out_fd) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    rc = getaddrinfo(url->host, url->port, &hints, &res);
    if (rc != 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to resolve remote host '%s': %s", url->host, gai_strerror(rc));
    }
    for (it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to connect to remote %s:%s", url->host, url->port);
    }
    *out_fd = fd;
    return SCRIBE_OK;
}

static scribe_error_t fd_write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    while (len != 0u) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return scribe_set_error(SCRIBE_EIO, "socket write failed");
        }
        if (n == 0) {
            return scribe_set_error(SCRIBE_EIO, "socket write made no progress");
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return SCRIBE_OK;
}

static scribe_error_t fd_read_all(int fd, byte_buf *out) {
    uint8_t tmp[8192];

    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return scribe_set_error(SCRIBE_EIO, "socket read failed");
        }
        if (n == 0) {
            return SCRIBE_OK;
        }
        {
            scribe_error_t err = buf_append(out, tmp, (size_t)n);
            if (err != SCRIBE_OK) {
                return err;
            }
        }
    }
}

static scribe_error_t http_path(const parsed_url *url, const char *endpoint, char **out) {
    size_t base_len = strlen(url->base_path);
    size_t endpoint_len = strlen(endpoint);
    size_t len;
    char *path;

    if (base_len == 0u || strcmp(url->base_path, "/") == 0) {
        return (*out = strdup(endpoint)) == NULL ? scribe_set_error(SCRIBE_ENOMEM, "failed to allocate http path")
                                                 : SCRIBE_OK;
    }
    len = base_len + endpoint_len + 1u;
    path = (char *)malloc(len);
    if (path == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate http path");
    }
    (void)snprintf(path, len, "%s%s", url->base_path, endpoint);
    *out = path;
    return SCRIBE_OK;
}

#ifdef SCRIBE_HAVE_TLS
#define H2_FRAME_DATA 0x0u
#define H2_FRAME_HEADERS 0x1u
#define H2_FRAME_SETTINGS 0x4u
#define H2_FRAME_GOAWAY 0x7u
#define H2_FLAG_END_STREAM 0x1u
#define H2_FLAG_ACK 0x1u
#define H2_FLAG_END_HEADERS 0x4u
#define H2_CLIENT_STREAM_ID 1

typedef struct {
    uint32_t length;
    uint8_t type;
    uint8_t flags;
    int32_t stream_id;
} h2_frame_header;

typedef struct {
    char *method;
    char *path;
    int status;
} h2_headers;

static scribe_error_t openssl_error(const char *context) {
    unsigned long code = ERR_get_error();
    char detail[256];

    if (code == 0ul) {
        return scribe_set_error(SCRIBE_EIO, "%s", context);
    }
    ERR_error_string_n(code, detail, sizeof(detail));
    return scribe_set_error(SCRIBE_EIO, "%s: %s", context, detail);
}

static scribe_error_t tls_handshake_error(SSL *ssl, const char *ca_file) {
    long verify = ssl == NULL ? X509_V_ERR_UNSPECIFIED : SSL_get_verify_result(ssl);

    if (verify != X509_V_OK) {
        const char *detail = X509_verify_cert_error_string(verify);

        if (ca_file != NULL && ca_file[0] != '\0') {
            return scribe_set_error(SCRIBE_EIO,
                                    "TLS handshake failed: certificate verify failed: %s "
                                    "(using SCRIBE_TLS_CA_FILE=%s)",
                                    detail, ca_file);
        }
        return scribe_set_error(SCRIBE_EIO,
                                "TLS handshake failed: certificate verify failed: %s "
                                "(set SCRIBE_TLS_CA_FILE=/path/to/ca.pem for private/self-signed CAs)",
                                detail);
    }
    return openssl_error("TLS handshake failed");
}

static int ssl_write_all(SSL *ssl, const uint8_t *data, size_t len) {
    const uint8_t *p = data;

    while (len != 0u) {
        int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
        int n = SSL_write(ssl, p, chunk);

        if (n <= 0) {
            int err = SSL_get_error(ssl, n);

            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return 0;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 1;
}

static int ssl_read_exact(SSL *ssl, uint8_t *data, size_t len) {
    uint8_t *p = data;

    while (len != 0u) {
        int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
        int n = SSL_read(ssl, p, chunk);

        if (n <= 0) {
            int err = SSL_get_error(ssl, n);

            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return 0;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 1;
}

static scribe_error_t h2_write_frame(SSL *ssl, uint8_t type, uint8_t flags, int32_t stream_id, const uint8_t *payload,
                                     size_t payload_len) {
    uint8_t header[9];

    if (payload_len > 0x00ffffffu) {
        return scribe_set_error(SCRIBE_EINVAL, "HTTP/2 frame payload is too large");
    }
    header[0] = (uint8_t)((payload_len >> 16) & 0xffu);
    header[1] = (uint8_t)((payload_len >> 8) & 0xffu);
    header[2] = (uint8_t)(payload_len & 0xffu);
    header[3] = type;
    header[4] = flags;
    header[5] = (uint8_t)(((uint32_t)stream_id >> 24) & 0x7fu);
    header[6] = (uint8_t)(((uint32_t)stream_id >> 16) & 0xffu);
    header[7] = (uint8_t)(((uint32_t)stream_id >> 8) & 0xffu);
    header[8] = (uint8_t)((uint32_t)stream_id & 0xffu);
    if (getenv("SCRIBE_TRACE_H2") != NULL) {
        fprintf(stderr, "h2 write frame type=%u flags=%u stream=%d length=%zu\n", type, flags, stream_id, payload_len);
    }
    if (!ssl_write_all(ssl, header, sizeof(header))) {
        return scribe_set_error(SCRIBE_EIO, "failed to write HTTP/2 frame header");
    }
    if (payload_len != 0u && !ssl_write_all(ssl, payload, payload_len)) {
        return scribe_set_error(SCRIBE_EIO, "failed to write HTTP/2 frame payload");
    }
    return SCRIBE_OK;
}

static scribe_error_t h2_read_frame_header(SSL *ssl, h2_frame_header *out) {
    uint8_t header[9];

    if (!ssl_read_exact(ssl, header, sizeof(header))) {
        return scribe_set_error(SCRIBE_EIO, "failed to read HTTP/2 frame header");
    }
    out->length = ((uint32_t)header[0] << 16) | ((uint32_t)header[1] << 8) | (uint32_t)header[2];
    out->type = header[3];
    out->flags = header[4];
    out->stream_id = (int32_t)((((uint32_t)header[5] & 0x7fu) << 24) | ((uint32_t)header[6] << 16) |
                               ((uint32_t)header[7] << 8) | (uint32_t)header[8]);
    if (getenv("SCRIBE_TRACE_H2") != NULL) {
        fprintf(stderr, "h2 read frame type=%u flags=%u stream=%d length=%u\n", out->type, out->flags, out->stream_id,
                out->length);
    }
    return SCRIBE_OK;
}

static scribe_error_t h2_read_frame_payload(SSL *ssl, const h2_frame_header *header, byte_buf *out) {
    uint8_t *tmp;
    scribe_error_t err;

    if (header->length == 0u) {
        return SCRIBE_OK;
    }
    tmp = (uint8_t *)malloc(header->length);
    if (tmp == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP/2 frame payload");
    }
    if (!ssl_read_exact(ssl, tmp, header->length)) {
        free(tmp);
        return scribe_set_error(SCRIBE_EIO, "failed to read HTTP/2 frame payload");
    }
    err = buf_append(out, tmp, header->length);
    free(tmp);
    return err;
}

static scribe_error_t hpack_encode_int(byte_buf *out, uint8_t prefix_mask, uint8_t prefix_bits, uint32_t value) {
    uint32_t max_prefix = (uint32_t)((1u << prefix_bits) - 1u);
    uint8_t byte;

    if (value < max_prefix) {
        byte = (uint8_t)(prefix_mask | value);
        return buf_append(out, &byte, 1u);
    }
    byte = (uint8_t)(prefix_mask | max_prefix);
    if (buf_append(out, &byte, 1u) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    value -= max_prefix;
    while (value >= 128u) {
        byte = (uint8_t)((value % 128u) + 128u);
        if (buf_append(out, &byte, 1u) != SCRIBE_OK) {
            return SCRIBE_ENOMEM;
        }
        value /= 128u;
    }
    byte = (uint8_t)value;
    return buf_append(out, &byte, 1u);
}

static scribe_error_t hpack_encode_string(byte_buf *out, const char *text) {
    size_t len = strlen(text);
    scribe_error_t err;

    if (len > UINT32_MAX) {
        return scribe_set_error(SCRIBE_EINVAL, "HTTP/2 header value is too large");
    }
    err = hpack_encode_int(out, 0u, 7u, (uint32_t)len);
    if (err == SCRIBE_OK) {
        err = buf_append(out, text, len);
    }
    return err;
}

static scribe_error_t hpack_encode_header(byte_buf *out, const char *name, const char *value) {
    uint8_t literal = 0u;
    scribe_error_t err = buf_append(out, &literal, 1u);

    if (err == SCRIBE_OK) {
        err = hpack_encode_string(out, name);
    }
    if (err == SCRIBE_OK) {
        err = hpack_encode_string(out, value);
    }
    return err;
}

static scribe_error_t hpack_decode_int(const uint8_t *data, size_t len, size_t *pos, uint8_t prefix_bits, uint8_t first,
                                       uint32_t *out) {
    uint32_t max_prefix = (uint32_t)((1u << prefix_bits) - 1u);
    uint32_t value = (uint32_t)(first & max_prefix);
    uint32_t shift = 0;

    if (value < max_prefix) {
        *out = value;
        return SCRIBE_OK;
    }
    for (;;) {
        uint8_t byte;

        if (*pos >= len || shift > 28u) {
            return scribe_set_error(SCRIBE_EPROTOCOL, "malformed HTTP/2 HPACK integer");
        }
        byte = data[(*pos)++];
        value += (uint32_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0u) {
            *out = value;
            return SCRIBE_OK;
        }
        shift += 7u;
    }
}

static const char *hpack_static_name(uint32_t index, const char **out_value) {
    *out_value = NULL;
    switch (index) {
    case 1:
        return ":authority";
    case 2:
        *out_value = "GET";
        return ":method";
    case 3:
        *out_value = "POST";
        return ":method";
    case 4:
        *out_value = "/";
        return ":path";
    case 5:
        *out_value = "/index.html";
        return ":path";
    case 6:
        *out_value = "http";
        return ":scheme";
    case 7:
        *out_value = "https";
        return ":scheme";
    case 8:
        *out_value = "200";
        return ":status";
    case 12:
        *out_value = "400";
        return ":status";
    case 13:
        *out_value = "404";
        return ":status";
    case 14:
        *out_value = "500";
        return ":status";
    case 23:
        return "authorization";
    case 28:
        return "content-length";
    case 31:
        return "content-type";
    default:
        return NULL;
    }
}

static scribe_error_t hpack_decode_string(const uint8_t *data, size_t len, size_t *pos, char **out) {
    uint8_t first;
    uint32_t value_len = 0;
    char *text;
    scribe_error_t err;

    *out = NULL;
    if (*pos >= len) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "malformed HTTP/2 HPACK string");
    }
    first = data[(*pos)++];
    if ((first & 0x80u) != 0u) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "HPACK Huffman strings are not supported");
    }
    err = hpack_decode_int(data, len, pos, 7u, first, &value_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    if ((size_t)value_len > len - *pos) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "truncated HTTP/2 HPACK string");
    }
    text = (char *)malloc((size_t)value_len + 1u);
    if (text == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP/2 header string");
    }
    memcpy(text, data + *pos, value_len);
    text[value_len] = '\0';
    *pos += value_len;
    *out = text;
    return SCRIBE_OK;
}

static scribe_error_t h2_headers_set(h2_headers *headers, const char *name, const char *value) {
    if (strcmp(name, ":method") == 0) {
        free(headers->method);
        headers->method = strdup(value);
        return headers->method == NULL ? scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP/2 method")
                                       : SCRIBE_OK;
    }
    if (strcmp(name, ":path") == 0) {
        free(headers->path);
        headers->path = strdup(value);
        return headers->path == NULL ? scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP/2 path") : SCRIBE_OK;
    }
    if (strcmp(name, ":status") == 0) {
        headers->status = atoi(value);
    }
    return SCRIBE_OK;
}

static void h2_headers_destroy(h2_headers *headers) {
    if (headers != NULL) {
        free(headers->method);
        free(headers->path);
        memset(headers, 0, sizeof(*headers));
    }
}

static scribe_error_t hpack_decode_headers(const uint8_t *data, size_t len, h2_headers *headers) {
    size_t pos = 0;

    while (pos < len) {
        uint8_t first = data[pos++];
        uint32_t index = 0;
        const char *name_const = NULL;
        const char *value_const = NULL;
        char *name = NULL;
        char *value = NULL;
        scribe_error_t err;

        if ((first & 0x80u) != 0u) {
            err = hpack_decode_int(data, len, &pos, 7u, first, &index);
            if (err != SCRIBE_OK) {
                return err;
            }
            name_const = hpack_static_name(index, &value_const);
            if (name_const != NULL && value_const != NULL) {
                err = h2_headers_set(headers, name_const, value_const);
                if (err != SCRIBE_OK) {
                    return err;
                }
            }
            continue;
        }
        if ((first & 0x40u) != 0u) {
            err = hpack_decode_int(data, len, &pos, 6u, first, &index);
        } else {
            err = hpack_decode_int(data, len, &pos, 4u, first, &index);
        }
        if (err != SCRIBE_OK) {
            return err;
        }
        if (index == 0u) {
            err = hpack_decode_string(data, len, &pos, &name);
            if (err != SCRIBE_OK) {
                return err;
            }
            name_const = name;
        } else {
            name_const = hpack_static_name(index, &value_const);
            if (name_const == NULL) {
                free(name);
                return scribe_set_error(SCRIBE_EPROTOCOL, "unsupported HTTP/2 HPACK static header index");
            }
        }
        err = hpack_decode_string(data, len, &pos, &value);
        if (err == SCRIBE_OK) {
            err = h2_headers_set(headers, name_const, value);
        }
        free(name);
        free(value);
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    return SCRIBE_OK;
}

static scribe_error_t tls_client_connect(const parsed_url *url, int fd, SSL_CTX **out_ctx, SSL **out_ssl) {
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    const unsigned char alpn[] = {2u, 'h', '2'};
    const unsigned char *selected = NULL;
    unsigned int selected_len = 0;
    const char *ca_file = getenv("SCRIBE_TLS_CA_FILE");
    scribe_error_t err = SCRIBE_OK;

    *out_ctx = NULL;
    *out_ssl = NULL;
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (ssl_ctx == NULL) {
        return openssl_error("failed to create TLS client context");
    }
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    if (ca_file != NULL && ca_file[0] != '\0') {
        if (SSL_CTX_load_verify_locations(ssl_ctx, ca_file, NULL) != 1) {
            err = openssl_error("failed to load TLS CA file");
        }
    } else if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
        err = openssl_error("failed to load system TLS trust store");
    }
    if (err == SCRIBE_OK) {
        ssl = SSL_new(ssl_ctx);
        if (ssl == NULL) {
            err = openssl_error("failed to create TLS connection");
        }
    }
    if (err == SCRIBE_OK && SSL_set_fd(ssl, fd) != 1) {
        err = openssl_error("failed to attach TLS connection to socket");
    }
    if (err == SCRIBE_OK && SSL_set_tlsext_host_name(ssl, url->host) != 1) {
        err = openssl_error("failed to set TLS SNI host");
    }
    if (err == SCRIBE_OK && SSL_set1_host(ssl, url->host) != 1) {
        err = openssl_error("failed to set TLS verification host");
    }
    if (err == SCRIBE_OK && SSL_set_alpn_protos(ssl, alpn, sizeof(alpn)) != 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to configure TLS ALPN for HTTP/2");
    }
    if (err == SCRIBE_OK && SSL_connect(ssl) != 1) {
        err = tls_handshake_error(ssl, ca_file);
    }
    if (err == SCRIBE_OK) {
        SSL_get0_alpn_selected(ssl, &selected, &selected_len);
        if (selected_len != 2u || memcmp(selected, "h2", 2u) != 0) {
            err = scribe_set_error(SCRIBE_EPROTOCOL, "TLS server did not negotiate HTTP/2");
        }
    }
    if (err == SCRIBE_OK) {
        *out_ctx = ssl_ctx;
        *out_ssl = ssl;
        return SCRIBE_OK;
    }
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    return err;
}

static scribe_error_t h2_build_request_headers(const parsed_url *url, const char *path, const char *token,
                                               const char *method, size_t body_len, byte_buf *out) {
    char authority[512];
    char content_length[64];
    char authorization[512];
    int n = snprintf(authority, sizeof(authority), "%s:%s", url->host, url->port);
    scribe_error_t err;

    if (n < 0 || (size_t)n >= sizeof(authority)) {
        return scribe_set_error(SCRIBE_EINVAL, "remote authority is too long");
    }
    err = hpack_encode_header(out, ":method", method);
    if (err == SCRIBE_OK) {
        err = hpack_encode_header(out, ":scheme", "https");
    }
    if (err == SCRIBE_OK) {
        err = hpack_encode_header(out, ":authority", authority);
    }
    if (err == SCRIBE_OK) {
        err = hpack_encode_header(out, ":path", path);
    }
    if (err == SCRIBE_OK && body_len != 0u) {
        n = snprintf(content_length, sizeof(content_length), "%zu", body_len);
        if (n < 0 || (size_t)n >= sizeof(content_length)) {
            err = scribe_set_error(SCRIBE_EIO, "failed to format HTTP/2 content length");
        } else {
            err = hpack_encode_header(out, "content-length", content_length);
        }
    }
    if (err == SCRIBE_OK && token != NULL && token[0] != '\0') {
        n = snprintf(authorization, sizeof(authorization), "Bearer %s", token);
        if (n < 0 || (size_t)n >= sizeof(authorization)) {
            err = scribe_set_error(SCRIBE_EINVAL, "remote bearer token is too long");
        } else {
            err = hpack_encode_header(out, "authorization", authorization);
        }
    }
    return err;
}

static scribe_error_t h2_tls_request(const parsed_url *url, const char *path, const char *token, const char *method,
                                     const uint8_t *body, size_t body_len, int fd, int *out_status, uint8_t **out_body,
                                     size_t *out_body_len) {
    static const uint8_t client_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    byte_buf headers = {0};
    byte_buf payload = {0};
    h2_headers decoded = {0};
    size_t off = 0;
    scribe_error_t err = tls_client_connect(url, fd, &ssl_ctx, &ssl);

    (void)signal(SIGPIPE, SIG_IGN);
    if (err == SCRIBE_OK && !ssl_write_all(ssl, client_preface, sizeof(client_preface) - 1u)) {
        err = scribe_set_error(SCRIBE_EIO, "failed to write HTTP/2 client preface");
    }
    if (err == SCRIBE_OK) {
        err = h2_write_frame(ssl, H2_FRAME_SETTINGS, 0u, 0, NULL, 0u);
    }
    if (err == SCRIBE_OK) {
        err = h2_build_request_headers(url, path, token, method, body_len, &headers);
    }
    if (err == SCRIBE_OK) {
        err = h2_write_frame(ssl, H2_FRAME_HEADERS,
                             (uint8_t)(H2_FLAG_END_HEADERS | (body_len == 0u ? H2_FLAG_END_STREAM : 0u)),
                             H2_CLIENT_STREAM_ID, headers.data, headers.len);
    }
    while (err == SCRIBE_OK && off < body_len) {
        size_t chunk = body_len - off;
        uint8_t flags;

        if (chunk > 16384u) {
            chunk = 16384u;
        }
        flags = off + chunk == body_len ? H2_FLAG_END_STREAM : 0u;
        err = h2_write_frame(ssl, H2_FRAME_DATA, flags, H2_CLIENT_STREAM_ID, body + off, chunk);
        off += chunk;
    }
    while (err == SCRIBE_OK) {
        h2_frame_header frame;
        byte_buf frame_payload = {0};

        err = h2_read_frame_header(ssl, &frame);
        if (err != SCRIBE_OK) {
            break;
        }
        err = h2_read_frame_payload(ssl, &frame, &frame_payload);
        if (err != SCRIBE_OK) {
            buf_destroy(&frame_payload);
            break;
        }
        if (frame.type == H2_FRAME_SETTINGS && (frame.flags & H2_FLAG_ACK) == 0u) {
            err = h2_write_frame(ssl, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0u);
        } else if (frame.stream_id == H2_CLIENT_STREAM_ID && frame.type == H2_FRAME_HEADERS) {
            err = hpack_decode_headers(frame_payload.data, frame_payload.len, &decoded);
        } else if (frame.stream_id == H2_CLIENT_STREAM_ID && frame.type == H2_FRAME_DATA) {
            err = buf_append(&payload, frame_payload.data, frame_payload.len);
        } else if (frame.type == H2_FRAME_GOAWAY) {
            err = scribe_set_error(SCRIBE_EIO, "HTTP/2 server closed the connection");
        }
        if (err == SCRIBE_OK && frame.stream_id == H2_CLIENT_STREAM_ID && (frame.flags & H2_FLAG_END_STREAM) != 0u) {
            buf_destroy(&frame_payload);
            break;
        }
        buf_destroy(&frame_payload);
    }
    if (err == SCRIBE_OK && decoded.status == 0) {
        err = scribe_set_error(SCRIBE_EIO, "HTTP/2 response missing status");
    }
    if (err == SCRIBE_OK) {
        uint8_t *copy = (uint8_t *)malloc(payload.len + 1u);

        if (copy == NULL) {
            err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP/2 response body");
        } else {
            memcpy(copy, payload.data, payload.len);
            copy[payload.len] = '\0';
            *out_status = decoded.status;
            *out_body = copy;
            *out_body_len = payload.len;
        }
    }
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    h2_headers_destroy(&decoded);
    buf_destroy(&headers);
    buf_destroy(&payload);
    return err;
}
#endif

static scribe_error_t http_request(const char *remote_url, const char *token, const char *method, const char *endpoint,
                                   const uint8_t *body, size_t body_len, int *out_status, uint8_t **out_body,
                                   size_t *out_body_len) {
    parsed_url url;
    char *path = NULL;
    byte_buf req = {0};
    byte_buf resp = {0};
    uint8_t *sep;
    int fd = -1;
    int status = 0;
    scribe_error_t err;

    *out_body = NULL;
    *out_body_len = 0;
    *out_status = 0;
    err = parse_http_url(remote_url, &url);
    if (err == SCRIBE_OK) {
        err = http_path(&url, endpoint, &path);
    }
    if (err == SCRIBE_OK) {
        err = connect_url(&url, &fd);
    }
    if (err == SCRIBE_OK && url.tls) {
#ifdef SCRIBE_HAVE_TLS
        err = h2_tls_request(&url, path, token, method, body, body_len, fd, out_status, out_body, out_body_len);
#else
        err = scribe_set_error(SCRIBE_ENOSYS, "TLS/HTTP2 transport is not enabled in this build");
#endif
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        buf_destroy(&req);
        buf_destroy(&resp);
        parsed_url_destroy(&url);
        free(path);
        return err;
    }
    if (err == SCRIBE_OK) {
        err = buf_appendf(&req, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %zu\r\n", method,
                          path, url.host, body_len);
    }
    if (err == SCRIBE_OK && token != NULL && token[0] != '\0') {
        err = buf_appendf(&req, "Authorization: Bearer %s\r\n", token);
    }
    if (err == SCRIBE_OK) {
        err = buf_append(&req, "\r\n", 2u);
    }
    if (err == SCRIBE_OK && body_len != 0u) {
        err = buf_append(&req, body, body_len);
    }
    if (err == SCRIBE_OK) {
        err = fd_write_all(fd, req.data, req.len);
    }
    if (err == SCRIBE_OK) {
        err = fd_read_all(fd, &resp);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (err == SCRIBE_OK) {
        if (sscanf((const char *)resp.data, "HTTP/1.1 %d", &status) != 1 &&
            sscanf((const char *)resp.data, "HTTP/1.0 %d", &status) != 1) {
            err = scribe_set_error(SCRIBE_EIO, "malformed HTTP response");
        }
    }
    if (err == SCRIBE_OK) {
        sep = (uint8_t *)strstr((const char *)resp.data, "\r\n\r\n");
        if (sep == NULL) {
            err = scribe_set_error(SCRIBE_EIO, "HTTP response missing body separator");
        } else {
            size_t header_len = (size_t)(sep + 4u - resp.data);
            size_t payload_len = resp.len - header_len;
            uint8_t *payload = (uint8_t *)malloc(payload_len + 1u);

            if (payload == NULL) {
                err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP response body");
            } else {
                memcpy(payload, resp.data + header_len, payload_len);
                payload[payload_len] = '\0';
                *out_status = status;
                *out_body = payload;
                *out_body_len = payload_len;
            }
        }
    }
    buf_destroy(&req);
    buf_destroy(&resp);
    parsed_url_destroy(&url);
    free(path);
    return err;
}

static scribe_error_t base64_encode_bytes(const uint8_t *data, size_t len, char **out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len;
    char *encoded;
    size_t i = 0u;
    size_t o = 0u;

    if (out == NULL || (data == NULL && len != 0u)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid base64 input");
    }
    if (len > (SIZE_MAX / 4u) * 3u) {
        return scribe_set_error(SCRIBE_ENOMEM, "base64 input is too large");
    }
    out_len = ((len + 2u) / 3u) * 4u;
    encoded = (char *)malloc(out_len + 1u);
    if (encoded == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate base64 output");
    }
    while (i < len) {
        size_t start = i;
        uint32_t a = data[i++];
        uint32_t b = i < len ? data[i++] : 0u;
        uint32_t c = i < len ? data[i++] : 0u;
        uint32_t triple = (a << 16u) | (b << 8u) | c;
        size_t remaining = len - start;

        encoded[o++] = alphabet[(triple >> 18u) & 0x3fu];
        encoded[o++] = alphabet[(triple >> 12u) & 0x3fu];
        encoded[o++] = remaining > 1u ? alphabet[(triple >> 6u) & 0x3fu] : '=';
        encoded[o++] = remaining > 2u ? alphabet[triple & 0x3fu] : '=';
    }
    encoded[o] = '\0';
    *out = encoded;
    return SCRIBE_OK;
}

static int url_is_unreserved(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
}

static scribe_error_t url_encode_component(const char *text, char **out) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    size_t len = 0;
    char *encoded;
    char *p;

    if (text == NULL) {
        text = "";
    }
    for (i = 0; text[i] != '\0'; i++) {
        len += url_is_unreserved((unsigned char)text[i]) ? 1u : 3u;
    }
    encoded = (char *)malloc(len + 1u);
    if (encoded == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate encoded URL component");
    }
    p = encoded;
    for (i = 0; text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];

        if (url_is_unreserved(ch)) {
            *p++ = (char)ch;
        } else {
            *p++ = '%';
            *p++ = hex[ch >> 4u];
            *p++ = hex[ch & 0x0fu];
        }
    }
    *p = '\0';
    *out = encoded;
    return SCRIBE_OK;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static scribe_error_t url_decode_component(const char *text, size_t len, char **out) {
    char *decoded = (char *)malloc(len + 1u);
    size_t r = 0;
    size_t w = 0;

    if (decoded == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate decoded URL component");
    }
    while (r < len) {
        if (text[r] == '%') {
            int hi;
            int lo;

            if (r + 2u >= len) {
                free(decoded);
                return scribe_set_error(SCRIBE_EPROTOCOL, "malformed percent-encoded URL component");
            }
            hi = hex_value(text[r + 1u]);
            lo = hex_value(text[r + 2u]);
            if (hi < 0 || lo < 0) {
                free(decoded);
                return scribe_set_error(SCRIBE_EPROTOCOL, "malformed percent-encoded URL component");
            }
            decoded[w++] = (char)((hi << 4) | lo);
            r += 3u;
        } else {
            decoded[w++] = text[r++];
        }
    }
    decoded[w] = '\0';
    *out = decoded;
    return SCRIBE_OK;
}

static scribe_error_t temp_file_path(scribe_ctx *ctx, const char *prefix, char **out_path) {
    char tmpl[4096];
    int fd;
    int n;

    n = snprintf(tmpl, sizeof(tmpl), "%s/%s-XXXXXX", ctx->repo_path, prefix);
    if (n < 0 || (size_t)n >= sizeof(tmpl)) {
        return scribe_set_error(SCRIBE_EPATH, "temporary path is too long");
    }
    fd = mkstemp(tmpl);
    if (fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to create temporary file");
    }
    close(fd);
    *out_path = strdup(tmpl);
    if (*out_path == NULL) {
        unlink(tmpl);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate temporary path");
    }
    return SCRIBE_OK;
}

static int missing_from_inventory_filter(const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    return !sync_hash_list_contains((const sync_hash_list *)user, hash);
}

static scribe_error_t create_temp_filtered_bundle(scribe_ctx *ctx, const sync_hash_list *receiver_inventory,
                                                  char **out_path, size_t *out_storage_entries, size_t *out_refs) {
    scribe_error_t err = temp_file_path(ctx, "sync-bundle", out_path);

    if (err == SCRIBE_OK) {
        err = scribe_bundle_create_filtered_file(ctx, *out_path, missing_from_inventory_filter,
                                                 (void *)receiver_inventory, out_storage_entries, out_refs);
        if (err != SCRIBE_OK) {
            unlink(*out_path);
            free(*out_path);
            *out_path = NULL;
        }
    }
    return err;
}

static scribe_error_t write_temp_bytes(scribe_ctx *ctx, const uint8_t *bytes, size_t len, char **out_path) {
    scribe_error_t err = temp_file_path(ctx, "sync-recv", out_path);

    if (err == SCRIBE_OK) {
        err = scribe_write_file_atomic(*out_path, bytes, len);
        if (err != SCRIBE_OK) {
            unlink(*out_path);
            free(*out_path);
            *out_path = NULL;
        }
    }
    return err;
}

static scribe_error_t refs_text_visit(const char *name, const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    refs_text_state *st = (refs_text_state *)user;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];

    scribe_hash_to_hex(hash, hex);
    return buf_appendf(&st->refs, "%s %s\n", name, hex);
}

static scribe_error_t refs_text(scribe_ctx *ctx, byte_buf *out) {
    refs_text_state st;
    scribe_error_t err;

    memset(&st, 0, sizeof(st));
    st.ctx = ctx;
    err = scribe_refs_iter(ctx, refs_text_visit, &st);
    if (err == SCRIBE_OK) {
        *out = st.refs;
    } else {
        buf_destroy(&st.refs);
    }
    return err;
}

static void fsck_health_find_value(const uint8_t *bytes, size_t len, const char *key, char *out, size_t out_size) {
    size_t key_len = strlen(key);
    size_t pos = 0;

    if (out_size == 0) {
        return;
    }
    while (pos < len) {
        size_t line_start = pos;
        size_t line_len;
        size_t value_len;

        while (pos < len && bytes[pos] != '\n') {
            pos++;
        }
        line_len = pos - line_start;
        if (pos < len && bytes[pos] == '\n') {
            pos++;
        }
        if (line_len > 0 && bytes[line_start + line_len - 1u] == '\r') {
            line_len--;
        }
        if (line_len <= key_len || memcmp(bytes + line_start, key, key_len) != 0 ||
            bytes[line_start + key_len] != ' ') {
            continue;
        }
        value_len = line_len - key_len - 1u;
        if (value_len >= out_size) {
            value_len = out_size - 1u;
        }
        memcpy(out, bytes + line_start + key_len + 1u, value_len);
        out[value_len] = '\0';
        return;
    }
}

static void read_fsck_health(scribe_ctx *ctx, char *last_at, size_t last_at_size, char *last_status,
                             size_t last_status_size) {
    char *path = scribe_path_join(ctx->repo_path, "fsck-status");
    uint8_t *bytes = NULL;
    size_t len = 0;

    if (last_at_size != 0u) {
        snprintf(last_at, last_at_size, "unknown");
    }
    if (last_status_size != 0u) {
        snprintf(last_status, last_status_size, "unknown");
    }
    if (path != NULL && scribe_file_exists(path) && scribe_read_file(path, &bytes, &len) == SCRIBE_OK) {
        fsck_health_find_value(bytes, len, "last_fsck_at", last_at, last_at_size);
        fsck_health_find_value(bytes, len, "last_fsck_status", last_status, last_status_size);
    }
    free(bytes);
    free(path);
}

static void print_fsck_health(scribe_ctx *ctx) {
    char last_at[64];
    char last_status[64];

    read_fsck_health(ctx, last_at, sizeof(last_at), last_status, sizeof(last_status));
    printf("last_fsck_at %s\n", last_at);
    printf("last_fsck_status %s\n", last_status);
}

typedef struct {
    scribe_ctx *ctx;
    int trace_perf;
    scribe_hash_set unique_hashes;
    size_t storage_entries;
    size_t loose_objects;
    size_t pack_files;
    size_t pack_objects;
    size_t refs;
    size_t main_commits;
    size_t objects;
    size_t blob_objects;
    size_t tree_objects;
    size_t commit_objects;
    char main_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char last_commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char last_commit_at[64];
} status_state;

static scribe_error_t status_visit_loose_object(const uint8_t hash[SCRIBE_HASH_SIZE], const char *path, time_t mtime,
                                                size_t compressed_size, void *user) {
    status_state *st = (status_state *)user;

    (void)hash;
    (void)path;
    (void)mtime;
    (void)compressed_size;
    st->loose_objects++;
    return SCRIBE_OK;
}

static void status_count_type(status_state *st, uint8_t type) {
    if (type == SCRIBE_OBJECT_BLOB) {
        st->blob_objects++;
    } else if (type == SCRIBE_OBJECT_TREE) {
        st->tree_objects++;
    } else if (type == SCRIBE_OBJECT_COMMIT) {
        st->commit_objects++;
    }
}

static scribe_error_t status_visit_loose_object_type(const uint8_t hash[SCRIBE_HASH_SIZE], const char *path,
                                                     time_t mtime, size_t compressed_size, void *user) {
    status_state *st = (status_state *)user;
    scribe_object obj;
    int already = 0;
    scribe_error_t err;

    (void)path;
    (void)mtime;
    (void)compressed_size;
    st->storage_entries++;
    err = scribe_hash_set_add(&st->unique_hashes, hash, &already);
    if (err != SCRIBE_OK || already) {
        return err;
    }
    memset(&obj, 0, sizeof(obj));
    err = scribe_object_read(st->ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    status_count_type(st, obj.type);
    scribe_object_free(&obj);
    return SCRIBE_OK;
}

static scribe_error_t status_visit_pack_file(const uint8_t pack_id[16], void *user) {
    status_state *st = (status_state *)user;

    (void)pack_id;
    st->pack_files++;
    return SCRIBE_OK;
}

static scribe_error_t status_visit_ref(const char *name, const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    status_state *st = (status_state *)user;

    (void)name;
    (void)hash;
    st->refs++;
    return SCRIBE_OK;
}

static scribe_error_t status_visit_pack_object_type(const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t type, void *user) {
    status_state *st = (status_state *)user;
    uint64_t started = st->trace_perf ? scribe_perf_now_ns() : 0u;
    int already = 0;
    scribe_error_t err;

    st->storage_entries++;
    err = scribe_hash_set_add(&st->unique_hashes, hash, &already);
    if (err != SCRIBE_OK || already) {
        return err;
    }
    status_count_type(st, type);
    if (started != 0u) {
        uint64_t ended = scribe_perf_now_ns();
        if (ended >= started) {
            scribe_perf_trace_add_object_type_count_ns(ended - started);
        }
    }
    return SCRIBE_OK;
}

static scribe_error_t status_count_loose_unique_objects(scribe_ctx *ctx, status_state *st) {
    uint64_t started;
    scribe_error_t err;

    started = scribe_perf_now_ns();
    err = scribe_loose_object_iter(ctx, status_visit_loose_object_type, st);
    st->objects = st->unique_hashes.count;
    if (started != 0u) {
        uint64_t ended = scribe_perf_now_ns();
        if (ended >= started) {
            scribe_perf_trace_add_object_type_count_ns(ended - started);
        }
    }
    return err;
}

static scribe_error_t status_count_main_history(scribe_ctx *ctx, status_state *st) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_error_t err = scribe_refs_read(ctx, "refs/heads/main", hash);

    snprintf(st->main_hex, sizeof(st->main_hex), "unborn");
    snprintf(st->last_commit_hex, sizeof(st->last_commit_hex), "none");
    snprintf(st->last_commit_at, sizeof(st->last_commit_at), "unknown");
    if (err == SCRIBE_ENOT_FOUND) {
        return SCRIBE_OK;
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_hash_to_hex(hash, st->main_hex);
    scribe_hash_to_hex(hash, st->last_commit_hex);
    while (1) {
        scribe_arena arena;
        scribe_commit_view view;
        uint8_t next_hash[SCRIBE_HASH_SIZE];

        err = scribe_arena_init(&arena, 4096);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = scribe_read_commit_view(ctx, hash, &arena, &view);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
        if (st->main_commits == 0u) {
            scribe_format_time_utc(view.committer_time, st->last_commit_at, sizeof(st->last_commit_at));
        }
        st->main_commits++;
        if (view.has_parent) {
            scribe_hash_copy(next_hash, view.parent);
        }
        scribe_arena_destroy(&arena);
        if (!view.has_parent) {
            break;
        }
        scribe_hash_copy(hash, next_hash);
    }
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_status(scribe_ctx *ctx) {
    status_state st;
    char last_fsck_at[64];
    char last_fsck_status[64];
    scribe_error_t err;

    if (scribe_perf_trace_enabled()) {
        scribe_perf_trace_reset();
    }
    memset(&st, 0, sizeof(st));
    st.ctx = ctx;
    st.trace_perf = scribe_perf_trace_enabled();
    err = status_count_main_history(ctx, &st);
    if (err == SCRIBE_OK) {
        err = scribe_refs_iter(ctx, status_visit_ref, &st);
    }
    if (err == SCRIBE_OK) {
        err = scribe_loose_object_iter(ctx, status_visit_loose_object, &st);
    }
    if (err == SCRIBE_OK) {
        err = scribe_pack_file_iter(ctx, status_visit_pack_file, &st);
    }
    if (err == SCRIBE_OK) {
        err = status_count_loose_unique_objects(ctx, &st);
    }
    if (err == SCRIBE_OK) {
        err = scribe_pack_validate_all_meta(ctx, &st.pack_objects, status_visit_pack_object_type, &st);
        st.objects = st.unique_hashes.count;
    }
    if (err == SCRIBE_OK) {
        read_fsck_health(ctx, last_fsck_at, sizeof(last_fsck_at), last_fsck_status, sizeof(last_fsck_status));
        printf("status ok\n");
        printf("store %s\n", ctx->repo_path);
        printf("main %s\n", st.main_hex);
        printf("commits %zu\n", st.main_commits);
        printf("last_commit %s\n", st.last_commit_hex);
        printf("last_commit_at %s\n", st.last_commit_at);
        printf("refs %zu\n", st.refs);
        printf("objects %zu\n", st.objects);
        printf("objects_blob %zu\n", st.blob_objects);
        printf("objects_tree %zu\n", st.tree_objects);
        printf("objects_commit %zu\n", st.commit_objects);
        printf("storage_entries %zu\n", st.storage_entries);
        printf("loose_objects %zu\n", st.loose_objects);
        printf("pack_files %zu\n", st.pack_files);
        printf("pack_objects %zu\n", st.pack_objects);
        printf("last_fsck_at %s\n", last_fsck_at);
        printf("last_fsck_status %s\n", last_fsck_status);
    }
    if (scribe_perf_trace_enabled()) {
        scribe_perf_trace trace;

        scribe_perf_trace_snapshot(&trace);
        fprintf(stderr,
                "SCRIBE_TRACE_PERF pack_validations=%llu pack_index_parses=%llu pack_file_opens=%llu "
                "full_pack_reads=%llu packed_object_reads=%llu pack_file_bytes_read=%llu "
                "pack_decompressed_bytes=%llu pack_validation_ms=%.3f object_type_count_ms=%.3f\n",
                (unsigned long long)trace.pack_validations, (unsigned long long)trace.pack_index_parses,
                (unsigned long long)trace.pack_file_opens, (unsigned long long)trace.full_pack_reads,
                (unsigned long long)trace.packed_object_reads, (unsigned long long)trace.pack_file_bytes_read,
                (unsigned long long)trace.pack_decompressed_bytes, (double)trace.pack_validation_ns / 1000000.0,
                (double)trace.object_type_count_ns / 1000000.0);
    }
    scribe_hash_set_destroy(&st.unique_hashes);
    return err;
}

scribe_error_t scribe_cli_info(scribe_ctx *ctx) {
    uint8_t main_hash[SCRIBE_HASH_SIZE];
    char main_hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    scribe_log_msg(ctx, SCRIBE_LOG_DEBUG, "repo", "info query");
    printf("scribe version %s\n", SCRIBE_VERSION);
    printf("hash_algorithm blake3-256\n");
    printf("pipe_protocol 2\n");
    printf("store %s\n", ctx->repo_path);
    printf("compression zstd level %d\n", ctx->config.compression_level);
    printf("worker_threads %d\n", ctx->config.worker_threads);
    printf("capabilities info refs log show diff cat-object ls-tree ingest content ingest-stream checkpoints\n");
    err = scribe_refs_read(ctx, "refs/heads/main", main_hash);
    if (err == SCRIBE_OK) {
        scribe_hash_to_hex(main_hash, main_hex);
        printf("main %s\n", main_hex);
    } else if (err == SCRIBE_ENOT_FOUND) {
        printf("main unborn\n");
    } else {
        return err;
    }
    print_fsck_health(ctx);
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_refs(scribe_ctx *ctx) {
    byte_buf refs = {0};
    scribe_error_t err = refs_text(ctx, &refs);

    if (err == SCRIBE_OK && refs.len != 0u && fwrite(refs.data, 1, refs.len, stdout) != refs.len) {
        err = scribe_set_error(SCRIBE_EIO, "failed to write refs output");
    }
    buf_destroy(&refs);
    return err;
}

static scribe_error_t print_ndjson_line_events(const uint8_t *body, size_t body_len) {
    size_t pos = 0;

    while (pos < body_len) {
        size_t start = pos;
        size_t line_len;
        char *line = NULL;
        size_t line_text_len = 0;
        scribe_error_t err;

        while (pos < body_len && body[pos] != '\n') {
            pos++;
        }
        line_len = pos - start;
        if (pos < body_len && body[pos] == '\n') {
            pos++;
            line_len++;
        }
        if (line_len == 0u) {
            continue;
        }
        err = scribe_ndjson_parse_line_event((const char *)body + start, line_len, &line, &line_text_len);
        if (err != SCRIBE_OK) {
            free(line);
            return err;
        }
        if (line_text_len != 0u && fwrite(line, 1, line_text_len, stdout) != line_text_len) {
            free(line);
            return scribe_set_error(SCRIBE_EIO, "failed to write remote query line");
        }
        free(line);
        fputc('\n', stdout);
    }
    return SCRIBE_OK;
}

static scribe_error_t remote_query_raw_print_config(const char *url, const char *token, const char *label,
                                                    const char *endpoint) {
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    scribe_error_t err = http_request(url, token, "GET", endpoint, NULL, 0, &status, &body, &body_len);

    if (err == SCRIBE_OK && status != 200) {
        int printable_len = body_len > 4096u ? 4096 : (int)body_len;

        err = scribe_set_error(SCRIBE_EADAPTER, "remote '%s' query failed with HTTP %d: %.*s", label, status,
                               printable_len, body == NULL ? "" : (char *)body);
    }
    if (err == SCRIBE_OK && body_len != 0u && fwrite(body, 1, body_len, stdout) != body_len) {
        err = scribe_set_error(SCRIBE_EIO, "failed to write remote query output");
    }
    free(body);
    return err;
}

static scribe_error_t remote_query_raw_print(scribe_ctx *ctx, const char *remote_name, const char *endpoint) {
    remote_config remote = {0};
    scribe_error_t err = remote_read(ctx, remote_name, &remote);

    if (err == SCRIBE_OK) {
        err = remote_query_raw_print_config(remote.url, remote.token, remote_name, endpoint);
    }
    remote_config_destroy(&remote);
    return err;
}

static scribe_error_t remote_query_ndjson_print_config(const char *url, const char *token, const char *label,
                                                       const char *endpoint) {
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    scribe_error_t err = http_request(url, token, "GET", endpoint, NULL, 0, &status, &body, &body_len);

    if (err == SCRIBE_OK && status != 200) {
        int printable_len = body_len > 4096u ? 4096 : (int)body_len;

        err = scribe_set_error(SCRIBE_EADAPTER, "remote '%s' query failed with HTTP %d: %.*s", label, status,
                               printable_len, body == NULL ? "" : (char *)body);
    }
    if (err == SCRIBE_OK) {
        err = print_ndjson_line_events(body, body_len);
    }
    free(body);
    return err;
}

static scribe_error_t remote_query_ndjson_print(scribe_ctx *ctx, const char *remote_name, const char *endpoint) {
    remote_config remote = {0};
    scribe_error_t err = remote_read(ctx, remote_name, &remote);

    if (err == SCRIBE_OK) {
        err = remote_query_ndjson_print_config(remote.url, remote.token, remote_name, endpoint);
    }
    remote_config_destroy(&remote);
    return err;
}

static scribe_error_t remote_request_raw_print_config(const char *url, const char *token, const char *label,
                                                      const char *method, const char *endpoint,
                                                      const uint8_t *request_body, size_t request_body_len) {
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    scribe_error_t err =
        http_request(url, token, method, endpoint, request_body, request_body_len, &status, &body, &body_len);

    if (err == SCRIBE_OK && status != 200) {
        int printable_len = body_len > 4096u ? 4096 : (int)body_len;

        err = scribe_set_error(SCRIBE_EADAPTER, "remote '%s' request failed with HTTP %d: %.*s", label, status,
                               printable_len, body == NULL ? "" : (char *)body);
    }
    if (err == SCRIBE_OK && body_len != 0u && fwrite(body, 1, body_len, stdout) != body_len) {
        err = scribe_set_error(SCRIBE_EIO, "failed to write remote response");
    }
    free(body);
    return err;
}

static scribe_error_t remote_request_raw_print(scribe_ctx *ctx, const char *remote_name, const char *method,
                                               const char *endpoint, const uint8_t *request_body,
                                               size_t request_body_len) {
    remote_config remote = {0};
    scribe_error_t err = remote_read(ctx, remote_name, &remote);

    if (err == SCRIBE_OK) {
        err = remote_request_raw_print_config(remote.url, remote.token, remote_name, method, endpoint, request_body,
                                              request_body_len);
    }
    remote_config_destroy(&remote);
    return err;
}

static scribe_error_t remote_post_raw_print_config(const char *url, const char *token, const char *label,
                                                   const char *endpoint, const uint8_t *request_body,
                                                   size_t request_body_len) {
    return remote_request_raw_print_config(url, token, label, "POST", endpoint, request_body, request_body_len);
}

static scribe_error_t remote_post_raw_print(scribe_ctx *ctx, const char *remote_name, const char *endpoint,
                                            const uint8_t *request_body, size_t request_body_len) {
    return remote_request_raw_print(ctx, remote_name, "POST", endpoint, request_body, request_body_len);
}

static scribe_error_t remote_query_ndjson_check_config(const char *url, const char *token, const char *label,
                                                       const char *endpoint) {
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    int saw_line = 0;
    size_t pos = 0;
    scribe_error_t err = http_request(url, token, "GET", endpoint, NULL, 0, &status, &body, &body_len);

    if (err == SCRIBE_OK && status != 200) {
        int printable_len = body_len > 4096u ? 4096 : (int)body_len;

        err = scribe_set_error(SCRIBE_EADAPTER, "remote '%s' query failed with HTTP %d: %.*s", label, status,
                               printable_len, body == NULL ? "" : (char *)body);
    }
    while (err == SCRIBE_OK && pos < body_len) {
        size_t start = pos;
        size_t line_len;
        char *line = NULL;
        size_t line_text_len = 0;

        while (pos < body_len && body[pos] != '\n') {
            pos++;
        }
        line_len = pos - start;
        if (pos < body_len && body[pos] == '\n') {
            pos++;
        }
        if (line_len == 0u) {
            continue;
        }
        err = scribe_ndjson_parse_line_event((const char *)body + start, line_len, &line, &line_text_len);
        free(line);
        if (err == SCRIBE_OK) {
            saw_line = 1;
        }
    }
    if (err == SCRIBE_OK && !saw_line) {
        err = scribe_set_error(SCRIBE_EPROTOCOL, "remote '%s' returned an empty info response", label);
    }
    free(body);
    return err;
}

static scribe_error_t remote_query_ndjson_check(scribe_ctx *ctx, const char *remote_name, const char *endpoint) {
    remote_config remote = {0};
    scribe_error_t err = remote_read(ctx, remote_name, &remote);

    if (err == SCRIBE_OK) {
        err = remote_query_ndjson_check_config(remote.url, remote.token, remote_name, endpoint);
    }
    remote_config_destroy(&remote);
    return err;
}

static scribe_error_t endpoint_append_encoded(byte_buf *endpoint, const char *text) {
    char *encoded = NULL;
    scribe_error_t err = url_encode_component(text, &encoded);

    if (err == SCRIBE_OK) {
        err = buf_append(endpoint, encoded, strlen(encoded));
    }
    free(encoded);
    return err;
}

static scribe_error_t content_endpoint_build(const char *path, const char *message, byte_buf *endpoint) {
    const char *component = path;
    const char *p;
    int first = 1;
    scribe_error_t err;

    if (path == NULL || path[0] == '\0') {
        return scribe_set_error(SCRIBE_EPATH, "content ingest path is required");
    }
    err = buf_append(endpoint, "/scribe/v1/content", strlen("/scribe/v1/content"));
    for (p = path; err == SCRIBE_OK; p++) {
        unsigned char ch = (unsigned char)*p;

        if (ch != '\0' && ch != '/' && (ch <= 0x1fu || ch == 0x7fu)) {
            return scribe_set_error(SCRIBE_EPATH, "invalid content ingest path component");
        }
        if (ch == '/' || ch == '\0') {
            size_t component_len = (size_t)(p - component);
            char *copy;

            if (component_len == 0u) {
                return scribe_set_error(SCRIBE_EPATH, "content ingest path contains an empty component");
            }
            copy = strndup(component, component_len);
            if (copy == NULL) {
                return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate content ingest path");
            }
            err = buf_append(endpoint, first ? "?path=" : "&path=", 6u);
            if (err == SCRIBE_OK) {
                err = endpoint_append_encoded(endpoint, copy);
            }
            free(copy);
            first = 0;
            if (ch == '\0') {
                break;
            }
            component = p + 1;
        }
    }
    if (err == SCRIBE_OK && message != NULL) {
        err = buf_append(endpoint, "&message=", 9u);
        if (err == SCRIBE_OK) {
            err = endpoint_append_encoded(endpoint, message);
        }
    }
    return err;
}

static scribe_error_t remote_query_one_arg(scribe_ctx *ctx, const char *remote, const char *prefix, const char *arg,
                                           int ndjson) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_append(&endpoint, prefix, strlen(prefix));

    if (err == SCRIBE_OK) {
        err = endpoint_append_encoded(&endpoint, arg);
    }
    if (err == SCRIBE_OK) {
        err = ndjson ? remote_query_ndjson_print(ctx, remote, (const char *)endpoint.data)
                     : remote_query_raw_print(ctx, remote, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

static scribe_error_t remote_query_one_arg_url(const char *url, const char *token, const char *prefix, const char *arg,
                                               int ndjson) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_append(&endpoint, prefix, strlen(prefix));

    if (err == SCRIBE_OK) {
        err = endpoint_append_encoded(&endpoint, arg);
    }
    if (err == SCRIBE_OK) {
        err = ndjson ? remote_query_ndjson_print_config(url, token, url, (const char *)endpoint.data)
                     : remote_query_raw_print_config(url, token, url, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_info(scribe_ctx *ctx, const char *remote) {
    return remote_query_ndjson_print(ctx, remote, "/scribe/v1/info");
}

scribe_error_t scribe_cli_remote_info_url(const char *url, const char *token) {
    return remote_query_ndjson_print_config(url, token, url, "/scribe/v1/info");
}

scribe_error_t scribe_cli_remote_check(scribe_ctx *ctx, const char *remote) {
    return remote_query_ndjson_check(ctx, remote, "/scribe/v1/info");
}

scribe_error_t scribe_cli_remote_check_url(const char *url, const char *token) {
    return remote_query_ndjson_check_config(url, token, url, "/scribe/v1/info");
}

scribe_error_t scribe_cli_remote_refs(scribe_ctx *ctx, const char *remote) {
    return remote_query_ndjson_print(ctx, remote, "/scribe/v1/refs");
}

scribe_error_t scribe_cli_remote_refs_url(const char *url, const char *token) {
    return remote_query_ndjson_print_config(url, token, url, "/scribe/v1/refs");
}

scribe_error_t scribe_cli_remote_ingest(scribe_ctx *ctx, const char *remote, const uint8_t *body, size_t body_len) {
    return remote_post_raw_print(ctx, remote, "/scribe/v1/ingest", body, body_len);
}

scribe_error_t scribe_cli_remote_ingest_url(const char *url, const char *token, const uint8_t *body, size_t body_len) {
    return remote_post_raw_print_config(url, token, url, "/scribe/v1/ingest", body, body_len);
}

scribe_error_t scribe_cli_remote_ingest_stream(scribe_ctx *ctx, const char *remote, const uint8_t *body,
                                               size_t body_len) {
    return remote_post_raw_print(ctx, remote, "/scribe/v1/ingest-stream", body, body_len);
}

scribe_error_t scribe_cli_remote_ingest_stream_url(const char *url, const char *token, const uint8_t *body,
                                                   size_t body_len) {
    return remote_post_raw_print_config(url, token, url, "/scribe/v1/ingest-stream", body, body_len);
}

scribe_error_t scribe_cli_remote_ingest_content(scribe_ctx *ctx, const char *remote, const char *path,
                                                const char *message, scribe_ingest_op op, const uint8_t *body,
                                                size_t body_len) {
    byte_buf endpoint = {0};
    const char *method;
    scribe_error_t err;

    if (op == SCRIBE_INGEST_OP_PUT) {
        method = "PUT";
    } else if (op == SCRIBE_INGEST_OP_DELETE && body == NULL && body_len == 0u) {
        method = "DELETE";
    } else {
        return scribe_set_error(SCRIBE_EINVAL, "invalid content ingest request");
    }
    err = content_endpoint_build(path, message, &endpoint);
    if (err == SCRIBE_OK) {
        err = remote_request_raw_print(ctx, remote, method, (const char *)endpoint.data, body, body_len);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_ingest_content_url(const char *url, const char *token, const char *path,
                                                    const char *message, scribe_ingest_op op, const uint8_t *body,
                                                    size_t body_len) {
    byte_buf endpoint = {0};
    const char *method;
    scribe_error_t err;

    if (op == SCRIBE_INGEST_OP_PUT) {
        method = "PUT";
    } else if (op == SCRIBE_INGEST_OP_DELETE && body == NULL && body_len == 0u) {
        method = "DELETE";
    } else {
        return scribe_set_error(SCRIBE_EINVAL, "invalid content ingest request");
    }
    err = content_endpoint_build(path, message, &endpoint);
    if (err == SCRIBE_OK) {
        err = remote_request_raw_print_config(url, token, url, method, (const char *)endpoint.data, body, body_len);
    }
    buf_destroy(&endpoint);
    return err;
}

static scribe_error_t endpoint_append_time_range(byte_buf *endpoint, const scribe_time_range *time_range) {
    scribe_error_t err = SCRIBE_OK;

    if (time_range == NULL) {
        return SCRIBE_OK;
    }
    if (time_range->has_since) {
        err = buf_appendf(endpoint, "&since=%lld", (long long)time_range->since_nanos);
    }
    if (err == SCRIBE_OK && time_range->has_until) {
        err = buf_appendf(endpoint, "&until=%lld", (long long)time_range->until_nanos);
    }
    return err;
}

scribe_error_t scribe_cli_remote_log(scribe_ctx *ctx, const char *remote, int oneline, size_t limit, int show_paths,
                                     const char *path_filter, const scribe_time_range *time_range) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_appendf(&endpoint, "/scribe/v1/log?oneline=%d&paths=%d&limit=%zu", oneline ? 1 : 0,
                                     show_paths ? 1 : 0, limit);

    if (err == SCRIBE_OK && path_filter != NULL) {
        err = buf_append(&endpoint, "&path=", 6u);
        if (err == SCRIBE_OK) {
            err = endpoint_append_encoded(&endpoint, path_filter);
        }
    }
    if (err == SCRIBE_OK) {
        err = endpoint_append_time_range(&endpoint, time_range);
    }
    if (err == SCRIBE_OK) {
        err = remote_query_ndjson_print(ctx, remote, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_log_url(const char *url, const char *token, int oneline, size_t limit, int show_paths,
                                         const char *path_filter, const scribe_time_range *time_range) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_appendf(&endpoint, "/scribe/v1/log?oneline=%d&paths=%d&limit=%zu", oneline ? 1 : 0,
                                     show_paths ? 1 : 0, limit);

    if (err == SCRIBE_OK && path_filter != NULL) {
        err = buf_append(&endpoint, "&path=", 6u);
        if (err == SCRIBE_OK) {
            err = endpoint_append_encoded(&endpoint, path_filter);
        }
    }
    if (err == SCRIBE_OK) {
        err = endpoint_append_time_range(&endpoint, time_range);
    }
    if (err == SCRIBE_OK) {
        err = remote_query_ndjson_print_config(url, token, url, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_show(scribe_ctx *ctx, const char *remote, const char *spec) {
    return remote_query_one_arg(ctx, remote, "/scribe/v1/show/", spec, 0);
}

scribe_error_t scribe_cli_remote_show_url(const char *url, const char *token, const char *spec) {
    return remote_query_one_arg_url(url, token, "/scribe/v1/show/", spec, 0);
}

scribe_error_t scribe_cli_remote_cat_object(scribe_ctx *ctx, const char *remote, char mode, const char *hex) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_append(&endpoint, "/scribe/v1/cat-object/", 22u);

    if (err == SCRIBE_OK) {
        err = endpoint_append_encoded(&endpoint, hex);
    }
    if (err == SCRIBE_OK) {
        err = buf_appendf(&endpoint, "?mode=%c", mode);
    }
    if (err == SCRIBE_OK) {
        err = remote_query_raw_print(ctx, remote, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_cat_object_url(const char *url, const char *token, char mode, const char *hex) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_append(&endpoint, "/scribe/v1/cat-object/", 22u);

    if (err == SCRIBE_OK) {
        err = endpoint_append_encoded(&endpoint, hex);
    }
    if (err == SCRIBE_OK) {
        err = buf_appendf(&endpoint, "?mode=%c", mode);
    }
    if (err == SCRIBE_OK) {
        err = remote_query_raw_print_config(url, token, url, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_diff(scribe_ctx *ctx, const char *remote, const char *a, const char *b) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_append(&endpoint, "/scribe/v1/diff/", 16u);

    if (err == SCRIBE_OK) {
        err = endpoint_append_encoded(&endpoint, a);
    }
    if (err == SCRIBE_OK && b != NULL) {
        err = buf_append(&endpoint, "/", 1u);
        if (err == SCRIBE_OK) {
            err = endpoint_append_encoded(&endpoint, b);
        }
    }
    if (err == SCRIBE_OK) {
        err = remote_query_ndjson_print(ctx, remote, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_diff_url(const char *url, const char *token, const char *a, const char *b) {
    byte_buf endpoint = {0};
    scribe_error_t err = buf_append(&endpoint, "/scribe/v1/diff/", 16u);

    if (err == SCRIBE_OK) {
        err = endpoint_append_encoded(&endpoint, a);
    }
    if (err == SCRIBE_OK && b != NULL) {
        err = buf_append(&endpoint, "/", 1u);
        if (err == SCRIBE_OK) {
            err = endpoint_append_encoded(&endpoint, b);
        }
    }
    if (err == SCRIBE_OK) {
        err = remote_query_ndjson_print_config(url, token, url, (const char *)endpoint.data);
    }
    buf_destroy(&endpoint);
    return err;
}

scribe_error_t scribe_cli_remote_ls_tree(scribe_ctx *ctx, const char *remote, const char *hex) {
    return remote_query_one_arg(ctx, remote, "/scribe/v1/ls-tree/", hex, 1);
}

scribe_error_t scribe_cli_remote_ls_tree_url(const char *url, const char *token, const char *hex) {
    return remote_query_one_arg_url(url, token, "/scribe/v1/ls-tree/", hex, 1);
}

scribe_error_t scribe_cli_remote_test(scribe_ctx *ctx, const char *name) {
    remote_config remote = {0};
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    scribe_error_t err = remote_read(ctx, name, &remote);

    if (err == SCRIBE_OK) {
        err = http_request(remote.url, remote.token, "GET", "/scribe/v1/info", NULL, 0, &status, &body, &body_len);
    }
    if (err == SCRIBE_OK && status != 200) {
        err = scribe_set_error(SCRIBE_EADAPTER, "remote '%s' test failed with HTTP %d: %s", name, status,
                               body == NULL ? "" : (char *)body);
    }
    if (err == SCRIBE_OK) {
        printf("remote %s ok\n", name);
        err = print_ndjson_line_events(body, body_len);
    }
    free(body);
    remote_config_destroy(&remote);
    return err;
}

scribe_error_t scribe_cli_replicate(scribe_ctx *ctx, const char *remote_name) {
    remote_config remote = {0};
    byte_buf inventory = {0};
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    char *bundle_path = NULL;
    size_t storage_entries = 0;
    size_t ref_entries = 0;
    scribe_error_t err = remote_read(ctx, remote_name, &remote);

    if (err == SCRIBE_OK) {
        err = object_inventory_text(ctx, &inventory, NULL);
    }
    if (err == SCRIBE_OK) {
        err = http_request(remote.url, remote.token, "POST", "/scribe/v1/negotiate-pull", inventory.data, inventory.len,
                           &status, &body, &body_len);
    }
    if (err == SCRIBE_OK && status != 200) {
        err = scribe_set_error(SCRIBE_EADAPTER, "replication from '%s' failed with HTTP %d: %s", remote_name, status,
                               body == NULL ? "" : (char *)body);
    }
    if (err == SCRIBE_OK) {
        err = write_temp_bytes(ctx, body, body_len, &bundle_path);
    }
    if (err == SCRIBE_OK) {
        err = scribe_bundle_import_file(ctx, bundle_path, "replicate", 1, &storage_entries, &ref_entries);
    }
    if (err == SCRIBE_OK) {
        printf("replicate %s: %zu storage entry(s), %zu ref(s)\n", remote_name, storage_entries, ref_entries);
    }
    if (bundle_path != NULL) {
        unlink(bundle_path);
    }
    free(bundle_path);
    free(body);
    buf_destroy(&inventory);
    remote_config_destroy(&remote);
    return err;
}

static void serve_signal(int signo) {
    (void)signo;
    g_serve_stop = 1;
}

static void serve_response_destroy(serve_response *res) {
    if (res != NULL) {
        free(res->body);
        memset(res, 0, sizeof(*res));
    }
}

static scribe_error_t send_response(serve_response *res, int status, const char *status_text, const char *content_type,
                                    const uint8_t *body, size_t body_len) {
    uint8_t *copy = NULL;
    int n;

    if (res == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid response object");
    }
    serve_response_destroy(res);
    if (body_len != 0u) {
        copy = (uint8_t *)malloc(body_len + 1u);
        if (copy == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP response body");
        }
        memcpy(copy, body, body_len);
        copy[body_len] = '\0';
    }
    n = snprintf(res->status_text, sizeof(res->status_text), "%s", status_text == NULL ? "" : status_text);
    if (n < 0 || (size_t)n >= sizeof(res->status_text)) {
        free(copy);
        return scribe_set_error(SCRIBE_EIO, "failed to format HTTP status text");
    }
    n = snprintf(res->content_type, sizeof(res->content_type), "%s",
                 content_type == NULL ? "application/octet-stream" : content_type);
    if (n < 0 || (size_t)n >= sizeof(res->content_type)) {
        free(copy);
        return scribe_set_error(SCRIBE_EIO, "failed to format HTTP content type");
    }
    res->status = status;
    res->body = copy;
    res->body_len = body_len;
    g_last_response_status = status;
    g_last_response_bytes = body_len;
    return SCRIBE_OK;
}

static scribe_error_t fd_send_response(int fd, const serve_response *res) {
    char header[256];
    int n;
    scribe_error_t err;

    if (res == NULL || res->status == 0) {
        return scribe_set_error(SCRIBE_EIO, "HTTP response was not produced");
    }
    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", res->status,
                 res->status_text, res->body_len, res->content_type);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        return scribe_set_error(SCRIBE_EIO, "failed to format HTTP response");
    }
    err = fd_write_all(fd, header, (size_t)n);
    if (err == SCRIBE_OK && res->body_len != 0u) {
        err = fd_write_all(fd, res->body, res->body_len);
    }
    return err;
}

static scribe_error_t send_text_response(serve_response *res, int status, const char *status_text, const char *fmt,
                                         ...) {
    byte_buf body = {0};
    va_list ap;
    va_list ap2;
    char stack[1024];
    char *heap = NULL;
    int n;
    scribe_error_t err;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return scribe_set_error(SCRIBE_EIO, "failed to format HTTP text response");
    }
    if ((size_t)n < sizeof(stack)) {
        va_end(ap2);
        err = buf_append(&body, stack, (size_t)n);
    } else {
        heap = (char *)malloc((size_t)n + 1u);
        if (heap == NULL) {
            va_end(ap2);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP text response");
        }
        (void)vsnprintf(heap, (size_t)n + 1u, fmt, ap2);
        va_end(ap2);
        err = buf_append(&body, heap, (size_t)n);
        free(heap);
    }
    if (err == SCRIBE_OK) {
        err = send_response(res, status, status_text, "text/plain", body.data, body.len);
    }
    buf_destroy(&body);
    return err;
}

static scribe_error_t json_append_string(byte_buf *out, const char *text) {
    const unsigned char *p = (const unsigned char *)(text == NULL ? "" : text);
    scribe_error_t err = buf_append(out, "\"", 1u);

    while (err == SCRIBE_OK && *p != '\0') {
        if (*p == '"' || *p == '\\') {
            char esc[2];

            esc[0] = '\\';
            esc[1] = (char)*p;
            err = buf_append(out, esc, sizeof(esc));
        } else if (*p == '\n') {
            err = buf_append(out, "\\n", 2u);
        } else if (*p == '\r') {
            err = buf_append(out, "\\r", 2u);
        } else if (*p == '\t') {
            err = buf_append(out, "\\t", 2u);
        } else if (*p < 0x20u) {
            err = buf_appendf(out, "\\u%04x", (unsigned int)*p);
        } else {
            err = buf_append(out, p, 1u);
        }
        p++;
    }
    if (err == SCRIBE_OK) {
        err = buf_append(out, "\"", 1u);
    }
    return err;
}

static scribe_error_t send_query_error(serve_response *res, int status, const char *status_text,
                                       scribe_error_t query_err) {
    byte_buf body = {0};
    scribe_error_t err = buf_append(&body, "{\"code\":", 8u);

    if (err == SCRIBE_OK) {
        err = json_append_string(&body, scribe_error_symbol(query_err));
    }
    if (err == SCRIBE_OK) {
        err = buf_append(&body, ",\"message\":", 11u);
    }
    if (err == SCRIBE_OK) {
        err = json_append_string(&body, scribe_last_error_detail());
    }
    if (err == SCRIBE_OK) {
        err = buf_append(&body, "}\n", 2u);
    }
    if (err == SCRIBE_OK) {
        err = send_response(res, status, status_text, "application/json", body.data, body.len);
    }
    buf_destroy(&body);
    return err;
}

typedef scribe_error_t (*captured_cli_fn)(void *user);

static scribe_error_t capture_stdout(captured_cli_fn fn, void *user, byte_buf *out) {
    FILE *tmp = tmpfile();
    int stdout_fd = fileno(stdout);
    int saved_fd = -1;
    uint8_t chunk[8192];
    scribe_error_t err = SCRIBE_OK;

    if (tmp == NULL) {
        return scribe_set_error(SCRIBE_EIO, "failed to create temporary output capture");
    }
    if (fflush(stdout) != 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to flush stdout before capture");
    }
    if (err == SCRIBE_OK) {
        saved_fd = dup(stdout_fd);
        if (saved_fd < 0) {
            err = scribe_set_error(SCRIBE_EIO, "failed to duplicate stdout");
        }
    }
    if (err == SCRIBE_OK && dup2(fileno(tmp), stdout_fd) < 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to redirect stdout");
    }
    if (err == SCRIBE_OK) {
        err = fn(user);
        if (fflush(stdout) != 0 && err == SCRIBE_OK) {
            err = scribe_set_error(SCRIBE_EIO, "failed to flush captured stdout");
        }
    }
    if (saved_fd >= 0) {
        if (dup2(saved_fd, stdout_fd) < 0 && err == SCRIBE_OK) {
            err = scribe_set_error(SCRIBE_EIO, "failed to restore stdout");
        }
        close(saved_fd);
    }
    if (err == SCRIBE_OK) {
        rewind(tmp);
        for (;;) {
            size_t n = fread(chunk, 1, sizeof(chunk), tmp);

            if (n != 0u) {
                err = buf_append(out, chunk, n);
                if (err != SCRIBE_OK) {
                    break;
                }
            }
            if (n < sizeof(chunk)) {
                if (ferror(tmp)) {
                    err = scribe_set_error(SCRIBE_EIO, "failed to read captured output");
                }
                break;
            }
        }
    }
    fclose(tmp);
    return err;
}

typedef struct {
    scribe_ctx *ctx;
} query_ctx_args;

typedef struct {
    scribe_ctx *ctx;
    int oneline;
    size_t limit;
    int show_paths;
    const char *path_filter;
    scribe_time_range time_range;
} query_log_args;

typedef struct {
    scribe_ctx *ctx;
    const char *arg;
} query_one_arg_args;

typedef struct {
    scribe_ctx *ctx;
    char mode;
    const char *hex;
} query_cat_args;

typedef struct {
    scribe_ctx *ctx;
    const char *a;
    const char *b;
} query_diff_args;

static scribe_error_t run_query_info(void *user) {
    query_ctx_args *args = (query_ctx_args *)user;

    return scribe_cli_info(args->ctx);
}

static scribe_error_t run_query_refs(void *user) {
    query_ctx_args *args = (query_ctx_args *)user;

    return scribe_cli_refs(args->ctx);
}

static scribe_error_t run_query_log(void *user) {
    query_log_args *args = (query_log_args *)user;

    return scribe_cli_log(args->ctx, args->oneline, args->limit, args->show_paths, args->path_filter,
                          &args->time_range);
}

static scribe_error_t run_query_show(void *user) {
    query_one_arg_args *args = (query_one_arg_args *)user;

    return scribe_cli_show(args->ctx, args->arg);
}

static scribe_error_t run_query_ls_tree(void *user) {
    query_one_arg_args *args = (query_one_arg_args *)user;

    return scribe_cli_ls_tree(args->ctx, args->arg);
}

static scribe_error_t run_query_cat_object(void *user) {
    query_cat_args *args = (query_cat_args *)user;

    return scribe_cli_cat_object(args->ctx, args->mode, args->hex);
}

static scribe_error_t run_query_diff(void *user) {
    query_diff_args *args = (query_diff_args *)user;

    return scribe_cli_diff(args->ctx, args->a, args->b);
}

static scribe_error_t send_captured_cli_response(serve_response *res, const char *content_type, captured_cli_fn fn,
                                                 void *user) {
    byte_buf body = {0};
    scribe_error_t err = capture_stdout(fn, user, &body);

    if (err == SCRIBE_OK) {
        err = send_response(res, 200, "OK", content_type, body.data, body.len);
    } else {
        scribe_error_t send_err = send_query_error(res, err == SCRIBE_ENOT_FOUND ? 404 : 400,
                                                   err == SCRIBE_ENOT_FOUND ? "Not Found" : "Bad Request", err);

        if (send_err != SCRIBE_OK) {
            err = send_err;
        } else {
            err = SCRIBE_OK;
        }
    }
    buf_destroy(&body);
    return err;
}

static scribe_error_t captured_text_to_ndjson(const byte_buf *text, byte_buf *out) {
    size_t pos = 0;

    while (pos < text->len) {
        size_t start = pos;
        size_t line_len;
        char *event = NULL;
        size_t event_len = 0;
        scribe_error_t err;

        while (pos < text->len && text->data[pos] != '\n') {
            pos++;
        }
        line_len = pos - start;
        if (pos < text->len && text->data[pos] == '\n') {
            pos++;
        }
        err = scribe_ndjson_line_event((const char *)text->data + start, line_len, &event, &event_len);
        if (err != SCRIBE_OK) {
            free(event);
            return err;
        }
        err = buf_append(out, event, event_len);
        free(event);
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    return SCRIBE_OK;
}

static scribe_error_t send_captured_cli_ndjson_response(serve_response *res, captured_cli_fn fn, void *user) {
    byte_buf text = {0};
    byte_buf ndjson = {0};
    scribe_error_t err = capture_stdout(fn, user, &text);

    if (err == SCRIBE_OK) {
        err = captured_text_to_ndjson(&text, &ndjson);
    }
    if (err == SCRIBE_OK) {
        err = send_response(res, 200, "OK", "application/x-ndjson", ndjson.data, ndjson.len);
    } else {
        scribe_error_t send_err = send_query_error(res, err == SCRIBE_ENOT_FOUND ? 404 : 400,
                                                   err == SCRIBE_ENOT_FOUND ? "Not Found" : "Bad Request", err);

        if (send_err != SCRIBE_OK) {
            err = send_err;
        } else {
            err = SCRIBE_OK;
        }
    }
    buf_destroy(&text);
    buf_destroy(&ndjson);
    return err;
}

static size_t request_target_path_len(const char *target) {
    const char *query = strchr(target, '?');

    return query == NULL ? strlen(target) : (size_t)(query - target);
}

static int request_target_equals(const char *target, const char *literal) {
    size_t len = request_target_path_len(target);

    return strlen(literal) == len && memcmp(target, literal, len) == 0;
}

static int request_target_has_prefix(const char *target, const char *prefix) {
    size_t target_len = request_target_path_len(target);
    size_t prefix_len = strlen(prefix);

    return target_len >= prefix_len && memcmp(target, prefix, prefix_len) == 0;
}

static scribe_error_t decode_target_suffix(const char *target, const char *prefix, char **out) {
    size_t prefix_len = strlen(prefix);
    size_t target_len = request_target_path_len(target);

    if (target_len < prefix_len) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "malformed query path");
    }
    return url_decode_component(target + prefix_len, target_len - prefix_len, out);
}

static scribe_error_t decode_checkpoint_target(const char *target, char **out_adapter, char **out_key) {
    const char *prefix = "/scribe/v1/checkpoints/";
    size_t prefix_len = strlen(prefix);
    size_t target_len = request_target_path_len(target);
    const char *rest;
    const char *slash;
    scribe_error_t err;

    if (out_adapter == NULL || out_key == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid checkpoint route arguments");
    }
    *out_adapter = NULL;
    *out_key = NULL;
    if (target_len <= prefix_len) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "malformed checkpoint path");
    }
    rest = target + prefix_len;
    slash = memchr(rest, '/', target_len - prefix_len);
    if (slash == NULL || slash == rest || slash + 1 >= target + target_len) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "malformed checkpoint path");
    }
    err = url_decode_component(rest, (size_t)(slash - rest), out_adapter);
    if (err == SCRIBE_OK) {
        err = url_decode_component(slash + 1, (size_t)(target + target_len - slash - 1), out_key);
    }
    if (err != SCRIBE_OK) {
        free(*out_adapter);
        free(*out_key);
        *out_adapter = NULL;
        *out_key = NULL;
    }
    return err;
}
static scribe_error_t query_param_decode(const char *target, const char *name, char **out) {
    const char *query = strchr(target, '?');
    size_t name_len = strlen(name);

    *out = NULL;
    if (query == NULL) {
        return SCRIBE_OK;
    }
    query++;
    while (*query != '\0') {
        const char *amp = strchr(query, '&');
        const char *eq = strchr(query, '=');
        size_t part_len = amp == NULL ? strlen(query) : (size_t)(amp - query);

        if (eq != NULL && eq < query + part_len && (size_t)(eq - query) == name_len &&
            memcmp(query, name, name_len) == 0) {
            return url_decode_component(eq + 1, part_len - (size_t)(eq + 1 - query), out);
        }
        if (amp == NULL) {
            break;
        }
        query = amp + 1;
    }
    return SCRIBE_OK;
}

static void content_query_destroy(char **path, size_t path_len, char *message) {
    size_t i;

    for (i = 0u; i < path_len; i++) {
        free(path[i]);
    }
    free(path);
    free(message);
}

static scribe_error_t content_query_decode(const char *target, char ***out_path, size_t *out_path_len,
                                           char **out_message) {
    const char *query = strchr(target, '?');
    char **path = NULL;
    size_t path_len = 0u;
    size_t path_cap = 0u;
    char *message = NULL;
    scribe_error_t err = SCRIBE_OK;

    *out_path = NULL;
    *out_path_len = 0u;
    *out_message = NULL;
    if (query == NULL) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "content ingest path is required");
    }
    query++;
    while (err == SCRIBE_OK && *query != '\0') {
        const char *amp = strchr(query, '&');
        const char *eq = strchr(query, '=');
        size_t part_len = amp == NULL ? strlen(query) : (size_t)(amp - query);

        if (eq != NULL && eq < query + part_len && (size_t)(eq - query) == 4u && memcmp(query, "path", 4u) == 0) {
            char *component = NULL;

            err = url_decode_component(eq + 1, part_len - (size_t)(eq + 1 - query), &component);
            if (err == SCRIBE_OK && path_len == path_cap) {
                size_t next_cap = path_cap == 0u ? 4u : path_cap * 2u;
                char **grown = (char **)realloc(path, sizeof(*path) * next_cap);

                if (grown == NULL) {
                    err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate content ingest path");
                } else {
                    path = grown;
                    path_cap = next_cap;
                }
            }
            if (err == SCRIBE_OK) {
                path[path_len++] = component;
                component = NULL;
            }
            free(component);
        } else if (eq != NULL && eq < query + part_len && (size_t)(eq - query) == 7u &&
                   memcmp(query, "message", 7u) == 0) {
            if (message != NULL) {
                err = scribe_set_error(SCRIBE_EPROTOCOL, "duplicate content ingest message");
            } else {
                err = url_decode_component(eq + 1, part_len - (size_t)(eq + 1 - query), &message);
            }
        }
        if (amp == NULL) {
            break;
        }
        query = amp + 1;
    }
    if (err == SCRIBE_OK && path_len == 0u) {
        err = scribe_set_error(SCRIBE_EPROTOCOL, "content ingest path is required");
    }
    if (err != SCRIBE_OK) {
        content_query_destroy(path, path_len, message);
        return err;
    }
    *out_path = path;
    *out_path_len = path_len;
    *out_message = message;
    return SCRIBE_OK;
}

static scribe_error_t query_param_int(const char *target, const char *name, int default_value, int *out) {
    char *value = NULL;
    char *end = NULL;
    long parsed;
    scribe_error_t err = query_param_decode(target, name, &value);

    if (err != SCRIBE_OK) {
        return err;
    }
    if (value == NULL) {
        *out = default_value;
        return SCRIBE_OK;
    }
    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        free(value);
        return scribe_set_error(SCRIBE_EPROTOCOL, "invalid integer query parameter '%s'", name);
    }
    *out = parsed != 0 ? 1 : 0;
    free(value);
    return SCRIBE_OK;
}

static scribe_error_t query_param_size(const char *target, const char *name, size_t default_value, size_t *out) {
    char *value = NULL;
    char *end = NULL;
    unsigned long parsed;
    scribe_error_t err = query_param_decode(target, name, &value);

    if (err != SCRIBE_OK) {
        return err;
    }
    if (value == NULL) {
        *out = default_value;
        return SCRIBE_OK;
    }
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        free(value);
        return scribe_set_error(SCRIBE_EPROTOCOL, "invalid size query parameter '%s'", name);
    }
    *out = (size_t)parsed;
    free(value);
    return SCRIBE_OK;
}

static scribe_error_t query_param_time(const char *target, const char *name, int *out_present, int64_t *out_nanos) {
    char *value = NULL;
    scribe_error_t err = query_param_decode(target, name, &value);

    if (err != SCRIBE_OK) {
        return err;
    }
    if (value == NULL) {
        *out_present = 0;
        *out_nanos = 0;
        return SCRIBE_OK;
    }
    err = scribe_parse_time_arg(value, out_nanos);
    if (err == SCRIBE_OK) {
        *out_present = 1;
    }
    free(value);
    return err;
}

static scribe_error_t handle_query_log(scribe_ctx *ctx, serve_response *res, const char *path) {
    query_log_args args;
    scribe_error_t err;

    memset(&args, 0, sizeof(args));
    args.ctx = ctx;
    err = query_param_int(path, "oneline", 0, &args.oneline);
    if (err == SCRIBE_OK) {
        err = query_param_int(path, "paths", 0, &args.show_paths);
    }
    if (err == SCRIBE_OK) {
        err = query_param_size(path, "limit", 0, &args.limit);
    }
    if (err == SCRIBE_OK) {
        err = query_param_decode(path, "path", (char **)&args.path_filter);
    }
    if (err == SCRIBE_OK) {
        err = query_param_time(path, "since", &args.time_range.has_since, &args.time_range.since_nanos);
    }
    if (err == SCRIBE_OK) {
        err = query_param_time(path, "until", &args.time_range.has_until, &args.time_range.until_nanos);
    }
    if (err == SCRIBE_OK && args.time_range.has_since && args.time_range.has_until &&
        args.time_range.since_nanos > args.time_range.until_nanos) {
        err = scribe_set_error(SCRIBE_EINVAL, "--since must be less than or equal to --until");
    }
    if (err == SCRIBE_OK) {
        err = send_captured_cli_ndjson_response(res, run_query_log, &args);
    } else {
        err = send_query_error(res, 400, "Bad Request", err);
    }
    free((void *)args.path_filter);
    return err;
}

static scribe_error_t handle_query_one_arg(scribe_ctx *ctx, serve_response *res, const char *path, const char *prefix,
                                           const char *content_type, captured_cli_fn fn) {
    query_one_arg_args args;
    scribe_error_t err;

    memset(&args, 0, sizeof(args));
    args.ctx = ctx;
    err = decode_target_suffix(path, prefix, (char **)&args.arg);
    if (err == SCRIBE_OK && strcmp(content_type, "application/x-ndjson") == 0) {
        err = send_captured_cli_ndjson_response(res, fn, &args);
    } else if (err == SCRIBE_OK) {
        err = send_captured_cli_response(res, content_type, fn, &args);
    } else {
        err = send_query_error(res, 400, "Bad Request", err);
    }
    free((void *)args.arg);
    return err;
}

static scribe_error_t handle_query_cat_object(scribe_ctx *ctx, serve_response *res, const char *path) {
    query_cat_args args;
    char *mode = NULL;
    scribe_error_t err;

    memset(&args, 0, sizeof(args));
    args.ctx = ctx;
    err = decode_target_suffix(path, "/scribe/v1/cat-object/", (char **)&args.hex);
    if (err == SCRIBE_OK) {
        err = query_param_decode(path, "mode", &mode);
    }
    if (err == SCRIBE_OK) {
        if (mode == NULL || strlen(mode) != 1u) {
            err = scribe_set_error(SCRIBE_EPROTOCOL, "cat-object mode query parameter is required");
        } else {
            args.mode = mode[0];
        }
    }
    if (err == SCRIBE_OK) {
        err = send_captured_cli_response(res, "application/octet-stream", run_query_cat_object, &args);
    } else {
        err = send_query_error(res, 400, "Bad Request", err);
    }
    free((void *)args.hex);
    free(mode);
    return err;
}

static scribe_error_t handle_query_diff(scribe_ctx *ctx, serve_response *res, const char *path) {
    query_diff_args args;
    const char *prefix = "/scribe/v1/diff/";
    size_t prefix_len = strlen(prefix);
    size_t path_len = request_target_path_len(path);
    const char *tail = path + prefix_len;
    const char *slash = NULL;
    scribe_error_t err;

    memset(&args, 0, sizeof(args));
    args.ctx = ctx;
    if (path_len < prefix_len) {
        err = scribe_set_error(SCRIBE_EPROTOCOL, "malformed diff query path");
    } else {
        slash = memchr(tail, '/', path_len - prefix_len);
        if (slash == NULL) {
            err = url_decode_component(tail, path_len - prefix_len, (char **)&args.a);
        } else {
            err = url_decode_component(tail, (size_t)(slash - tail), (char **)&args.a);
            if (err == SCRIBE_OK) {
                err = url_decode_component(slash + 1, path_len - (size_t)(slash + 1 - path), (char **)&args.b);
            }
        }
    }
    if (err == SCRIBE_OK) {
        err = send_captured_cli_ndjson_response(res, run_query_diff, &args);
    } else {
        err = send_query_error(res, 400, "Bad Request", err);
    }
    free((void *)args.a);
    free((void *)args.b);
    return err;
}

static uint8_t *find_header_end(byte_buf *buf) { return (uint8_t *)strstr((const char *)buf->data, "\r\n\r\n"); }

static long elapsed_ms(const struct timespec *start, const struct timespec *end) {
    long sec = (long)(end->tv_sec - start->tv_sec);
    long nsec = end->tv_nsec - start->tv_nsec;

    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    return sec * 1000L + nsec / 1000000L;
}

static scribe_error_t read_http_request(int fd, byte_buf *out, char **out_method, char **out_path, uint8_t **out_body,
                                        size_t *out_body_len) {
    uint8_t tmp[8192];
    size_t header_len = 0u;
    size_t content_length = 0;

    *out_method = NULL;
    *out_path = NULL;
    *out_body = NULL;
    *out_body_len = 0;
    while (header_len == 0u || out->len < header_len + content_length) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return scribe_set_error(SCRIBE_EIO, "failed to read HTTP request");
        }
        if (n == 0) {
            break;
        }
        {
            scribe_error_t err = buf_append(out, tmp, (size_t)n);
            if (err != SCRIBE_OK) {
                return err;
            }
        }
        if (header_len == 0u) {
            uint8_t *header_end = find_header_end(out);

            if (header_end != NULL) {
                char *headers = (char *)out->data;
                char *cl;

                header_len = (size_t)(header_end - out->data) + 4u;
                cl = strstr(headers, "\r\nContent-Length:");
                if (cl == NULL) {
                    cl = strstr(headers, "\r\ncontent-length:");
                }
                if (cl != NULL) {
                    content_length = (size_t)strtoull(cl + 17, NULL, 10);
                }
                if (content_length > SIZE_MAX - header_len) {
                    return scribe_set_error(SCRIBE_EIO, "HTTP request body is too large");
                }
            }
        }
    }
    if (header_len == 0u) {
        return scribe_set_error(SCRIBE_EIO, "HTTP request missing headers");
    }
    if (out->len < header_len + content_length) {
        return scribe_set_error(SCRIBE_EIO, "HTTP request body is truncated");
    }
    {
        char method[16];
        char path[1024];
        if (sscanf((const char *)out->data, "%15s %1023s", method, path) != 2) {
            return scribe_set_error(SCRIBE_EIO, "malformed HTTP request line");
        }
        *out_method = strdup(method);
        *out_path = strdup(path);
        if (*out_method == NULL || *out_path == NULL) {
            free(*out_method);
            free(*out_path);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate HTTP request line");
        }
    }
    *out_body = out->data + header_len;
    *out_body_len = content_length;
    return SCRIBE_OK;
}

static scribe_error_t handle_receive_pack(scribe_ctx *ctx, serve_response *res, const uint8_t *body, size_t body_len) {
    scribe_ctx *write_ctx = NULL;
    char *bundle_path = NULL;
    size_t storage_entries = 0;
    size_t ref_entries = 0;
    scribe_error_t err = scribe_open(ctx->repo_path, 1, &write_ctx);

    printf("serve receive-pack: received %zu byte(s)\n", body_len);
    fflush(stdout);
    if (err == SCRIBE_OK) {
        err = write_temp_bytes(write_ctx, body, body_len, &bundle_path);
    }
    if (err == SCRIBE_OK) {
        err = scribe_bundle_import_file(write_ctx, bundle_path, "receive-pack", 1, &storage_entries, &ref_entries);
    }
    if (bundle_path != NULL) {
        unlink(bundle_path);
    }
    free(bundle_path);
    scribe_close(write_ctx);
    if (err != SCRIBE_OK) {
        int status = err == SCRIBE_EREF_STALE ? 409 : err == SCRIBE_ELOCKED ? 423 : 400;
        printf("serve receive-pack: rejected %s %s\n", scribe_error_symbol(err), scribe_last_error_detail());
        fflush(stdout);
        return send_text_response(res, status,
                                  err == SCRIBE_EREF_STALE ? "Conflict"
                                  : err == SCRIBE_ELOCKED  ? "Locked"
                                                           : "Bad Request",
                                  "ERR %s %s\n", scribe_error_symbol(err), scribe_last_error_detail());
    }
    printf("serve receive-pack: imported %zu storage entry(s), %zu ref(s)\n", storage_entries, ref_entries);
    fflush(stdout);
    return send_text_response(res, 200, "OK", "OK %zu storage entry(s), %zu ref(s)\n", storage_entries, ref_entries);
}

static scribe_error_t handle_ingest(scribe_ctx *ctx, serve_response *res, const uint8_t *body, size_t body_len) {
    scribe_ctx *write_ctx = NULL;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    byte_buf response = {0};
    int status;
    const char *status_text;
    scribe_error_t err;

    if (body_len > SCRIBE_INGEST_MAX_BODY) {
        scribe_error_t query_err = scribe_set_error(SCRIBE_EINVAL, "ingest request exceeds 16 MiB");

        return send_query_error(res, 413, "Payload Too Large", query_err);
    }
    err = scribe_open(ctx->repo_path, 1, &write_ctx);
    if (err == SCRIBE_OK) {
        err = scribe_ingest_json_commit(write_ctx, body, body_len, commit_hash);
    }
    scribe_close(write_ctx);
    if (err != SCRIBE_OK) {
        status = err == SCRIBE_ELOCKED ? 423 : 400;
        status_text = err == SCRIBE_ELOCKED ? "Locked" : "Bad Request";
        printf("serve ingest: rejected %s %s\n", scribe_error_symbol(err), scribe_last_error_detail());
        fflush(stdout);
        return send_query_error(res, status, status_text, err);
    }
    scribe_hash_to_hex(commit_hash, commit_hex);
    printf("serve ingest: committed %s from %zu byte request\n", commit_hex, body_len);
    fflush(stdout);
    err = buf_appendf(&response, "{\"commit\":\"%s\"}\n", commit_hex);
    if (err == SCRIBE_OK) {
        err = send_response(res, 200, "OK", "application/json", response.data, response.len);
    }
    buf_destroy(&response);
    return err;
}

static scribe_error_t handle_ingest_content(scribe_ctx *ctx, serve_response *res, const char *target,
                                            scribe_ingest_op op, const uint8_t *body, size_t body_len) {
    char **path = NULL;
    size_t path_len = 0u;
    char *message = NULL;
    scribe_ctx *write_ctx = NULL;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    byte_buf response = {0};
    int status;
    const char *status_text;
    scribe_error_t err;

    if (body_len > SCRIBE_INGEST_MAX_BODY) {
        scribe_error_t query_err = scribe_set_error(SCRIBE_EINVAL, "content ingest request exceeds 16 MiB");

        return send_query_error(res, 413, "Payload Too Large", query_err);
    }
    if (op == SCRIBE_INGEST_OP_DELETE && body_len != 0u) {
        scribe_error_t query_err = scribe_set_error(SCRIBE_EPROTOCOL, "DELETE content ingest cannot include a body");

        return send_query_error(res, 400, "Bad Request", query_err);
    }
    err = content_query_decode(target, &path, &path_len, &message);
    if (err != SCRIBE_OK) {
        return send_query_error(res, 400, "Bad Request", err);
    }
    err = scribe_open(ctx->repo_path, 1, &write_ctx);
    if (err == SCRIBE_OK) {
        err = scribe_ingest_content_commit(write_ctx, (const char *const *)path, path_len, op,
                                           op == SCRIBE_INGEST_OP_PUT ? body : NULL,
                                           op == SCRIBE_INGEST_OP_PUT ? body_len : 0u, message, commit_hash);
    }
    scribe_close(write_ctx);
    content_query_destroy(path, path_len, message);
    if (err != SCRIBE_OK) {
        status = err == SCRIBE_ELOCKED ? 423 : 400;
        status_text = err == SCRIBE_ELOCKED ? "Locked" : "Bad Request";
        printf("serve content ingest: rejected %s %s\n", scribe_error_symbol(err), scribe_last_error_detail());
        fflush(stdout);
        return send_query_error(res, status, status_text, err);
    }
    scribe_hash_to_hex(commit_hash, commit_hex);
    printf("serve content ingest: committed %s from %zu byte request\n", commit_hex, body_len);
    fflush(stdout);
    err = buf_appendf(&response, "{\"commit\":\"%s\"}\n", commit_hex);
    if (err == SCRIBE_OK) {
        err = send_response(res, 200, "OK", "application/json", response.data, response.len);
    }
    buf_destroy(&response);
    return err;
}

static scribe_error_t handle_ingest_stream(scribe_ctx *ctx, serve_response *res, const uint8_t *body, size_t body_len) {
    scribe_ctx *write_ctx = NULL;
    FILE *in = NULL;
    FILE *out = NULL;
    char *response = NULL;
    size_t response_len = 0u;
    scribe_error_t err;

    if (body_len > SCRIBE_INGEST_STREAM_MAX_BODY) {
        scribe_error_t query_err = scribe_set_error(SCRIBE_EINVAL, "ingest stream request exceeds 128 MiB");

        return send_query_error(res, 413, "Payload Too Large", query_err);
    }
    in = fmemopen((void *)body, body_len, "rb");
    if (in == NULL) {
        return send_query_error(res, 500, "Internal Server Error",
                                scribe_set_error(SCRIBE_EIO, "failed to open ingest stream body"));
    }
    out = open_memstream(&response, &response_len);
    if (out == NULL) {
        fclose(in);
        return send_query_error(res, 500, "Internal Server Error",
                                scribe_set_error(SCRIBE_EIO, "failed to open ingest stream response"));
    }
    err = scribe_open(ctx->repo_path, 1, &write_ctx);
    if (err == SCRIBE_OK) {
        err = scribe_pipe_commit_batch(write_ctx, in, out);
    }
    scribe_close(write_ctx);
    fclose(in);
    fclose(out);
    if (err != SCRIBE_OK) {
        int status = err == SCRIBE_ELOCKED ? 423 : 400;
        const char *status_text = err == SCRIBE_ELOCKED ? "Locked" : "Bad Request";

        printf("serve ingest-stream: rejected %s %s\n", scribe_error_symbol(err), scribe_last_error_detail());
        fflush(stdout);
        if (response_len != 0u) {
            scribe_error_t send_err =
                send_response(res, status, status_text, "text/plain", (const uint8_t *)response, response_len);
            free(response);
            return send_err;
        }
        free(response);
        return send_query_error(res, status, status_text, err);
    }
    printf("serve ingest-stream: committed frame stream from %zu byte request\n", body_len);
    fflush(stdout);
    err = send_response(res, 200, "OK", "text/plain", (const uint8_t *)response, response_len);
    free(response);
    return err;
}

static scribe_error_t handle_checkpoint_get(scribe_ctx *ctx, serve_response *res, const char *path) {
    char *adapter = NULL;
    char *key = NULL;
    uint8_t *value = NULL;
    size_t value_len = 0u;
    char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char *encoded = NULL;
    byte_buf response = {0};
    scribe_error_t err = decode_checkpoint_target(path, &adapter, &key);

    if (err == SCRIBE_OK) {
        err = scribe_checkpoint_read(ctx, adapter, key, &value, &value_len, commit_hex);
    }
    if (err == SCRIBE_ENOT_FOUND) {
        scribe_error_t query_err = scribe_set_error(SCRIBE_ENOT_FOUND, "checkpoint not found");

        free(adapter);
        free(key);
        return send_query_error(res, 404, "Not Found", query_err);
    }
    if (err == SCRIBE_OK) {
        err = base64_encode_bytes(value, value_len, &encoded);
    }
    if (err == SCRIBE_OK) {
        err = buf_appendf(&response, "{\"commit\":\"%s\",\"value\":\"%s\"}\n", commit_hex, encoded);
    }
    if (err == SCRIBE_OK) {
        err = send_response(res, 200, "OK", "application/json", response.data, response.len);
    }
    free(adapter);
    free(key);
    free(value);
    free(encoded);
    buf_destroy(&response);
    return err;
}

static scribe_error_t handle_upload_pack(scribe_ctx *ctx, serve_response *res) {
    char *bundle_path = NULL;
    uint8_t *bytes = NULL;
    size_t len = 0;
    size_t loose_count = 0;
    size_t pack_file_count = 0;
    size_t ref_count = 0;
    scribe_error_t err = temp_file_path(ctx, "sync-bundle", &bundle_path);

    if (err == SCRIBE_OK) {
        err = scribe_bundle_create_file(ctx, bundle_path, &loose_count, &pack_file_count, &ref_count);
    }
    if (err == SCRIBE_OK) {
        err = scribe_read_file(bundle_path, &bytes, &len);
    }
    if (bundle_path != NULL) {
        unlink(bundle_path);
    }
    free(bundle_path);
    if (err != SCRIBE_OK) {
        free(bytes);
        return send_text_response(res, 500, "Internal Server Error", "ERR %s %s\n", scribe_error_symbol(err),
                                  scribe_last_error_detail());
    }
    printf("serve upload-pack: sent full bundle with %zu loose object(s), %zu pack file(s), %zu ref(s), %zu byte(s)\n",
           loose_count, pack_file_count, ref_count, len);
    fflush(stdout);
    err = send_response(res, 200, "OK", "application/octet-stream", bytes, len);
    free(bytes);
    return err;
}

static scribe_error_t handle_negotiate_push(scribe_ctx *ctx, serve_response *res) {
    byte_buf inventory = {0};
    size_t object_count = 0;
    scribe_error_t err = object_inventory_text(ctx, &inventory, &object_count);

    if (err != SCRIBE_OK) {
        buf_destroy(&inventory);
        return err;
    }
    printf("serve negotiate-push: advertised %zu existing object(s)\n", object_count);
    fflush(stdout);
    err = send_response(res, 200, "OK", "text/plain", inventory.data, inventory.len);
    buf_destroy(&inventory);
    return err;
}

static scribe_error_t handle_negotiate_pull(scribe_ctx *ctx, serve_response *res, const uint8_t *body,
                                            size_t body_len) {
    sync_hash_list receiver_inventory = {0};
    char *bundle_path = NULL;
    uint8_t *bytes = NULL;
    size_t len = 0;
    size_t storage_entries = 0;
    size_t ref_entries = 0;
    scribe_error_t err = parse_inventory_text(body, body_len, &receiver_inventory);

    if (err == SCRIBE_OK) {
        err = create_temp_filtered_bundle(ctx, &receiver_inventory, &bundle_path, &storage_entries, &ref_entries);
    }
    if (err == SCRIBE_OK) {
        err = scribe_read_file(bundle_path, &bytes, &len);
    }
    if (bundle_path != NULL) {
        unlink(bundle_path);
    }
    free(bundle_path);
    sync_hash_list_destroy(&receiver_inventory);
    if (err != SCRIBE_OK) {
        free(bytes);
        return err;
    }
    printf("serve negotiate-pull: sent %zu missing storage entry(s), %zu ref(s), %zu byte(s)\n", storage_entries,
           ref_entries, len);
    fflush(stdout);
    err = send_response(res, 200, "OK", "application/octet-stream", bytes, len);
    free(bytes);
    return err;
}

static scribe_error_t serve_dispatch_request(scribe_ctx *ctx, const char *method, const char *path, const uint8_t *body,
                                             size_t body_len, serve_response *res) {
    scribe_error_t err = SCRIBE_OK;

    if (err == SCRIBE_OK && strcmp(method, "GET") == 0 && request_target_equals(path, "/scribe/v1/info")) {
        query_ctx_args args;

        args.ctx = ctx;
        err = send_captured_cli_ndjson_response(res, run_query_info, &args);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 && request_target_equals(path, "/scribe/v1/refs")) {
        query_ctx_args args;

        args.ctx = ctx;
        err = send_captured_cli_ndjson_response(res, run_query_refs, &args);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 && request_target_equals(path, "/scribe/v1/log")) {
        err = handle_query_log(ctx, res, path);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 && request_target_has_prefix(path, "/scribe/v1/show/")) {
        err = handle_query_one_arg(ctx, res, path, "/scribe/v1/show/", "application/octet-stream", run_query_show);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 &&
               request_target_has_prefix(path, "/scribe/v1/ls-tree/")) {
        err = handle_query_one_arg(ctx, res, path, "/scribe/v1/ls-tree/", "application/x-ndjson", run_query_ls_tree);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 &&
               request_target_has_prefix(path, "/scribe/v1/cat-object/")) {
        err = handle_query_cat_object(ctx, res, path);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 && request_target_has_prefix(path, "/scribe/v1/diff/")) {
        err = handle_query_diff(ctx, res, path);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 &&
               request_target_has_prefix(path, "/scribe/v1/checkpoints/")) {
        err = handle_checkpoint_get(ctx, res, path);
    } else if (err == SCRIBE_OK && strcmp(method, "GET") == 0 && strcmp(path, "/scribe/v1/upload-pack") == 0) {
        err = handle_upload_pack(ctx, res);
    } else if (err == SCRIBE_OK && strcmp(method, "POST") == 0 && strcmp(path, "/scribe/v1/receive-pack") == 0) {
        err = handle_receive_pack(ctx, res, body, body_len);
    } else if (err == SCRIBE_OK && strcmp(method, "POST") == 0 && strcmp(path, "/scribe/v1/ingest") == 0) {
        err = handle_ingest(ctx, res, body, body_len);
    } else if (err == SCRIBE_OK && strcmp(method, "PUT") == 0 && request_target_equals(path, "/scribe/v1/content")) {
        err = handle_ingest_content(ctx, res, path, SCRIBE_INGEST_OP_PUT, body, body_len);
    } else if (err == SCRIBE_OK && strcmp(method, "DELETE") == 0 && request_target_equals(path, "/scribe/v1/content")) {
        err = handle_ingest_content(ctx, res, path, SCRIBE_INGEST_OP_DELETE, body, body_len);
    } else if (err == SCRIBE_OK && strcmp(method, "POST") == 0 && strcmp(path, "/scribe/v1/ingest-stream") == 0) {
        err = handle_ingest_stream(ctx, res, body, body_len);
    } else if (err == SCRIBE_OK && strcmp(method, "POST") == 0 && strcmp(path, "/scribe/v1/negotiate-pull") == 0) {
        err = handle_negotiate_pull(ctx, res, body, body_len);
    } else if (err == SCRIBE_OK && strcmp(method, "POST") == 0 && strcmp(path, "/scribe/v1/negotiate-push") == 0) {
        err = handle_negotiate_push(ctx, res);
    } else if (err == SCRIBE_OK && strcmp(method, "POST") == 0 && strcmp(path, "/scribe/v1/update-ref") == 0) {
        scribe_error_t query_err = scribe_set_error(SCRIBE_ENOSYS, "update-ref is not used by M6");

        err = send_query_error(res, 501, "Not Implemented", query_err);
    } else if (err == SCRIBE_OK) {
        scribe_error_t query_err = scribe_set_error(SCRIBE_ENOT_FOUND, "endpoint not found");

        err = send_query_error(res, 404, "Not Found", query_err);
    }
    if (err != SCRIBE_OK) {
        (void)send_query_error(res, 500, "Internal Server Error", err);
    }
    return err;
}

#ifdef SCRIBE_HAVE_TLS
static scribe_error_t h2_build_response_headers(const serve_response *res, byte_buf *out) {
    char status[16];
    char content_length[64];
    int n = snprintf(status, sizeof(status), "%d", res->status);
    scribe_error_t err;

    if (n < 0 || (size_t)n >= sizeof(status)) {
        return scribe_set_error(SCRIBE_EIO, "failed to format HTTP/2 status");
    }
    n = snprintf(content_length, sizeof(content_length), "%zu", res->body_len);
    if (n < 0 || (size_t)n >= sizeof(content_length)) {
        return scribe_set_error(SCRIBE_EIO, "failed to format HTTP/2 content length");
    }
    err = hpack_encode_header(out, ":status", status);
    if (err == SCRIBE_OK) {
        err = hpack_encode_header(out, "content-type", res->content_type);
    }
    if (err == SCRIBE_OK) {
        err = hpack_encode_header(out, "content-length", content_length);
    }
    return err;
}

static scribe_error_t h2_send_response(SSL *ssl, int32_t stream_id, const serve_response *res) {
    byte_buf headers = {0};
    size_t off = 0;
    scribe_error_t err = h2_build_response_headers(res, &headers);

    if (err == SCRIBE_OK) {
        err = h2_write_frame(ssl, H2_FRAME_HEADERS, H2_FLAG_END_HEADERS, stream_id, headers.data, headers.len);
    }
    while (err == SCRIBE_OK && (off < res->body_len || (res->body_len == 0u && off == 0u))) {
        size_t chunk = res->body_len - off;
        const uint8_t *chunk_data = res->body == NULL ? (const uint8_t *)"" : res->body + off;
        uint8_t flags;

        if (chunk > 16384u) {
            chunk = 16384u;
        }
        flags = off + chunk == res->body_len ? H2_FLAG_END_STREAM : 0u;
        err = h2_write_frame(ssl, H2_FRAME_DATA, flags, stream_id, chunk_data, chunk);
        off += chunk;
        if (res->body_len == 0u) {
            break;
        }
    }
    buf_destroy(&headers);
    return err;
}

static scribe_error_t serve_h2_connection(scribe_ctx *ctx, SSL *ssl) {
    static const uint8_t client_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    uint8_t preface[sizeof(client_preface) - 1u];
    h2_headers headers = {0};
    byte_buf body = {0};
    int32_t active_stream = 0;
    int done = 0;
    scribe_error_t err;

    if (!ssl_read_exact(ssl, preface, sizeof(preface)) || memcmp(preface, client_preface, sizeof(preface)) != 0) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "invalid HTTP/2 client preface");
    }
    err = h2_write_frame(ssl, H2_FRAME_SETTINGS, 0u, 0, NULL, 0u);
    while (err == SCRIBE_OK && !done && !g_serve_stop) {
        h2_frame_header frame;
        byte_buf frame_payload = {0};

        err = h2_read_frame_header(ssl, &frame);
        if (err != SCRIBE_OK) {
            break;
        }
        err = h2_read_frame_payload(ssl, &frame, &frame_payload);
        if (err != SCRIBE_OK) {
            buf_destroy(&frame_payload);
            break;
        }
        if (frame.type == H2_FRAME_SETTINGS) {
            if ((frame.flags & H2_FLAG_ACK) == 0u) {
                err = h2_write_frame(ssl, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0u);
            }
        } else if (frame.type == H2_FRAME_HEADERS) {
            active_stream = frame.stream_id;
            h2_headers_destroy(&headers);
            buf_destroy(&body);
            memset(&headers, 0, sizeof(headers));
            memset(&body, 0, sizeof(body));
            err = hpack_decode_headers(frame_payload.data, frame_payload.len, &headers);
            if (err == SCRIBE_OK && (frame.flags & H2_FLAG_END_STREAM) != 0u) {
                done = 1;
            }
        } else if (frame.type == H2_FRAME_DATA && frame.stream_id == active_stream) {
            err = buf_append(&body, frame_payload.data, frame_payload.len);
            if (err == SCRIBE_OK && (frame.flags & H2_FLAG_END_STREAM) != 0u) {
                done = 1;
            }
        } else if (frame.type == H2_FRAME_GOAWAY) {
            done = 1;
        }
        buf_destroy(&frame_payload);
    }
    if (err == SCRIBE_OK && active_stream != 0 && headers.method != NULL && headers.path != NULL) {
        serve_response res = {0};
        struct timespec started;
        struct timespec finished;

        g_last_response_status = 0;
        g_last_response_bytes = 0;
        (void)clock_gettime(CLOCK_MONOTONIC, &started);
        scribe_log_msg(ctx, SCRIBE_LOG_INFO, "serve", "request started: method=%s path=%s request_bytes=%zu",
                       headers.method, headers.path, body.len);
        scribe_log_flush(ctx);
        err = serve_dispatch_request(ctx, headers.method, headers.path, body.data, body.len, &res);
        if (err == SCRIBE_OK) {
            err = h2_send_response(ssl, active_stream, &res);
        }
        if (err == SCRIBE_OK) {
            for (;;) {
                h2_frame_header extra;
                byte_buf extra_payload = {0};
                scribe_error_t drain_err = h2_read_frame_header(ssl, &extra);

                if (drain_err != SCRIBE_OK) {
                    break;
                }
                drain_err = h2_read_frame_payload(ssl, &extra, &extra_payload);
                buf_destroy(&extra_payload);
                if (drain_err != SCRIBE_OK || extra.type == H2_FRAME_GOAWAY) {
                    break;
                }
            }
        }
        (void)clock_gettime(CLOCK_MONOTONIC, &finished);
        scribe_log_msg(ctx, SCRIBE_LOG_INFO, "serve",
                       "request: method=%s path=%s status=%d duration_ms=%ld request_bytes=%zu response_bytes=%zu",
                       headers.method, headers.path, res.status, elapsed_ms(&started, &finished), body.len,
                       res.body_len);
        scribe_log_flush(ctx);
        serve_response_destroy(&res);
    } else if (err == SCRIBE_OK && !done) {
        err = scribe_set_error(SCRIBE_EPROTOCOL, "HTTP/2 request did not include method and path");
    }
    h2_headers_destroy(&headers);
    buf_destroy(&body);
    return err;
}

static int h2_alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg) {
    static const unsigned char h2_proto[] = {2u, 'h', '2'};

    (void)ssl;
    (void)arg;
    if (SSL_select_next_proto((unsigned char **)out, outlen, h2_proto, sizeof(h2_proto), in, inlen) ==
        OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

static scribe_error_t create_server_tls_ctx(const char *cert, const char *key, SSL_CTX **out) {
    SSL_CTX *ssl_ctx;

    *out = NULL;
    if (cert == NULL || cert[0] == '\0' || key == NULL || key[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "server TLS requires both --cert and --key");
    }
    ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (ssl_ctx == NULL) {
        return openssl_error("failed to create TLS server context");
    }
    SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_alpn_select_cb(ssl_ctx, h2_alpn_select, NULL);
    if (SSL_CTX_use_certificate_file(ssl_ctx, cert, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl_ctx);
        return openssl_error("failed to load TLS certificate");
    }
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl_ctx);
        return openssl_error("failed to load TLS private key");
    }
    if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
        SSL_CTX_free(ssl_ctx);
        return openssl_error("TLS private key does not match certificate");
    }
    *out = ssl_ctx;
    return SCRIBE_OK;
}

static scribe_error_t serve_one_tls_h2(scribe_ctx *ctx, SSL_CTX *ssl_ctx, int fd) {
    SSL *ssl = SSL_new(ssl_ctx);
    const unsigned char *selected = NULL;
    unsigned int selected_len = 0;
    scribe_error_t err = SCRIBE_OK;

    if (ssl == NULL) {
        return openssl_error("failed to create TLS server connection");
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        err = openssl_error("failed to attach TLS server connection to socket");
    }
    if (err == SCRIBE_OK && SSL_accept(ssl) != 1) {
        err = openssl_error("TLS handshake failed");
    }
    if (err == SCRIBE_OK) {
        SSL_get0_alpn_selected(ssl, &selected, &selected_len);
        if (selected_len != 2u || memcmp(selected, "h2", 2u) != 0) {
            err = scribe_set_error(SCRIBE_EPROTOCOL, "TLS client did not negotiate HTTP/2");
        }
    }
    if (err == SCRIBE_OK) {
        err = serve_h2_connection(ctx, ssl);
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    return err;
}
#endif

static scribe_error_t serve_one(scribe_ctx *ctx, int fd) {
    byte_buf req = {0};
    char *method = NULL;
    char *path = NULL;
    uint8_t *body = NULL;
    size_t body_len = 0;
    serve_response res = {0};
    struct timespec started;
    struct timespec finished;
    scribe_error_t err;

    g_last_response_status = 0;
    g_last_response_bytes = 0;
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    err = read_http_request(fd, &req, &method, &path, &body, &body_len);
    if (err == SCRIBE_OK) {
        scribe_log_msg(ctx, SCRIBE_LOG_INFO, "serve", "request started: method=%s path=%s request_bytes=%zu", method,
                       path, body_len);
        scribe_log_flush(ctx);
        err = serve_dispatch_request(ctx, method, path, body, body_len, &res);
    }
    if (err == SCRIBE_OK) {
        err = fd_send_response(fd, &res);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    scribe_log_msg(ctx, SCRIBE_LOG_INFO, "serve",
                   "request: method=%s path=%s status=%d duration_ms=%ld request_bytes=%zu response_bytes=%zu",
                   method == NULL ? "-" : method, path == NULL ? "-" : path, g_last_response_status,
                   elapsed_ms(&started, &finished), body_len, g_last_response_bytes);
    scribe_log_flush(ctx);
    free(method);
    free(path);
    serve_response_destroy(&res);
    buf_destroy(&req);
    return err;
}

static scribe_error_t parse_listen_addr(const char *listen_addr, char **out_host, char **out_port) {
    const char *colon;
    size_t host_len;

    if (listen_addr == NULL || listen_addr[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "listen address is required");
    }
    colon = strrchr(listen_addr, ':');
    if (colon == NULL || colon[1] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "listen address must be host:port");
    }
    host_len = (size_t)(colon - listen_addr);
    *out_host = (char *)malloc(host_len + 1u);
    *out_port = strdup(colon + 1);
    if (*out_host == NULL || *out_port == NULL) {
        free(*out_host);
        free(*out_port);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate listen address");
    }
    memcpy(*out_host, listen_addr, host_len);
    (*out_host)[host_len] = '\0';
    if ((*out_host)[0] == '\0') {
        free(*out_host);
        *out_host = strdup("0.0.0.0");
        if (*out_host == NULL) {
            free(*out_port);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate listen host");
        }
    }
    return SCRIBE_OK;
}

static scribe_error_t listen_socket(const char *listen_addr, int *out_fd) {
    char *host = NULL;
    char *port = NULL;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int fd = -1;
    int one = 1;
    int rc;
    scribe_error_t err = parse_listen_addr(listen_addr, &host, &port);

    if (err != SCRIBE_OK) {
        return err;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        free(host);
        free(port);
        return scribe_set_error(SCRIBE_EIO, "failed to resolve listen address '%s': %s", listen_addr, gai_strerror(rc));
    }
    for (it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0 && listen(fd, 16) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    free(host);
    free(port);
    if (fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to listen on %s", listen_addr);
    }
    *out_fd = fd;
    return SCRIBE_OK;
}

static char *serve_resolved_repo_path(scribe_ctx *ctx) {
    char *resolved;

    if (ctx == NULL || ctx->repo_path == NULL) {
        return strdup("unknown");
    }
    resolved = realpath(ctx->repo_path, NULL);
    if (resolved != NULL) {
        return resolved;
    }
    return strdup(ctx->repo_path);
}

static void print_serve_banner(scribe_ctx *ctx, const char *listen_addr, const char *cert, const char *key) {
    char *repo = serve_resolved_repo_path(ctx);
    char *log_path = NULL;
    uint8_t main_hash[SCRIBE_HASH_SIZE];
    char main_hex[SCRIBE_HEX_HASH_SIZE + 1];
    const char *main_ref = "<none>";
    const char *transport = "HTTP/1.1";
    const char *tls = "disabled";
    scribe_error_t ref_err;

    if (repo != NULL && strcmp(repo, "unknown") != 0) {
        log_path = scribe_path_join(repo, "scribe_server.log");
    }
    ref_err = ctx == NULL ? SCRIBE_EINVAL : scribe_refs_read(ctx, "refs/heads/main", main_hash);
    if (ref_err == SCRIBE_OK) {
        scribe_hash_to_hex(main_hash, main_hex);
        main_ref = main_hex;
    } else if (ref_err != SCRIBE_ENOT_FOUND) {
        main_ref = "<unavailable>";
    }
    if ((cert != NULL && cert[0] != '\0') || (key != NULL && key[0] != '\0')) {
#ifdef SCRIBE_HAVE_TLS
        transport = "HTTP/2";
        tls = "enabled";
#else
        tls = "disabled (secure transport support is not enabled in this build)";
#endif
    }

    fputs("\n"
          "  ____            _ _          \n"
          " / ___|  ___ _ __(_) |__   ___ \n"
          " \\___ \\ / __| '__| | '_ \\ / _ \\\n"
          "  ___) | (__| |  | | |_) |  __/\n"
          " |____/ \\___|_|  |_|_.__/ \\___|\n"
          "\n",
          stdout);
    printf("scribe server %s\n", SCRIBE_VERSION);
    printf("listening on %s\n", listen_addr);
    printf("repository %s\n", repo == NULL ? "unknown" : repo);
    printf("main %s\n", main_ref);
    printf("transport %s\n", transport);
    printf("tls %s\n", tls);
    printf("query_api /scribe/v1\n");
    printf(
        "capabilities info refs log show diff cat-object ls-tree ingest content ingest-stream checkpoints replicate\n");
    printf("log %s\n", log_path == NULL ? "scribe_server.log" : log_path);
    printf("mode blocking single-connection server; repository opened read-only; store mutations are served through "
           "this process\n\n");
    fflush(stdout);

    free(repo);
    free(log_path);
}

scribe_error_t scribe_cli_serve(scribe_ctx *ctx, const char *listen_addr, const char *cert, const char *key) {
    int fd = -1;
    int use_tls = (cert != NULL && cert[0] != '\0') || (key != NULL && key[0] != '\0');
#ifdef SCRIBE_HAVE_TLS
    SSL_CTX *ssl_ctx = NULL;
#endif
    struct sigaction sa;
    scribe_error_t err;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = serve_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    g_serve_stop = 0;
    err = listen_socket(listen_addr, &fd);
    if (err != SCRIBE_OK) {
        return err;
    }
#ifdef SCRIBE_HAVE_TLS
    if (use_tls) {
        err = create_server_tls_ctx(cert, key, &ssl_ctx);
        if (err != SCRIBE_OK) {
            close(fd);
            return err;
        }
    }
#else
    if (use_tls) {
        close(fd);
        return scribe_set_error(SCRIBE_ENOSYS, "TLS/HTTP2 transport is not enabled in this build");
    }
#endif
    print_serve_banner(ctx, listen_addr, cert, key);
    while (!g_serve_stop) {
        int client_fd = accept(fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = scribe_set_error(SCRIBE_EIO, "accept failed");
            break;
        }
#ifdef SCRIBE_HAVE_TLS
        if (use_tls) {
            (void)serve_one_tls_h2(ctx, ssl_ctx, client_fd);
        } else
#endif
        {
            (void)serve_one(ctx, client_fd);
        }
        close(client_fd);
    }
#ifdef SCRIBE_HAVE_TLS
    SSL_CTX_free(ssl_ctx);
#endif
    close(fd);
    return err;
}
