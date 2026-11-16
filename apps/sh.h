/* The PalanuOS shell: a small sh-like command language on top of the process, TTY, and VFS APIs. */
#ifndef PAL_SH_H
#define PAL_SH_H

#include "libapp.h"

#define SH_LINE_MAX 200
#define SH_HIST_MAX 50 /* this session's up-arrow list; pal/history.h keeps more across restarts */
#define SH_VARS_MAX 16
#define SH_ALIAS_MAX 8
#define SH_COMP_MAX 24

typedef struct {
    char name[NEMA_NAME_MAX + 2]; /* directories end with '/' */
} sh_cand_t;

typedef struct {
    int start;      /* offset in the line where the word being completed begins */
    int n;          /* number of candidates */
    bool is_command;
    sh_cand_t c[SH_COMP_MAX];
} sh_comp_t;

typedef struct {
    struct {
        char name[16], val[64];
    } var[SH_VARS_MAX];
    struct {
        char name[16], val[64];
    } alias[SH_ALIAS_MAX];
    char *hist[SH_HIST_MAX];
    int nhist;
    int status;
    nema_pid_t pgid;
    bool interactive, exiting;
    int cols;
    sh_comp_t comp;
    nema_owner_t owner;
} sh_t;

/* sh_edit.c */
int sh_readline(sh_t *sh, const char *prompt, char *out, size_t cap); /* length; -1 at end of input */
void sh_hist_add(sh_t *sh, const char *line);

/* sh.c */
void sh_complete(sh_t *sh, const char *line, int cur, sh_comp_t *out);
const char *sh_var(sh_t *sh, const char *name);

#endif
