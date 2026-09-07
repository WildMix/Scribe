/*
 * Minimal M7 daemon supervisor.
 *
 * The daemon intentionally stays small: it reads a line-oriented config file,
 * optionally starts MongoDB adapter child processes, and exposes a local HTTP
 * status endpoint. It is not a multi-writer coordinator; each watched
 * repository is still protected by its own `.scribe/lock` in the child process.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    DAEMON_REPO = 0,
    DAEMON_WATCH = 1,
} daemon_entry_kind;

typedef struct {
    char *name;
    char *store;
    char *uri;
    daemon_entry_kind kind;
    pid_t pid;
    int restart_count;
} daemon_entry;

typedef struct {
    char *listen;
    daemon_entry *entries;
    size_t count;
    size_t cap;
} daemon_config;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} text_buf;

#define APPEND_LIT(buf, lit) text_append((buf), (lit), sizeof(lit) - 1u)

static volatile sig_atomic_t g_daemon_stop = 0;

static void daemon_signal(int signo) {
    (void)signo;
    g_daemon_stop = 1;
}

static scribe_error_t text_append(text_buf *buf, const char *data, size_t len) {
    char *grown;
    size_t new_cap;

    if (len == 0u) {
        return SCRIBE_OK;
    }
    if (buf->len > SIZE_MAX - len - 1u) {
        return scribe_set_error(SCRIBE_ENOMEM, "daemon response is too large");
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
            return scribe_set_error(SCRIBE_ENOMEM, "daemon response is too large");
        }
        new_cap *= 2u;
    }
    grown = (char *)realloc(buf->data, new_cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow daemon response");
    }
    buf->data = grown;
    buf->cap = new_cap;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return SCRIBE_OK;
}

static scribe_error_t text_appendf(text_buf *buf, const char *fmt, ...) {
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
        return scribe_set_error(SCRIBE_EIO, "failed to format daemon response");
    }
    if ((size_t)n < sizeof(stack)) {
        va_end(ap2);
        return text_append(buf, stack, (size_t)n);
    }
    heap = (char *)malloc((size_t)n + 1u);
    if (heap == NULL) {
        va_end(ap2);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate daemon response");
    }
    (void)vsnprintf(heap, (size_t)n + 1u, fmt, ap2);
    va_end(ap2);
    err = text_append(buf, heap, (size_t)n);
    free(heap);
    return err;
}

static void text_destroy(text_buf *buf) {
    if (buf != NULL) {
        free(buf->data);
        memset(buf, 0, sizeof(*buf));
    }
}

static void json_string(text_buf *buf, const char *s) {
    const unsigned char *p = (const unsigned char *)(s == NULL ? "" : s);

    (void)APPEND_LIT(buf, "\"");
    while (*p != '\0') {
        char esc[8];
        switch (*p) {
        case '"':
            (void)APPEND_LIT(buf, "\\\"");
            break;
        case '\\':
            (void)APPEND_LIT(buf, "\\\\");
            break;
        case '\n':
            (void)APPEND_LIT(buf, "\\n");
            break;
        case '\r':
            (void)APPEND_LIT(buf, "\\r");
            break;
        case '\t':
            (void)APPEND_LIT(buf, "\\t");
            break;
        default:
            if (*p < 0x20u) {
                (void)snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
                (void)text_append(buf, esc, strlen(esc));
            } else {
                (void)text_append(buf, (const char *)p, 1u);
            }
            break;
        }
        p++;
    }
    (void)APPEND_LIT(buf, "\"");
}

static char *trim(char *s) {
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static int daemon_name_valid(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0') {
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

static scribe_error_t daemon_config_add(daemon_config *cfg, daemon_entry_kind kind, const char *name, const char *store,
                                        const char *uri) {
    daemon_entry *grown;
    size_t new_cap;

    if (!daemon_name_valid(name)) {
        return scribe_set_error(SCRIBE_ECONFIG, "invalid daemon repository name '%s'", name == NULL ? "" : name);
    }
    if (store == NULL || store[0] == '\0') {
        return scribe_set_error(SCRIBE_ECONFIG, "daemon repository store is required");
    }
    if (kind == DAEMON_WATCH && (uri == NULL || uri[0] == '\0')) {
        return scribe_set_error(SCRIBE_ECONFIG, "daemon watch uri is required");
    }
    if (cfg->count == cfg->cap) {
        new_cap = cfg->cap == 0u ? 4u : cfg->cap * 2u;
        grown = (daemon_entry *)realloc(cfg->entries, sizeof(*grown) * new_cap);
        if (grown == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow daemon config");
        }
        memset(grown + cfg->cap, 0, sizeof(*grown) * (new_cap - cfg->cap));
        cfg->entries = grown;
        cfg->cap = new_cap;
    }
    cfg->entries[cfg->count].kind = kind;
    cfg->entries[cfg->count].name = strdup(name);
    cfg->entries[cfg->count].store = strdup(store);
    cfg->entries[cfg->count].uri = uri == NULL ? NULL : strdup(uri);
    if (cfg->entries[cfg->count].name == NULL || cfg->entries[cfg->count].store == NULL ||
        (uri != NULL && cfg->entries[cfg->count].uri == NULL)) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate daemon config entry");
    }
    cfg->count++;
    return SCRIBE_OK;
}

static void daemon_config_destroy(daemon_config *cfg) {
    size_t i;

    if (cfg == NULL) {
        return;
    }
    for (i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].name);
        free(cfg->entries[i].store);
        free(cfg->entries[i].uri);
    }
    free(cfg->entries);
    free(cfg->listen);
    memset(cfg, 0, sizeof(*cfg));
}

static scribe_error_t parse_daemon_config(const char *path, daemon_config *cfg) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    char *line;
    char *save = NULL;
    scribe_error_t err;

    memset(cfg, 0, sizeof(*cfg));
    err = scribe_read_file(path, &bytes, &len);
    if (err != SCRIBE_OK) {
        return err;
    }
    (void)len;
    for (line = strtok_r((char *)bytes, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char *parts[4];
        char *cursor;
        size_t count = 0;

        cursor = trim(line);
        if (cursor[0] == '\0' || cursor[0] == '#') {
            continue;
        }
        while (count < 4u && cursor != NULL) {
            parts[count++] = cursor;
            cursor = strchr(cursor, '\t');
            if (cursor != NULL) {
                *cursor++ = '\0';
            }
        }
        if (strcmp(parts[0], "listen") == 0 && count == 2u) {
            free(cfg->listen);
            cfg->listen = strdup(parts[1]);
            if (cfg->listen == NULL) {
                err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate daemon listen address");
                break;
            }
        } else if (strcmp(parts[0], "repo") == 0 && count == 3u) {
            err = daemon_config_add(cfg, DAEMON_REPO, parts[1], parts[2], NULL);
            if (err != SCRIBE_OK) {
                break;
            }
        } else if (strcmp(parts[0], "watch") == 0 && count == 4u) {
            err = daemon_config_add(cfg, DAEMON_WATCH, parts[1], parts[2], parts[3]);
            if (err != SCRIBE_OK) {
                break;
            }
        } else {
            err = scribe_set_error(SCRIBE_ECONFIG,
                                   "daemon config lines are: listen<TAB>addr, repo<TAB>name<TAB>store, or "
                                   "watch<TAB>name<TAB>store<TAB>uri");
            break;
        }
    }
    free(bytes);
    if (err != SCRIBE_OK) {
        daemon_config_destroy(cfg);
        return err;
    }
    if (cfg->listen == NULL) {
        daemon_config_destroy(cfg);
        return scribe_set_error(SCRIBE_ECONFIG, "daemon config missing listen address");
    }
    return SCRIBE_OK;
}

static scribe_error_t split_addr(const char *listen, char **out_host, char **out_port) {
    const char *colon;
    size_t host_len;

    colon = strrchr(listen, ':');
    if (colon == NULL || colon[1] == '\0') {
        return scribe_set_error(SCRIBE_ECONFIG, "daemon listen address must be host:port");
    }
    host_len = (size_t)(colon - listen);
    *out_host = (char *)malloc(host_len + 1u);
    *out_port = strdup(colon + 1);
    if (*out_host == NULL || *out_port == NULL) {
        free(*out_host);
        free(*out_port);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate daemon listen address");
    }
    memcpy(*out_host, listen, host_len);
    (*out_host)[host_len] = '\0';
    return SCRIBE_OK;
}

static scribe_error_t daemon_listen(const char *listen_addr, int *out_fd) {
    char *host = NULL;
    char *port = NULL;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int fd = -1;
    int one = 1;
    int rc;
    scribe_error_t err = split_addr(listen_addr, &host, &port);

    if (err != SCRIBE_OK) {
        return err;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    rc = getaddrinfo(host, port, &hints, &res);
    free(host);
    free(port);
    if (rc != 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to resolve daemon listen address: %s", gai_strerror(rc));
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
    if (fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to listen on %s", listen_addr);
    }
    *out_fd = fd;
    return SCRIBE_OK;
}

static scribe_error_t write_all(int fd, const char *data, size_t len) {
    while (len != 0u) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return scribe_set_error(SCRIBE_EIO, "daemon socket write failed");
        }
        if (n == 0) {
            return scribe_set_error(SCRIBE_EIO, "daemon socket write made no progress");
        }
        data += (size_t)n;
        len -= (size_t)n;
    }
    return SCRIBE_OK;
}

static scribe_error_t send_http(int fd, int status, const char *text, const char *body, const char *content_type) {
    char header[256];
    int n;
    size_t body_len = strlen(body);

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", status, text,
                 body_len, content_type);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        return scribe_set_error(SCRIBE_EIO, "failed to format daemon response header");
    }
    if (write_all(fd, header, (size_t)n) != SCRIBE_OK) {
        return SCRIBE_EIO;
    }
    return write_all(fd, body, body_len);
}

static int child_running(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }
    return kill(pid, 0) == 0 || errno == EPERM;
}

static char *daemon_adapter_executable(const char *self_path, const char *name) {
    const char *configured = getenv("SCRIBE_MONGO_BIN");
    const char *slash;
    size_t dir_len;
    size_t name_len;
    char *path;

    if (configured != NULL && configured[0] != '\0') {
        return strdup(configured);
    }
    if (self_path == NULL || self_path[0] == '\0') {
        return strdup(name);
    }
    slash = strrchr(self_path, '/');
    if (slash == NULL) {
        return strdup(name);
    }
    dir_len = (size_t)(slash - self_path) + 1u;
    name_len = strlen(name);
    path = (char *)malloc(dir_len + name_len + 1u);
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, self_path, dir_len);
    memcpy(path + dir_len, name, name_len + 1u);
    return path;
}

static void reap_and_restart(daemon_config *cfg, const char *self_path) {
    size_t i;

    for (i = 0; i < cfg->count; i++) {
        daemon_entry *entry = &cfg->entries[i];
        int status = 0;

        if (entry->kind != DAEMON_WATCH) {
            continue;
        }
        if (entry->pid > 0 && waitpid(entry->pid, &status, WNOHANG) == 0) {
            continue;
        }
        if (entry->pid > 0) {
            entry->pid = 0;
            entry->restart_count++;
        }
        if (g_daemon_stop) {
            continue;
        }
        entry->pid = fork();
        if (entry->pid == 0) {
            char *adapter = daemon_adapter_executable(self_path, "scribe-mongo");

            if (adapter != NULL) {
                execl(adapter, adapter, "--store", entry->store, "watch", entry->uri, (char *)NULL);
            }
            _exit(127);
        }
        if (entry->pid < 0) {
            entry->pid = 0;
        }
    }
}

static scribe_error_t append_repo_status(text_buf *body, const daemon_entry *entry) {
    scribe_ctx *ctx = NULL;
    uint8_t head[SCRIBE_HASH_SIZE];
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    json_string(body, entry->name);
    (void)text_appendf(body, ",\"kind\":\"%s\",\"store\":", entry->kind == DAEMON_WATCH ? "watch" : "repo");
    json_string(body, entry->store);
    err = scribe_open(entry->store, 0, &ctx);
    if (err == SCRIBE_OK) {
        if (scribe_refs_read(ctx, "refs/heads/main", head) == SCRIBE_OK) {
            scribe_hash_to_hex(head, hex);
            (void)APPEND_LIT(body, ",\"head\":");
            json_string(body, hex);
        } else {
            (void)APPEND_LIT(body, ",\"head\":null");
        }
        scribe_close(ctx);
        (void)APPEND_LIT(body, ",\"open\":true");
    } else {
        (void)APPEND_LIT(body, ",\"head\":null,\"open\":false,\"error\":");
        json_string(body, scribe_error_symbol(err));
    }
    if (entry->kind == DAEMON_WATCH) {
        (void)text_appendf(body, ",\"child_pid\":%ld,\"child_running\":%s,\"restart_count\":%d", (long)entry->pid,
                           child_running(entry->pid) ? "true" : "false", entry->restart_count);
    }
    return SCRIBE_OK;
}

static scribe_error_t status_body(daemon_config *cfg, text_buf *body) {
    size_t i;

    (void)APPEND_LIT(body, "{\"status\":\"ok\",\"repositories\":[");
    for (i = 0; i < cfg->count; i++) {
        if (i != 0u) {
            (void)APPEND_LIT(body, ",");
        }
        (void)APPEND_LIT(body, "{\"name\":");
        (void)append_repo_status(body, &cfg->entries[i]);
        (void)APPEND_LIT(body, "}");
    }
    return APPEND_LIT(body, "]}\n");
}

static void serve_status_client(int fd, daemon_config *cfg) {
    char req[1024];
    ssize_t n;
    text_buf body = {0};

    n = recv(fd, req, sizeof(req) - 1u, 0);
    if (n <= 0) {
        return;
    }
    req[n] = '\0';
    if (strncmp(req, "GET /scribe/v1/status ", 22u) != 0 && strncmp(req, "GET /status ", 12u) != 0) {
        (void)send_http(fd, 404, "Not Found", "ERR SCRIBE_ENOT_FOUND endpoint not found\n", "text/plain");
        return;
    }
    if (status_body(cfg, &body) == SCRIBE_OK) {
        (void)send_http(fd, 200, "OK", body.data == NULL ? "{}\n" : body.data, "application/json");
    } else {
        (void)send_http(fd, 500, "Internal Server Error", "ERR SCRIBE_ERR status failed\n", "text/plain");
    }
    text_destroy(&body);
}

static void stop_children(daemon_config *cfg) {
    size_t i;

    for (i = 0; i < cfg->count; i++) {
        if (cfg->entries[i].kind == DAEMON_WATCH && cfg->entries[i].pid > 0) {
            kill(cfg->entries[i].pid, SIGTERM);
        }
    }
    for (i = 0; i < cfg->count; i++) {
        if (cfg->entries[i].kind == DAEMON_WATCH && cfg->entries[i].pid > 0) {
            (void)waitpid(cfg->entries[i].pid, NULL, 0);
            cfg->entries[i].pid = 0;
        }
    }
}

scribe_error_t scribe_cli_daemon(const char *config_path, const char *self_path) {
    daemon_config cfg;
    struct sigaction sa;
    int fd = -1;
    scribe_error_t err;

    if (config_path == NULL || config_path[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "daemon config path is required");
    }
    err = parse_daemon_config(config_path, &cfg);
    if (err != SCRIBE_OK) {
        return err;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    err = daemon_listen(cfg.listen, &fd);
    if (err != SCRIBE_OK) {
        daemon_config_destroy(&cfg);
        return err;
    }
    printf("daemon listening on %s\n", cfg.listen);
    fflush(stdout);
    while (!g_daemon_stop) {
        fd_set set;
        struct timeval tv;
        int rc;

        reap_and_restart(&cfg, self_path == NULL ? "scribe" : self_path);
        FD_ZERO(&set);
        FD_SET(fd, &set);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        rc = select(fd + 1, &set, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = scribe_set_error(SCRIBE_EIO, "daemon select failed");
            break;
        }
        if (rc > 0 && FD_ISSET(fd, &set)) {
            int client = accept(fd, NULL, NULL);
            if (client >= 0) {
                serve_status_client(client, &cfg);
                close(client);
            }
        }
    }
    stop_children(&cfg);
    close(fd);
    daemon_config_destroy(&cfg);
    return err;
}
