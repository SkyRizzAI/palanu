/* The text viewer's model (no drawing): the bytes of a file, the soft-wrapped rows for a column count, search, the binary test and
 * the hex dump rows. Pure, so tests/unit/test_textview.c can check it on a plain buffer. */
#ifndef TEXTVIEW_MODEL_H
#define TEXTVIEW_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TV_MAX_BYTES (256u * 1024u) /* the viewer reads at most this much; more is noted as truncated */
#define TV_MAX_ROWS 8192            /* wrapped rows kept; a file with more is shown up to the last one that fits */
#define TV_HEX_BYTES 8              /* bytes per row of the hex dump */

typedef struct {
    uint32_t start, end; /* bytes [start, end) of the row; a '\n' is not part of it */
    uint32_t line;       /* 1-based line number when the row starts a line, else 0 */
} tv_row_t;

typedef struct {
    uint8_t *data;   /* TV_MAX_BYTES, owned by the viewer */
    size_t len;      /* bytes read */
    bool truncated;  /* the file is longer than TV_MAX_BYTES */
    bool binary;     /* shown as a hex dump */
    tv_row_t *rows;  /* TV_MAX_ROWS, owned by the viewer */
    int nrows;
    bool rows_full;  /* the row table filled up before the end of the text */
    int lines;       /* logical lines laid out */
    int cols;        /* the column count the rows were made for (0 = not laid out) */
} textview_t;

/* Decodes one UTF-8 sequence at p (n bytes available). Returns its length (1 for a bad byte) and stores the code point. */
size_t tv_decode(const uint8_t *p, size_t n, uint32_t *cp);
/* True when the first bytes hold a NUL or too many control bytes (tabs, newlines and carriage returns are text). */
bool tv_looks_binary(const uint8_t *p, size_t n);
/* Lays the text out in rows of at most cols columns (tabs go to the next multiple of 4). Returns the row count. */
int tv_layout(textview_t *t, int cols);
/* Number of hex dump rows. */
int tv_hex_rows(const textview_t *t);
/* The row that holds byte off (text rows), or 0. */
int tv_row_of(const textview_t *t, size_t off);
/* First offset >= from where needle occurs (ASCII case-insensitive when ci), or -1. */
long tv_find(const textview_t *t, size_t from, const char *needle, bool ci);
/* Collects the occurrences of needle that start inside rows [first, last] into starts (up to max). Returns the count. */
int tv_matches(const textview_t *t, int first, int last, const char *needle, bool ci, uint32_t *starts, int max);

#endif
