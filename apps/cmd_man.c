/* man: the offline manual. `man` lists the topics (the user's docs, stored deflated in the image: apps/man_data.c, made by
 * forge/tools/mandata.ts) and the commands; `man TOPIC` prints one, `man COMMAND` a command's line of help, `man -k WORD` finds the
 * lines of the manual that hold WORD. A long topic reads well through a pipe: man guide | tail -40. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/inflate.h"

typedef struct {
    const char *name, *title;
    const uint8_t *z;
    size_t zlen, len;
} man_topic_t;
extern const man_topic_t MAN_TOPICS[];
extern const size_t MAN_TOPICS_N;

static char *open_topic(const man_topic_t *t) {
    char *buf = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, t->len + 1);
    if (!buf) return NULL;
    if (pal_inflate(t->z, t->zlen, buf, t->len) != NEMA_OK) {
        nema_free(buf);
        return NULL;
    }
    buf[t->len] = 0;
    return buf;
}

static bool has_ci(const char *line, size_t n, const char *word) {
    size_t m = strlen(word);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && (line[i + k] | 32) == (word[k] | 32)) k++;
        if (k == m) return true;
    }
    return false;
}

static int cmd_man(int argc, char **argv) {
    if (argc < 2) {
        pal_printf("Topics (man TOPIC):\n");
        for (size_t i = 0; i < MAN_TOPICS_N; i++) pal_printf("  %-10s %s\n", MAN_TOPICS[i].name, MAN_TOPICS[i].title);
        pal_printf("Commands: man COMMAND for its line, help for all of them. man -k WORD searches the manual.\n");
        return 0;
    }
    if (!strcmp(argv[1], "-k") && argc > 2) {
        int hits = 0;
        for (size_t i = 0; i < MAN_TOPICS_N; i++) {
            char *t = open_topic(&MAN_TOPICS[i]);
            if (!t) continue;
            for (char *p = t; *p;) {
                char *e = strchr(p, '\n');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                if (has_ci(p, n, argv[2])) {
                    pal_printf("%s: %.*s\n", MAN_TOPICS[i].name, (int)(n > 70 ? 70 : n), p);
                    hits++;
                }
                p = e ? e + 1 : p + n;
            }
            nema_free(t);
        }
        if (!hits) pal_printf("nothing about %s\n", argv[2]);
        return hits ? 0 : 1;
    }
    for (size_t i = 0; i < MAN_TOPICS_N; i++)
        if (!strcmp(argv[1], MAN_TOPICS[i].name)) {
            char *t = open_topic(&MAN_TOPICS[i]);
            if (!t) {
                pal_eprintf("man: not enough memory\n");
                return 1;
            }
            pal_printf("%s\n", t);
            nema_free(t);
            return 0;
        }
    const nema_app_t *a = nema_app_find(argv[1]);
    if (a) {
        pal_printf("%s: %s\n", a->name, a->help && a->help[0] ? a->help : "(no description)");
        return 0;
    }
    pal_eprintf("man: no topic or command called %s (man lists them)\n", argv[1]);
    return 1;
}

/* printf FORMAT [ARG]...: the format with \n, \t, \\ and %s, %d, %x, %c, %% (each ARG used once, a missing one is empty). */
static int cmd_printf(int argc, char **argv) {
    if (argc < 2) {
        pal_eprintf("usage: printf FORMAT [ARG]...\n");
        return 2;
    }
    char out[512];
    size_t o = 0;
    int ai = 2;
    for (const char *f = argv[1]; *f && o < sizeof(out) - 32; f++) {
        if (*f == '\\' && f[1]) {
            f++;
            out[o++] = *f == 'n' ? '\n' : *f == 't' ? '\t' : *f == 'r' ? '\r' : *f == '0' ? 0 : *f;
            continue;
        }
        if (*f != '%' || !f[1]) {
            out[o++] = *f;
            continue;
        }
        f++;
        const char *arg = ai < argc ? argv[ai++] : "";
        char tmp[64];
        switch (*f) {
        case 's': snprintf(tmp, sizeof(tmp), "%s", arg); break;
        case 'd': case 'i': snprintf(tmp, sizeof(tmp), "%ld", strtol(arg, NULL, 0)); break;
        case 'x': snprintf(tmp, sizeof(tmp), "%lx", strtoul(arg, NULL, 0)); break;
        case 'c': snprintf(tmp, sizeof(tmp), "%c", arg[0]); break;
        case '%': snprintf(tmp, sizeof(tmp), "%%"), ai--; break;
        default: snprintf(tmp, sizeof(tmp), "%%%c", *f), ai--; break;
        }
        size_t k = strlen(tmp);
        if (o + k < sizeof(out)) memcpy(out + o, tmp, k), o += k;
    }
    nema_write(1, out, o);
    return 0;
}

static const nema_app_t MAN_APPS[] = {
    {"man", "the offline manual: topics, commands, search (man -k WORD)", cmd_man, 8192, 0},
    {"printf", "print a formatted line: printf FORMAT [ARG]...", cmd_printf, 4096, 0},
};

void pal_apps_register_man(void);
void pal_apps_register_man(void) {
    for (size_t i = 0; i < sizeof(MAN_APPS) / sizeof(MAN_APPS[0]); i++) nema_app_register(&MAN_APPS[i]);
}
