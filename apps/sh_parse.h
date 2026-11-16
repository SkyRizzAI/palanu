/* Shell parsing: tokens, commands, and pipelines. Split out of sh.c so host tests can drive the parser directly
 * without a process. The behaviour is the shell's own; nothing here is a second implementation. */
#ifndef PAL_SH_PARSE_H
#define PAL_SH_PARSE_H

#include "sh.h"

enum { T_WORD, T_PIPE, T_OR, T_AND, T_SEMI, T_BG, T_IN, T_OUT, T_APP, T_ERR, T_ERR2OUT };

#define TOK_MAX 48
#define POOL_MAX 1100
#define ARGS_MAX 24
#define STAGES_MAX 4

typedef struct {
    int kind;
    char *s;
    bool glob;
    const char *end; /* where in the source line this token stopped: the rest of the line is re-tokenized from here */
} tok_t;

typedef struct {
    tok_t t[TOK_MAX];
    int n;
    char pool[POOL_MAX];
    size_t used;
    const char *error;
} toks_t;

typedef struct {
    char *argv[ARGS_MAX];
    int argc;
    char *in, *out, *err;
    bool append, err_to_out;
} cmd_t;

/* Split a line into tokens. Quotes, escapes, $VAR, $?, and ~ are resolved here. */
void sh_tokenize(sh_t *sh, const char *line, toks_t *k);
/* Parse one pipeline from tokens at *i (advancing it past the pipeline, not past the operator that ends it).
 * Returns 0 and fills cmds, nc, and bg; returns -1 on a syntax error. gstore holds glob and alias text. */
int sh_parse_pipeline(sh_t *sh, toks_t *k, int *i, cmd_t cmds[STAGES_MAX], int *nc, bool *bg, char *gstore,
                      size_t *gused, size_t gcap);
/* Parse and run one line: pipelines joined by ; && || and an optional trailing &. Returns the exit status. */
int sh_run_line(sh_t *sh, const char *line);

#endif
