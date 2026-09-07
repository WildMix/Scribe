/*
 * Minimal linenoise-compatible shim for Scribe.
 *
 * This implements the subset used by the Scribe REPL without requiring a system
 * dependency: raw terminal input, cursor movement, up/down history, simple tab
 * completion, and a stdio fallback for piped integration tests.
 */
#include "linenoise.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/types.h>
#include <unistd.h>

static linenoiseCompletionCallback *g_completion_callback = NULL;
static char **g_history = NULL;
static int g_history_len = 0;
static int g_history_cap = 0;
static int g_history_max = 100;

static char *linenoise_getline(const char *prompt) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    if (prompt != NULL) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        return NULL;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[n - 1] = '\0';
        n--;
    }
    return line;
}

static int enable_raw_mode(struct termios *orig) {
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, orig) != 0) {
        return 0;
    }
    raw = *orig;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
}

static void refresh_line(const char *prompt, const char *buf, size_t len, size_t pos) {
    fputc('\r', stdout);
    if (prompt != NULL) {
        fputs(prompt, stdout);
    }
    fputs(buf == NULL ? "" : buf, stdout);
    fputs("\x1b[K", stdout);
    if (pos < len) {
        fprintf(stdout, "\x1b[%zuD", len - pos);
    }
    fflush(stdout);
}

static int ensure_line_cap(char **buf, size_t *cap, size_t need) {
    char *grown;
    size_t new_cap = *cap == 0u ? 64u : *cap;

    while (new_cap < need) {
        new_cap *= 2u;
    }
    if (new_cap == *cap) {
        return 1;
    }
    grown = (char *)realloc(*buf, new_cap);
    if (grown == NULL) {
        return 0;
    }
    *buf = grown;
    *cap = new_cap;
    return 1;
}

static int replace_line(char **buf, size_t *len, size_t *pos, size_t *cap, const char *text) {
    size_t text_len = text == NULL ? 0u : strlen(text);

    if (!ensure_line_cap(buf, cap, text_len + 1u)) {
        return 0;
    }
    if (text_len != 0u) {
        memcpy(*buf, text, text_len);
    }
    (*buf)[text_len] = '\0';
    *len = text_len;
    *pos = text_len;
    return 1;
}

static void delete_before_cursor(char *buf, size_t *len, size_t *pos) {
    if (*pos == 0u) {
        return;
    }
    memmove(buf + *pos - 1u, buf + *pos, *len - *pos + 1u);
    (*pos)--;
    (*len)--;
}

static void delete_at_cursor(char *buf, size_t *len, size_t *pos) {
    if (*pos >= *len) {
        return;
    }
    memmove(buf + *pos, buf + *pos + 1u, *len - *pos);
    (*len)--;
}

static int insert_at_cursor(char **buf, size_t *len, size_t *pos, size_t *cap, char c) {
    if (!ensure_line_cap(buf, cap, *len + 2u)) {
        return 0;
    }
    memmove(*buf + *pos + 1u, *buf + *pos, *len - *pos + 1u);
    (*buf)[*pos] = c;
    (*pos)++;
    (*len)++;
    return 1;
}

static int is_word_char(unsigned char ch) { return isalnum(ch) || ch == '_'; }

static size_t previous_word_pos(const char *buf, size_t pos) {
    while (pos > 0u && !is_word_char((unsigned char)buf[pos - 1u])) {
        pos--;
    }
    while (pos > 0u && is_word_char((unsigned char)buf[pos - 1u])) {
        pos--;
    }
    return pos;
}

static size_t next_word_pos(const char *buf, size_t len, size_t pos) {
    while (pos < len && is_word_char((unsigned char)buf[pos])) {
        pos++;
    }
    while (pos < len && !is_word_char((unsigned char)buf[pos])) {
        pos++;
    }
    return pos;
}

static int read_escape_sequence(char *params, size_t params_size, char *final) {
    unsigned char c;
    size_t len = 0;

    if (params_size == 0u || read(STDIN_FILENO, &c, 1) != 1 || c != '[') {
        return 0;
    }
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c >= 0x40u && c <= 0x7eu) {
            params[len] = '\0';
            *final = (char)c;
            return 1;
        }
        if (len + 1u >= params_size) {
            return 0;
        }
        params[len++] = (char)c;
    }
    return 0;
}

