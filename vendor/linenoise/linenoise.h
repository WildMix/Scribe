#ifndef SCRIBE_VENDOR_LINENOISE_H
#define SCRIBE_VENDOR_LINENOISE_H

typedef struct linenoiseCompletions {
    unsigned int len;
    char **cvec;
} linenoiseCompletions;

typedef void(linenoiseCompletionCallback)(const char *, linenoiseCompletions *);

char *linenoise(const char *prompt);
void linenoiseFree(void *ptr);
int linenoiseHistoryAdd(const char *line);
int linenoiseHistorySetMaxLen(int len);
int linenoiseHistoryLoad(const char *filename);
int linenoiseHistorySave(const char *filename);
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn);
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str);

#endif
