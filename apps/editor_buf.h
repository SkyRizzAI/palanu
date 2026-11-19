/* The text editor's buffer (editor_buf.c): a gap buffer of bytes with line arithmetic. Pure, so tests/unit/test_editor_buf.c checks
 * it without a screen. Positions are byte offsets into the text; lines end with '\n'. */
#ifndef PAL_APPS_EDITOR_BUF_H
#define PAL_APPS_EDITOR_BUF_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *buf;   /* cap bytes, owned by the caller */
    size_t cap;
    size_t gs, ge; /* the gap: [gs, ge) is free */
} ebuf_t;

void eb_init(ebuf_t *b, char *mem, size_t cap);
size_t eb_len(const ebuf_t *b);
char eb_at(const ebuf_t *b, size_t pos);           /* the byte at pos (0 past the end) */
void eb_move(ebuf_t *b, size_t pos);               /* the gap (the cursor) to pos */
bool eb_insert(ebuf_t *b, const char *s, size_t n); /* at the gap; false when it does not fit */
size_t eb_delete_back(ebuf_t *b, size_t n);        /* up to n bytes before the gap; returns how many */
size_t eb_delete_fwd(ebuf_t *b, size_t n);         /* up to n bytes after it */
size_t eb_cursor(const ebuf_t *b);
size_t eb_copy(const ebuf_t *b, size_t from, char *out, size_t n); /* text [from, from + n) into out; returns the bytes copied */

size_t eb_line_start(const ebuf_t *b, size_t pos); /* the start of pos's line */
size_t eb_line_end(const ebuf_t *b, size_t pos);   /* its '\n' (or the end of the text) */
size_t eb_line_of(const ebuf_t *b, size_t pos);    /* 0-based line number of pos */
size_t eb_lines(const ebuf_t *b);                  /* lines in the text (an empty text has 1) */
size_t eb_line_pos(const ebuf_t *b, size_t line);  /* where line starts (the end of the text past the last line) */
long eb_find(const ebuf_t *b, size_t from, const char *needle); /* first match at or after from, wrapping; -1 when none (ASCII case-insensitive) */

#endif