static int escape_has_ctrl_modifier(const char *params) {
    const char *p = params == NULL ? "" : params;

    if (strcmp(p, "5") == 0) {
        return 1;
    }
    while ((p = strstr(p, ";5")) != NULL) {
        char next = p[2];

        if (next == '\0' || next == ';' || next == ':') {
            return 1;
        }
        p += 2;
    }
    return 0;
}

static void free_completions(linenoiseCompletions *lc) {
    unsigned int i;

    for (i = 0; i < lc->len; i++) {
        free(lc->cvec[i]);
    }
    free(lc->cvec);
}

char *linenoise(const char *prompt) {
    struct termios orig;
    char *buf = NULL;
    char *snapshot = NULL;
    size_t len = 0;
    size_t pos = 0;
    size_t cap = 0;
    int history_index = g_history_len;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || !enable_raw_mode(&orig)) {
        return linenoise_getline(prompt);
    }
    if (!replace_line(&buf, &len, &pos, &cap, "")) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
        return NULL;
    }
    if (prompt != NULL) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    for (;;) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            free(snapshot);
            (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            return NULL;
        }
        if (n == 0 || (c == 4u && len == 0u)) {
            free(buf);
            free(snapshot);
            (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            return NULL;
        }
        if (c == '\r' || c == '\n') {
            fputs("\r\n", stdout);
            break;
        }
        if (c == 1u) {
            pos = 0;
            refresh_line(prompt, buf, len, pos);
            continue;
        }
        if (c == 5u) {
            pos = len;
            refresh_line(prompt, buf, len, pos);
            continue;
        }
        if (c == 4u) {
            if (pos < len) {
                delete_at_cursor(buf, &len, &pos);
                history_index = g_history_len;
                refresh_line(prompt, buf, len, pos);
            }
            continue;
        }
        if (c == 127u || c == 8u) {
            if (pos != 0u) {
                delete_before_cursor(buf, &len, &pos);
                history_index = g_history_len;
                refresh_line(prompt, buf, len, pos);
            }
            continue;
        }
        if (c == '\t') {
            linenoiseCompletions lc;

            memset(&lc, 0, sizeof(lc));
            if (g_completion_callback != NULL) {
                g_completion_callback(buf, &lc);
            }
            if (lc.len == 1u) {
                if (replace_line(&buf, &len, &pos, &cap, lc.cvec[0])) {
                    refresh_line(prompt, buf, len, pos);
                }
            } else if (lc.len > 1u) {
                unsigned int i;

                fputs("\r\n", stdout);
                for (i = 0; i < lc.len; i++) {
                    fputs(lc.cvec[i], stdout);
                    fputc('\n', stdout);
                }
                refresh_line(prompt, buf, len, pos);
            }
            free_completions(&lc);
            continue;
        }
        if (c == 27u) {
            char params[32];
            char final = '\0';

            if (!read_escape_sequence(params, sizeof(params), &final)) {
                continue;
            }
            if (final == 'A' && g_history_len > 0 && history_index > 0) {
                if (history_index == g_history_len) {
                    free(snapshot);
                    snapshot = strdup(buf);
                }
                history_index--;
                if (replace_line(&buf, &len, &pos, &cap, g_history[history_index])) {
                    refresh_line(prompt, buf, len, pos);
                }
            } else if (final == 'B' && history_index < g_history_len) {
                history_index++;
                if (replace_line(&buf, &len, &pos, &cap,
                                 history_index == g_history_len ? snapshot : g_history[history_index])) {
                    refresh_line(prompt, buf, len, pos);
                }
            } else if (final == 'C') {
                if (escape_has_ctrl_modifier(params)) {
                    size_t new_pos = next_word_pos(buf, len, pos);

                    if (new_pos != pos) {
                        pos = new_pos;
                        refresh_line(prompt, buf, len, pos);
                    }
                } else if (pos < len) {
                    pos++;
                    fputs("\x1b[C", stdout);
                    fflush(stdout);
                }
            } else if (final == 'D') {
                if (escape_has_ctrl_modifier(params)) {
                    size_t new_pos = previous_word_pos(buf, pos);

                    if (new_pos != pos) {
                        pos = new_pos;
                        refresh_line(prompt, buf, len, pos);
                    }
                } else if (pos > 0u) {
                    pos--;
                    fputs("\x1b[D", stdout);
                    fflush(stdout);
                }
            } else if (final == 'H') {
                pos = 0;
                refresh_line(prompt, buf, len, pos);
            } else if (final == 'F') {
                pos = len;
                refresh_line(prompt, buf, len, pos);
            } else if (final == '~') {
                if (strcmp(params, "3") == 0 && pos < len) {
                    delete_at_cursor(buf, &len, &pos);
                    history_index = g_history_len;
                    refresh_line(prompt, buf, len, pos);
                }
            }
            continue;
        }
        if (c < 32u) {
            continue;
        }
        if (!insert_at_cursor(&buf, &len, &pos, &cap, (char)c)) {
            free(buf);
            free(snapshot);
            (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            return NULL;
        }
        history_index = g_history_len;
        refresh_line(prompt, buf, len, pos);
    }
    free(snapshot);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    return buf;
}

