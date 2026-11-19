#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal/history.h"
#include "sh_parse.h"

/* ------------------------------------------------------------------ variables and aliases */
const char *sh_var(sh_t *sh, const char *name) {
    for (int i = 0; i < SH_VARS_MAX; i++)
        if (sh->var[i].name[0] && strcmp(sh->var[i].name, name) == 0) return sh->var[i].val;
    return NULL;
}

static void set_var(sh_t *sh, const char *name, const char *val) {
    int free_slot = -1;
    for (int i = 0; i < SH_VARS_MAX; i++) {
        if (sh->var[i].name[0] && strcmp(sh->var[i].name, name) == 0) {
            nema_strlcpy(sh->var[i].val, val, sizeof(sh->var[i].val));
            return;
        }
        if (!sh->var[i].name[0] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        nema_strlcpy(sh->var[free_slot].name, name, sizeof(sh->var[0].name));
        nema_strlcpy(sh->var[free_slot].val, val, sizeof(sh->var[0].val));
    }
}

static void unset_var(sh_t *sh, const char *name) {
    for (int i = 0; i < SH_VARS_MAX; i++)
        if (strcmp(sh->var[i].name, name) == 0) sh->var[i].name[0] = 0;
}

static const char *alias_of(sh_t *sh, const char *name) {
    for (int i = 0; i < SH_ALIAS_MAX; i++)
        if (sh->alias[i].name[0] && strcmp(sh->alias[i].name, name) == 0) return sh->alias[i].val;
    return NULL;
}

/* ------------------------------------------------------------------ tokenizer */
static void add_tok(toks_t *k, int kind, char *s, bool glob) {
    if (k->n == TOK_MAX) {
        k->error = "too many words";
        return;
    }
    k->t[k->n].kind = kind;
    k->t[k->n].s = s;
    k->t[k->n].glob = glob;
    k->n++;
}

static int expand_var(sh_t *sh, const char *s, char *dst, size_t cap, int *consumed) {
    char name[16];
    int i = 1; /* s[0] == '$' */
    if (s[i] == '?') {
        *consumed = 2;
        return snprintf(dst, cap, "%d", sh->status);
    }
    bool brace = s[i] == '{';
    if (brace) i++;
    int n = 0;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_' || (n && s[i] >= '0' && s[i] <= '9')) {
        if (n < 15) name[n++] = s[i];
        i++;
    }
    name[n] = 0;
    if (brace && s[i] == '}') i++;
    if (n == 0) {
        *consumed = 1;
        return snprintf(dst, cap, "$");
    }
    *consumed = i;
    const char *v = sh_var(sh, name);
    if (!v && strcmp(name, "PWD") == 0) v = nema_self() ? nema_self()->cwd : "/";
    return snprintf(dst, cap, "%s", v ? v : "");
}

void sh_tokenize(sh_t *sh, const char *line, toks_t *k) {
    memset(k, 0, sizeof(*k));
    const char *p = line;
    while (*p) {
        if (k->n) k->t[k->n - 1].end = p;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        if (*p == '|') { if (p[1] == '|') { add_tok(k, T_OR, NULL, false); p += 2; } else { add_tok(k, T_PIPE, NULL, false); p++; } continue; }
        if (*p == '&') { if (p[1] == '&') { add_tok(k, T_AND, NULL, false); p += 2; } else { add_tok(k, T_BG, NULL, false); p++; } continue; }
        if (*p == ';') { add_tok(k, T_SEMI, NULL, false); p++; continue; }
        if (*p == '<') { add_tok(k, T_IN, NULL, false); p++; continue; }
        if (*p == '>') { if (p[1] == '>') { add_tok(k, T_APP, NULL, false); p += 2; } else { add_tok(k, T_OUT, NULL, false); p++; } continue; }
        if (*p == '2' && p[1] == '>' && (p == line || p[-1] == ' ')) {
            if (p[2] == '&' && p[3] == '1') { add_tok(k, T_ERR2OUT, NULL, false); p += 4; }
            else { add_tok(k, T_ERR, NULL, false); p += 2; }
            continue;
        }
        /* a word: quotes, escapes, $VAR, and ~ are resolved here */
        char *w = k->pool + k->used;
        size_t wl = 0;
        bool glob = false, first = true;
        while (*p && *p != ' ' && *p != '\t' && !strchr("|&;<>", *p)) {
            if (k->used + wl + 70 > POOL_MAX) { k->error = "line too long"; return; }
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') w[wl++] = *p++;
                if (*p) p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) { w[wl++] = p[1]; p += 2; }
                    else if (*p == '$') { int c; wl += (size_t)expand_var(sh, p, w + wl, 64, &c); p += c; }
                    else w[wl++] = *p++;
                }
                if (*p) p++;
            } else if (*p == '\\' && p[1]) {
                w[wl++] = p[1];
                p += 2;
            } else if (*p == '$') {
                int c;
                wl += (size_t)expand_var(sh, p, w + wl, 64, &c);
                p += c;
            } else if (*p == '~' && first && (p[1] == 0 || p[1] == '/' || p[1] == ' ')) {
                const char *h = sh_var(sh, "HOME");
                wl += (size_t)snprintf(w + wl, 64, "%s", h ? h : "/");
                p++;
            } else {
                if (*p == '*' || *p == '?') glob = true;
                w[wl++] = *p++;
            }
            first = false;
        }
        w[wl++] = 0;
        k->used += wl;
        add_tok(k, T_WORD, w, glob);
        if (k->error) return;
    }
    if (k->n && !k->t[k->n - 1].end) k->t[k->n - 1].end = p;
}

