/* PalanuOS document screens (ADR-0012): a screen that reads lines (a log, a file) described as data, so each desktop session draws
 * it in its own look. The app keeps its state (the first line shown, the toolbar focus) and its keys; the doc points at that state
 * and gives the title, the toolbar rows and the lines through callbacks. The classic session draws the view as before (its draw);
 * a session that draws documents itself (Athena) reads the bound doc instead, and writes back how many lines it shows (vis), so
 * the app's paging fits whichever look is on screen. Built-in apps only for now; nothing here allocates. */
#ifndef PAL_DOC_H
#define PAL_DOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAL_DOC_LINE_MAX 200 /* longest line text a line callback writes, NUL included */

typedef struct {
    const char *label;
    void (*value)(void *ctx, char *out, size_t cap); /* the value on the right, or NULL */
    int (*get)(void *ctx);                           /* non-NULL: a switch, on when it returns non-zero */
} pal_doc_tool_t;

typedef struct pal_doc {
    void (*title)(void *ctx, char *out, size_t cap);
    const pal_doc_tool_t *tools; /* rows above the lines (Follow newest, Level), or NULL */
    int (*ntools)(void *ctx);    /* how many of tools show now (NULL: none) */
    int *focus;                  /* the toolbar row with the focus, -1 or past the last for none */
    int (*count)(void *ctx);     /* lines */
    void (*line)(void *ctx, int i, char *out, size_t cap, bool *mark); /* line i; *mark highlights it (a search hit) */
    void (*empty)(void *ctx, char *out, size_t cap); /* shown with no lines, or NULL */
    int *top;                    /* the first line shown: the app's */
    int *vis;                    /* lines that fit: written by whoever draws */
    bool mono;                   /* fixed-width text (a file, hex) */
    void *ctx;
    uint32_t gen;                /* the app bumps it when lines change without the count changing */
} pal_doc_t;

/* Binds doc to a view id (NULL unbinds); at most 4 at once. */
void pal_doc_bind(const char *view_id, pal_doc_t *doc);
pal_doc_t *pal_doc_bound(const char *view_id);

#endif
