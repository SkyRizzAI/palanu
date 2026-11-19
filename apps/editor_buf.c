/* Gap buffer (editor_buf.h). The text is buf[0, gs) followed by buf[ge, cap); typing and deleting at the cursor move no bytes. */
#include "editor_buf.h"

#include <string.h>

void eb_init(ebuf_t *b, char *mem, size_t cap) {
    b->buf = mem;
    b->cap = cap;
    b->gs = 0;
    b->ge = cap;
}

size_t eb_len(const ebuf_t *b) { return b->cap - (b->ge - b->gs); }
size_t eb_cursor(const ebuf_t *b) { return b->gs; }

char eb_at(const ebuf_t *b, size_t pos) {
    if (pos >= eb_len(b)) return 0;
    return pos < b->gs ? b->buf[pos] : b->buf[pos + (b->ge - b->gs)];
}

void eb_move(ebuf_t *b, size_t pos) {
    size_t len = eb_len(b);
    if (pos > len) pos = len;
    if (pos < b->gs) { /* bytes [pos, gs) go to the end of the gap */
        size_t n = b->gs - pos;
        memmove(b->buf + b->ge - n, b->buf + pos, n);
        b->gs -= n, b->ge -= n;
    } else if (pos > b->gs) {
        size_t n = pos - b->gs;
        memmove(b->buf + b->gs, b->buf + b->ge, n);
        b->gs += n, b->ge += n;
    }
}

bool eb_insert(ebuf_t *b, const char *s, size_t n) {
    if (n > b->ge - b->gs) return false;
    memcpy(b->buf + b->gs, s, n);
    b->gs += n;
    return true;
}

size_t eb_delete_back(ebuf_t *b, size_t n) {
    if (n > b->gs) n = b->gs;
    b->gs -= n;
    return n;
}

size_t eb_delete_fwd(ebuf_t *b, size_t n) {
    size_t after = b->cap - b->ge;
    if (n > after) n = after;
    b->ge += n;
    return n;
}

size_t eb_copy(const ebuf_t *b, size_t from, char *out, size_t n) {
    size_t len = eb_len(b), k = 0;
    for (size_t p = from; p < len && k < n; p++) out[k++] = eb_at(b, p);
    return k;
}

size_t eb_line_start(const ebuf_t *b, size_t pos) {
    while (pos > 0 && eb_at(b, pos - 1) != '\n') pos--;
    return pos;
}

size_t eb_line_end(const ebuf_t *b, size_t pos) {
    size_t len = eb_len(b);
    while (pos < len && eb_at(b, pos) != '\n') pos++;
    return pos;
}

size_t eb_line_of(const ebuf_t *b, size_t pos) {
    size_t n = 0;
    for (size_t p = 0; p < pos && p < eb_len(b); p++) n += eb_at(b, p) == '\n';
    return n;
}

size_t eb_lines(const ebuf_t *b) { return eb_line_of(b, eb_len(b)) + 1; }

size_t eb_line_pos(const ebuf_t *b, size_t line) {
    size_t len = eb_len(b), p = 0;
    while (line > 0 && p < len) {
        if (eb_at(b, p++) == '\n') line--;
    }
    return line > 0 ? len : p;
}

static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

long eb_find(const ebuf_t *b, size_t from, const char *needle) {
    size_t len = eb_len(b), m = needle ? strlen(needle) : 0;
    if (m == 0 || m > len) return -1;
    for (size_t k = 0; k < len; k++) {
        size_t p = (from + k) % len;
        if (p + m > len) continue;
        size_t i = 0;
        while (i < m && low(eb_at(b, p + i)) == low(needle[i])) i++;
        if (i == m) return (long)p;
    }
    return -1;
}