/* ------------------------------------------------------------------ globbing */
static bool match(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (; ; s++) {
            if (match(pat + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (*s && (*pat == '?' || *pat == *s)) return match(pat + 1, s + 1);
    return false;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Expand the final path component of word into out[]. Returns the number of matches (0 = keep the word). */
static int glob_word(const char *word, char **out, int max, char *store, size_t store_cap) {
    char dir[NEMA_PATH_MAX], pat[NEMA_NAME_MAX];
    const char *slash = strrchr(word, '/');
    if (slash) {
        size_t dl = (size_t)(slash - word);
        if (dl == 0) dl = 1;
        if (dl >= sizeof(dir)) return 0;
        memcpy(dir, word, dl);
        dir[dl] = 0;
        nema_strlcpy(pat, slash + 1, sizeof(pat));
    } else {
        strcpy(dir, ".");
        nema_strlcpy(pat, word, sizeof(pat));
    }
    int n = 0;
    size_t used = 0;
    nema_dirent_t e;
    for (size_t i = 0; nema_fs_readdir(dir, i, &e) == NEMA_OK && n < max; i++) {
        if (e.name[0] == '.' && pat[0] != '.') continue;
        if (!match(pat, e.name)) continue;
        int l = slash ? snprintf(store + used, store_cap - used, "%.*s/%s", (int)(slash - word), word, e.name)
                      : snprintf(store + used, store_cap - used, "%s", e.name);
        if (l < 0 || used + (size_t)l + 1 > store_cap) break;
        out[n++] = store + used;
        used += (size_t)l + 1;
    }
    qsort(out, (size_t)n, sizeof(char *), cmp_str);
    return n;
}

/* ------------------------------------------------------------------ commands and pipelines */
static const char *BUILTINS[] = {"cd", "exit", "export", "unset", "alias", "unalias", "history", "source", "type", "help", "pwd", "clear", NULL};

static bool is_builtin(const char *name) {
    for (int i = 0; BUILTINS[i]; i++)
        if (strcmp(BUILTINS[i], name) == 0) return true;
    return false;
}

static int sh_source_fd(sh_t *sh, int fd) {
    size_t len;
    char *text = pal_read_all(fd, &len);
    if (!text) return 1;
    char *p = text, *end = text + len;
    int st = 0;
    while (p < end && !sh->exiting) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[SH_LINE_MAX];
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        memcpy(line, p, l);
        line[l] = 0;
        if (l && line[l - 1] == '\r') line[l - 1] = 0;
        st = sh_run_line(sh, line);
        p += l + (nl ? 1 : 0);
        if (!nl) break;
    }
    nema_free(text);
    return st;
}

static int builtin_run(sh_t *sh, int argc, char **argv) {
    const char *c = argv[0];
    if (strcmp(c, "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : (sh_var(sh, "HOME") ? sh_var(sh, "HOME") : "/");
        nema_err_t r = nema_set_cwd(dir);
        if (r != NEMA_OK) {
            pal_eprintf("cd: %s: %s\n", dir, pal_strerror(r));
            return 1;
        }
        return 0;
    }
    if (strcmp(c, "pwd") == 0) {
        pal_printf("%s\n", nema_self()->cwd);
        return 0;
    }
    if (strcmp(c, "exit") == 0) {
        sh->exiting = true;
        return argc > 1 ? atoi(argv[1]) : sh->status;
    }
    if (strcmp(c, "clear") == 0) {
        pal_puts("\x1b[2J\x1b[H");
        return 0;
    }
    if (strcmp(c, "export") == 0 || strcmp(c, "unset") == 0) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (strcmp(c, "unset") == 0) unset_var(sh, argv[i]);
            else if (eq) {
                *eq = 0;
                set_var(sh, argv[i], eq + 1);
            }
        }
        return 0;
    }
    if (strcmp(c, "alias") == 0) {
        if (argc == 1) {
            for (int i = 0; i < SH_ALIAS_MAX; i++)
                if (sh->alias[i].name[0]) pal_printf("alias %s='%s'\n", sh->alias[i].name, sh->alias[i].val);
            return 0;
        }
        char *eq = strchr(argv[1], '=');
        if (!eq) return 1;
        *eq = 0;
        for (int i = 0; i < SH_ALIAS_MAX; i++) {
            if (!sh->alias[i].name[0] || strcmp(sh->alias[i].name, argv[1]) == 0) {
                nema_strlcpy(sh->alias[i].name, argv[1], sizeof(sh->alias[i].name));
                nema_strlcpy(sh->alias[i].val, eq + 1, sizeof(sh->alias[i].val));
                return 0;
            }
        }
        return 1;
    }
    if (strcmp(c, "unalias") == 0) {
        for (int i = 0; i < SH_ALIAS_MAX && argc > 1; i++)
            if (strcmp(sh->alias[i].name, argv[1]) == 0) sh->alias[i].name[0] = 0;
        return 0;
    }
    if (strcmp(c, "history") == 0) {
        for (int i = 0; i < sh->nhist; i++) pal_printf("%3d  %s\n", i + 1, sh->hist[i]);
        return 0;
    }
    if (strcmp(c, "source") == 0) {
        if (argc < 2) return 1;
        int fd = nema_open(argv[1], NEMA_O_READ);
        if (fd < 0) {
            pal_eprintf("source: %s: %s\n", argv[1], pal_strerror(fd));
            return 1;
        }
        int r = sh_source_fd(sh, fd);
        nema_close(fd);
        return r;
    }
    if (strcmp(c, "type") == 0) {
        int st = 0;
        for (int i = 1; i < argc; i++) {
            if (is_builtin(argv[i])) pal_printf("%s is a shell builtin\n", argv[i]);
            else if (alias_of(sh, argv[i])) pal_printf("%s is aliased to '%s'\n", argv[i], alias_of(sh, argv[i]));
            else if (nema_app_find(argv[i])) pal_printf("%s is /bin/%s\n", argv[i], argv[i]);
            else { pal_eprintf("%s: not found\n", argv[i]); st = 1; }
        }
        return st;
    }
    if (strcmp(c, "help") == 0) {
        pal_puts("Palanu shell. Builtins:\n  ");
        for (int i = 0; BUILTINS[i]; i++) pal_printf("%s ", BUILTINS[i]);
        pal_puts("\n\nCommands in /bin:\n");
        for (size_t i = 0; i < nema_app_count(); i++) {
            const nema_app_t *a = nema_app_at(i);
            if (a->flags & NEMA_APP_HIDDEN) continue;
            pal_printf("  %-10s %s\n", a->name, a->help ? a->help : "");
        }
        pal_puts("\nTab completes, Up/Down browse history, Ctrl-C cancels, Ctrl-D exits.\n");
        return 0;
    }
    return 127;
}

static nema_obj_t *open_redirect(const char *path, uint32_t flags, const char *what) {
    nema_obj_t *o;
    nema_err_t r = nema_fs_open(path, flags, &o);
    if (r != NEMA_OK) {
        pal_eprintf("sh: %s: %s (%s)\n", path, pal_strerror(r), what);
        return NULL;
    }
    return o;
}

static void tty_mode(uint32_t mode) { nema_ioctl(0, NEMA_TIOC_SETMODE, &mode); }
static void tty_fg(nema_pid_t pgid) { nema_ioctl(0, NEMA_TIOC_SETFG, &pgid); }

static int run_pipeline(sh_t *sh, cmd_t *cmds, int n, bool bg) {
    if (n == 1 && !bg && is_builtin(cmds[0].argv[0])) return builtin_run(sh, cmds[0].argc, cmds[0].argv);

    nema_obj_t *prev_r = NULL;
    nema_pid_t pids[STAGES_MAX] = {0};
    int spawned = 0, status = 0;
    nema_pid_t pgid = 0;
    for (int i = 0; i < n; i++) {
        cmd_t *c = &cmds[i];
        nema_obj_t *in = prev_r, *out = NULL, *err = NULL, *next_r = NULL, *open_in = NULL, *open_out = NULL, *open_err = NULL;
        if (c->in) {
            open_in = open_redirect(c->in, NEMA_O_READ, "input");
            if (!open_in) { status = 1; break; }
            in = open_in;
        }
        if (i + 1 < n) {
            nema_obj_t *w;
            if (nema_pipe_create(1024, NEMA_MEM_ANY, sh->owner, &next_r, &w) != NEMA_OK) { pal_eprintf("sh: out of memory\n"); status = 1; nema_obj_unref(open_in); break; }
            out = w;
        }
        if (c->out) {
            open_out = open_redirect(c->out, NEMA_O_WRITE | NEMA_O_CREATE | (c->append ? NEMA_O_APPEND : NEMA_O_TRUNC), "output");
            if (!open_out) { status = 1; nema_obj_unref(open_in); nema_obj_unref(out); nema_obj_unref(next_r); break; }
            if (out) nema_obj_unref(out);
            out = open_out;
        }
        if (c->err) {
            open_err = open_redirect(c->err, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, "error output");
            err = open_err;
        } else if (c->err_to_out) {
            err = out;
        }
        nema_spawn_t sp = {.name = c->argv[0], .argc = c->argc, .argv = (const char *const *)c->argv,
                           .stdio = {in, out, err}, .pgid = pgid, .flags = bg ? NEMA_SPAWN_DETACH : 0};
        nema_pid_t pid;
        nema_err_t r = nema_proc_spawn(&sp, &pid);
        /* the children hold their own references now */
        nema_obj_unref(prev_r);
        nema_obj_unref(open_in);
        nema_obj_unref(out);
        nema_obj_unref(open_err);
        prev_r = next_r;
        if (r != NEMA_OK) {
            if (r == NEMA_ERR_NOENT) pal_eprintf("sh: %s: command not found\n", c->argv[0]);
            else pal_eprintf("sh: %s: %s\n", c->argv[0], pal_strerror(r));
            status = r == NEMA_ERR_NOENT ? 127 : 126;
            break;
        }
        if (i == 0) pgid = pid;
        pids[spawned++] = pid;
    }
    nema_obj_unref(prev_r);
    if (spawned == 0) return status ? status : 1;

    if (bg) {
        pal_printf("[%d]\n", pids[spawned - 1]);
        return 0;
    }
    tty_mode(NEMA_TTY_COOKED);
    tty_fg(pgid);
    for (int i = 0; i < spawned; i++) {
        int code = 0;
        nema_err_t r;
        while ((r = nema_proc_wait(pids[i], &code, -1)) == NEMA_ERR_INTR) nema_sig_take(NEMA_SIG_FATAL);
        if (i == spawned - 1) status = r == NEMA_OK ? code : 1;
    }
    tty_fg(sh->pgid);
    if (sh->interactive) tty_mode(NEMA_TTY_ONLCR);
    return status;
}

/* Build argv for one command from tokens starting at *i; stops at an operator. */
static int parse_cmd(sh_t *sh, toks_t *k, int *i, cmd_t *c, char *gstore, size_t *gused, size_t gcap) {
    memset(c, 0, sizeof(*c));
    for (; *i < k->n; (*i)++) {
        tok_t *t = &k->t[*i];
        if (t->kind == T_WORD) {
            if (c->argc == 0) {
                const char *al = alias_of(sh, t->s);
                if (al) { /* split the alias value on spaces; it is not expanded again */
                    char *copy = gstore + *gused;
                    size_t l = strlen(al) + 1;
                    if (*gused + l > gcap) return -1;
                    memcpy(copy, al, l);
                    *gused += l;
                    char *save = NULL;
                    for (char *w = strtok_r(copy, " ", &save); w && c->argc < ARGS_MAX - 1; w = strtok_r(NULL, " ", &save))
                        c->argv[c->argc++] = w;
                    continue;
                }
            }
            if (t->glob) {
                char *m[16];
                int nm = glob_word(t->s, m, 16, gstore + *gused, gcap - *gused);
                if (nm > 0) {
                    for (int j = 0; j < nm && c->argc < ARGS_MAX - 1; j++) c->argv[c->argc++] = m[j];
                    for (int j = 0; j < nm; j++) *gused += strlen(m[j]) + 1;
                    continue;
                }
            }
            if (c->argc < ARGS_MAX - 1) c->argv[c->argc++] = t->s;
        } else if (t->kind == T_IN || t->kind == T_OUT || t->kind == T_APP || t->kind == T_ERR) {
            if (*i + 1 >= k->n || k->t[*i + 1].kind != T_WORD) return -1;
            char *target = k->t[++(*i)].s;
            if (t->kind == T_IN) c->in = target;
            else if (t->kind == T_ERR) c->err = target;
            else { c->out = target; c->append = t->kind == T_APP; }
        } else if (t->kind == T_ERR2OUT) {
            c->err_to_out = true;
        } else {
            break; /* an operator that ends the command */
        }
    }
    c->argv[c->argc] = NULL;
    return 0;
}

/* Parse one pipeline: up to STAGES_MAX commands joined by |, then an optional &. */
int sh_parse_pipeline(sh_t *sh, toks_t *k, int *i, cmd_t cmds[STAGES_MAX], int *nc_out, bool *bg_out, char *gstore,
                      size_t *gused, size_t gcap) {
    int nc = 0;
    for (;;) {
        if (nc == STAGES_MAX || parse_cmd(sh, k, i, &cmds[nc], gstore, gused, gcap) < 0) return -1;
        /* a stage with no command is only allowed for a lone redirection; inside a pipeline it is a syntax error */
        if (cmds[nc].argc == 0 && (nc > 0 || (*i < k->n && k->t[*i].kind == T_PIPE))) return -1;
        nc++;
        if (*i < k->n && k->t[*i].kind == T_PIPE) {
            (*i)++;
            continue;
        }
        break;
    }
    if (nc && cmds[nc - 1].argc == 0) return -1;
    *nc_out = nc;
    *bg_out = *i < k->n && k->t[*i].kind == T_BG;
    return 0;
}

static int run_line_impl(sh_t *sh, toks_t *k, const char *line);

/* Tokens live in PSRAM, not on the 12 KB task stack; one allocation per line keeps tabs independent. */
int sh_run_line(sh_t *sh, const char *line) {
    toks_t *k = (toks_t *)nema_alloc(NEMA_MEM_ANY, sh->owner, sizeof(*k));
    if (!k) {
        pal_eprintf("sh: out of memory\n");
        return sh->status = 1;
    }
    int r = run_line_impl(sh, k, line);
    nema_free(k);
    return r;
}

/* The index of the & that ends an and-or list of two or more pipelines at the start of k, or -1 (no &&/||, or it
 * ends with ; or the line). */
static int bg_list_end(const toks_t *k) {
    bool andor = false;
    for (int j = 0; j < k->n; j++) {
        int kind = k->t[j].kind;
        if (kind == T_AND || kind == T_OR) andor = true;
        else if (kind == T_SEMI) return -1;
        else if (kind == T_BG) return andor && j > 0 ? j : -1;
    }
    return -1;
}

static int run_line_impl(sh_t *sh, toks_t *k, const char *line) {
    char gstore[512];
    size_t gused = 0;
    sh_tokenize(sh, line, k);
    if (k->error) {
        pal_eprintf("sh: %s\n", k->error);
        return sh->status = 2;
    }
    if (k->n == 0) return sh->status;

    int i = 0, status = sh->status;
    bool run = true;
    const char *src = line; /* the text k was tokenized from */
    while (i < k->n && !sh->exiting) {
        int bg_at = i == 0 ? bg_list_end(k) : -1;
        if (bg_at > 0) {
            /* "a && b &": the whole list runs in the background, in a shell of its own, as POSIX says */
            size_t len = (size_t)(k->t[bg_at - 1].end - src);
            char text[SH_LINE_MAX];
            snprintf(text, sizeof(text), "%.*s", (int)len, src);
            cmd_t c;
            memset(&c, 0, sizeof(c));
            c.argv[0] = (char *)"sh", c.argv[1] = (char *)"-c", c.argv[2] = text;
            c.argc = 3;
            status = sh->status = run_pipeline(sh, &c, 1, true);
            i = bg_at;
        } else if (k->t[i].kind == T_WORD && strchr(k->t[i].s, '=') && k->t[i].s[0] != '=' &&
            (i + 1 >= k->n || k->t[i + 1].kind != T_WORD)) {
            char *eq = strchr(k->t[i].s, '=');
            *eq = 0;
            if (run) set_var(sh, k->t[i].s, eq + 1);
            i++;
        } else {
            cmd_t cmds[STAGES_MAX];
            int nc = 0;
            bool bg = false;
            if (sh_parse_pipeline(sh, k, &i, cmds, &nc, &bg, gstore, &gused, sizeof(gstore)) < 0) {
                pal_eprintf("sh: syntax error\n");
                return sh->status = 2;
            }
            if (run && cmds[0].argc > 0) status = sh->status = run_pipeline(sh, cmds, nc, bg);
        }
        if (i >= k->n) break;
        int op = k->t[i].kind;
        const char *rest = k->t[i].end;
        if (op == T_AND) run = status == 0;
        else if (op == T_OR) run = status != 0;
        else run = true;
        /* variables and $? in the rest of the line must see what the commands before it just did: tokenize again from here */
        sh_tokenize(sh, rest, k);
        src = rest;
        gused = 0;
        i = 0;
        if (k->error) {
            pal_eprintf("sh: %s\n", k->error);
            return sh->status = 2;
        }
        if (k->n == 0) break;
    }
    return status;
}

/* ------------------------------------------------------------------ completion */
static void add_cand(sh_comp_t *c, const char *name, bool dir) {
    if (c->n >= SH_COMP_MAX) return;
    for (int i = 0; i < c->n; i++) {
        char tmp[NEMA_NAME_MAX + 2];
        snprintf(tmp, sizeof(tmp), "%s%s", name, dir ? "/" : "");
        if (strcmp(c->c[i].name, tmp) == 0) return;
    }
    snprintf(c->c[c->n].name, sizeof(c->c[0].name), "%s%s", name, dir ? "/" : "");
    c->n++;
}

void sh_complete(sh_t *sh, const char *line, int cur, sh_comp_t *out) {
    memset(out, 0, sizeof(*out));
    int start = cur;
    while (start > 0 && line[start - 1] != ' ') start--;
    out->start = start;
    char word[NEMA_PATH_MAX];
    int wl = cur - start;
    if (wl >= (int)sizeof(word)) return;
    memcpy(word, line + start, (size_t)wl);
    word[wl] = 0;
    bool first = true;
    for (int i = 0; i < start; i++)
        if (line[i] != ' ') first = false;
    out->is_command = first;
    if (first && !strchr(word, '/')) {
        for (int i = 0; BUILTINS[i]; i++)
            if (strncmp(BUILTINS[i], word, (size_t)wl) == 0) add_cand(out, BUILTINS[i], false);
        for (size_t i = 0; i < nema_app_count(); i++) {
            const nema_app_t *a = nema_app_at(i);
            if (!(a->flags & NEMA_APP_HIDDEN) && strncmp(a->name, word, (size_t)wl) == 0) add_cand(out, a->name, false);
        }
        return;
    }
    char dir[NEMA_PATH_MAX], prefix[NEMA_NAME_MAX];
    const char *slash = strrchr(word, '/');
    if (slash) {
        size_t dl = (size_t)(slash - word);
        if (dl == 0) dl = 1;
        memcpy(dir, word, dl);
        dir[dl] = 0;
        nema_strlcpy(prefix, slash + 1, sizeof(prefix));
        out->start = start + (int)(slash - word) + 1;
    } else {
        strcpy(dir, ".");
        nema_strlcpy(prefix, word, sizeof(prefix));
    }
    nema_dirent_t e;
    for (size_t i = 0; nema_fs_readdir(dir, i, &e) == NEMA_OK; i++) {
        if (e.name[0] == '.' && prefix[0] != '.') continue;
        if (strncmp(e.name, prefix, strlen(prefix)) == 0) add_cand(out, e.name, e.type == NEMA_T_DIR);
    }
    (void)sh;
}

/* ------------------------------------------------------------------ main */
static void make_prompt(sh_t *sh, char *out, size_t cap) {
    const char *cwd = nema_self()->cwd;
    const char *home = sh_var(sh, "HOME");
    char shown[48];
    if (home && strncmp(cwd, home, strlen(home)) == 0 && (cwd[strlen(home)] == 0 || cwd[strlen(home)] == '/') && strlen(home) > 1)
        snprintf(shown, sizeof(shown), "~%s", cwd + strlen(home));
    else
        nema_strlcpy(shown, cwd, sizeof(shown));
    const char *code = sh->status == 0 ? "\x1b[1;32m" : "\x1b[1;31m";
    snprintf(out, cap, "%spalanu\x1b[0m:\x1b[1;34m%s\x1b[0m$ ", code, shown);
}

static int sh_main(int argc, char **argv) {
    sh_t *sh = (sh_t *)nema_calloc(NEMA_MEM_ANY, nema_self()->owner, sizeof(*sh));
    if (!sh) return 1;
    sh->owner = nema_self()->owner;
    sh->pgid = nema_self()->pgid;
    set_var(sh, "HOME", "/home");
    set_var(sh, "USER", "palanu");
    set_var(sh, "TERM", "xterm-256color");
    nema_err_t cd = nema_set_cwd("/home");
    (void)cd;

    int status = 0;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        status = sh_run_line(sh, argv[2]);
    } else if (argc >= 2 && argv[1][0] != '-') {
        int fd = nema_open(argv[1], NEMA_O_READ);
        if (fd < 0) {
            pal_eprintf("sh: %s: %s\n", argv[1], pal_strerror(fd));
            status = 127;
        } else {
            status = sh_source_fd(sh, fd);
            nema_close(fd);
        }
    } else {
        sh->interactive = pal_isatty(0);
        if (sh->interactive) {
            nema_winsize_t ws;
            sh->cols = nema_ioctl(0, NEMA_TIOC_GETSIZE, &ws) == NEMA_OK ? ws.cols : 40;
            tty_mode(NEMA_TTY_ONLCR);
            tty_fg(sh->pgid);
            pal_puts("\x1b[1mPalanuOS\x1b[0m  type \x1b[1mhelp\x1b[0m for commands\n");
            /* the up arrow reaches commands from before the restart: start with the newest of the saved ones */
            int total = pal_history_count();
            for (int i = total > SH_HIST_MAX ? total - SH_HIST_MAX : 0; i < total; i++) {
                char h[SH_LINE_MAX];
                if (!pal_history_get(i, h, sizeof(h))) break;
                char *copy = (char *)nema_alloc(NEMA_MEM_ANY, sh->owner, strlen(h) + 1);
                if (!copy) break;
                strcpy(copy, h);
                sh->hist[sh->nhist++] = copy;
            }
            int fd = nema_open("/etc/profile", NEMA_O_READ);
            if (fd >= 0) {
                sh_source_fd(sh, fd);
                nema_close(fd);
            }
        }
        char line[SH_LINE_MAX], prompt[96];
        while (!sh->exiting) {
            nema_winsize_t ws;
            if (nema_ioctl(0, NEMA_TIOC_GETSIZE, &ws) == NEMA_OK) sh->cols = ws.cols;
            make_prompt(sh, prompt, sizeof(prompt));
            int n = sh_readline(sh, prompt, line, sizeof(line));
            if (n < 0) break;
            if (n == 0) continue;
            sh_hist_add(sh, line);
            sh_run_line(sh, line);
        }
        status = sh->status;
        if (sh->interactive) {
            pal_history_flush(); /* the lines of this session, before it goes */
            tty_mode(NEMA_TTY_COOKED);
        }
    }
    for (int i = 0; i < sh->nhist; i++) nema_free(sh->hist[i]);
    nema_free(sh);
    return status;
}

static const nema_app_t SH_APP = {"sh", "the shell (sh [-c cmd | script])", sh_main, 12 * 1024, 0};

void pal_app_register_sh(void);
void pal_app_register_sh(void) { nema_app_register(&SH_APP); }
