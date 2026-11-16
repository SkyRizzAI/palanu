/* The text viewer's model: see textview_model.h. */
#include <string.h>

#include "textview_model.h"

size_t tv_decode(const uint8_t *p, size_t n, uint32_t *cp) {
    uint8_t b = p[0];
    size_t want = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
    if (want == 0 || want > n) {
        *cp = 0xFFFD;
        return 1;
    }
    uint32_t v = want == 1 ? b : want == 2 ? (b & 0x1Fu) : want == 3 ? (b & 0x0Fu) : (b & 0x07u);
    for (size_t i = 1; i < want; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (p[i] & 0x3Fu);
    }
    *cp = v;
    return want;
}

bool tv_looks_binary(const uint8_t *p, size_t n) {
    size_t odd = 0, lim = n < 512 ? n : 512;
    for (size_t i = 0; i < lim; i++) {
        if (p[i] == 0) return true;
        if (p[i] < 0x20 && p[i] != '\t' && p[i] != '\n' && p[i] != '\r' && p[i] != 0x1B) odd++;
    }
    return lim > 0 && odd * 10 > lim;
}

static void emit(textview_t *t, uint32_t start, uint32_t end, uint32_t line) {
    if (t->nrows >= TV_MAX_ROWS) {
        t->rows_full = true;
        return;
    }
    tv_row_t *r = &t->rows[t->nrows++];
    r->start = start;
    r->end = end;
    r->line = line;
    if (line) t->lines++;
}

int tv_layout(textview_t *t, int cols) {
    t->nrows = 0;
    t->lines = 0;
    t->rows_full = false;
    t->cols = cols;
    if (cols < 4) cols = 4;
    uint32_t row_start = 0, line = 1;
    uint32_t col = 0;
    bool line_start = true;
    size_t pos = 0;
    while (pos < t->len && !t->rows_full) {
        uint8_t b = t->data[pos];
        if (b == '\n') {
            emit(t, row_start, (uint32_t)pos, line_start ? line : 0);
            pos++;
            row_start = (uint32_t)pos;
            col = 0;
            line++;
            line_start = true;
            continue;
        }
        if (b == '\r') {
            pos++;
            continue;
        }
        uint32_t cp;
        size_t n = tv_decode(t->data + pos, t->len - pos, &cp);
        uint32_t w = cp == '\t' ? 4u - col % 4u : 1u;
        if (col + w > (uint32_t)cols && col > 0) {
            emit(t, row_start, (uint32_t)pos, line_start ? line : 0);
            row_start = (uint32_t)pos;
            col = 0;
            line_start = false;
            continue; /* the same character starts the next row */
        }
        col += w;
        pos += n;
    }
    if (!t->rows_full && row_start < t->len) emit(t, row_start, (uint32_t)t->len, line_start ? line : 0);
    return t->nrows;
}

int tv_hex_rows(const textview_t *t) { return (int)((t->len + TV_HEX_BYTES - 1) / TV_HEX_BYTES); }

int tv_row_of(const textview_t *t, size_t off) {
    int lo = 0, hi = t->nrows - 1;
    if (hi < 0) return 0;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (t->rows[mid].start <= off) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static int fold(uint8_t c, bool ci) { return ci && c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool at(const textview_t *t, size_t i, const char *needle, size_t m, bool ci) {
    for (size_t k = 0; k < m; k++)
        if (fold(t->data[i + k], ci) != fold((uint8_t)needle[k], ci)) return false;
    return true;
}

long tv_find(const textview_t *t, size_t from, const char *needle, bool ci) {
    size_t m = needle ? strlen(needle) : 0;
    if (!m || t->len < m) return -1;
    for (size_t i = from; i + m <= t->len; i++)
        if (at(t, i, needle, m, ci)) return (long)i;
    return -1;
}

int tv_matches(const textview_t *t, int first, int last, const char *needle, bool ci, uint32_t *starts, int max) {
    size_t m = needle ? strlen(needle) : 0;
    if (!m || t->nrows == 0 || first < 0 || last < first) return 0;
    if (last >= t->nrows) last = t->nrows - 1;
    size_t from = t->rows[first].start, to = t->rows[last].end;
    int k = 0;
    for (size_t i = from; i + m <= to && k < max; i++) {
        if (at(t, i, needle, m, ci)) starts[k++] = (uint32_t)i;
    }
    return k;
}