void linenoiseFree(void *ptr) { free(ptr); }

int linenoiseHistorySetMaxLen(int len) {
    if (len <= 0) {
        return 0;
    }
    g_history_max = len;
    while (g_history_len > g_history_max) {
        free(g_history[0]);
        memmove(g_history, g_history + 1, sizeof(*g_history) * (size_t)(g_history_len - 1));
        g_history_len--;
    }
    return 1;
}

int linenoiseHistoryAdd(const char *line) {
    char **grown;

    if (line == NULL || line[0] == '\0') {
        return 0;
    }
    if (g_history_len == g_history_max && g_history_len > 0) {
        free(g_history[0]);
        memmove(g_history, g_history + 1, sizeof(*g_history) * (size_t)(g_history_len - 1));
        g_history_len--;
    }
    if (g_history_len == g_history_cap) {
        int new_cap = g_history_cap == 0 ? 16 : g_history_cap * 2;

        grown = (char **)realloc(g_history, sizeof(*g_history) * (size_t)new_cap);
        if (grown == NULL) {
            return 0;
        }
        g_history = grown;
        g_history_cap = new_cap;
    }
    g_history[g_history_len] = strdup(line);
    if (g_history[g_history_len] == NULL) {
        return 0;
    }
    g_history_len++;
    return 1;
}

int linenoiseHistoryLoad(const char *filename) {
    FILE *f;
    char *line = NULL;
    size_t cap = 0;

    if (filename == NULL) {
        return 0;
    }
    f = fopen(filename, "r");
    if (f == NULL) {
        return 0;
    }
    while (getline(&line, &cap, f) >= 0) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) {
            line[--len] = '\0';
        }
        (void)linenoiseHistoryAdd(line);
    }
    free(line);
    fclose(f);
    return 1;
}

int linenoiseHistorySave(const char *filename) {
    FILE *f;
    int i;

    if (filename == NULL) {
        return 0;
    }
    f = fopen(filename, "w");
    if (f == NULL) {
        return 0;
    }
    for (i = 0; i < g_history_len; i++) {
        fprintf(f, "%s\n", g_history[i]);
    }
    fclose(f);
    return 1;
}

void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) { g_completion_callback = fn; }

void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str) {
    char **grown;

    if (lc == NULL || str == NULL) {
        return;
    }
    grown = (char **)realloc(lc->cvec, sizeof(*lc->cvec) * (size_t)(lc->len + 1u));
    if (grown == NULL) {
        return;
    }
    lc->cvec = grown;
    lc->cvec[lc->len] = strdup(str);
    if (lc->cvec[lc->len] != NULL) {
        lc->len++;
    }
}

void linenoiseShimTouchCompletion(const char *line) {
    linenoiseCompletions lc;
    unsigned int i;

    memset(&lc, 0, sizeof(lc));
    if (g_completion_callback != NULL) {
        g_completion_callback(line == NULL ? "" : line, &lc);
    }
    for (i = 0; i < lc.len; i++) {
        free(lc.cvec[i]);
    }
    free(lc.cvec);
}
