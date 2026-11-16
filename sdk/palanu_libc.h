/* palanu_libc.h: a small freestanding C library for PalanuOS apps. App code can be ordinary C:
 * strings, ctype, malloc, snprintf, printf, qsort, rand. Nothing here needs a download: clang with the wasm32
 * target and the linker are enough.
 *
 *     #define PALANU_LIBC_IMPLEMENTATION   // in exactly one .c file of the app, before the include
 *     #include "palanu_libc.h"
 *
 * Other files include it without the define and get declarations only. The app is built with `-I sdk` (the
 * palanu build does that), so the include is "palanu_libc.h". Guide: docs/DEVELOPER.md.
 *
 * Heap: first fit over one free list sorted by address, with coalescing; blocks are 8-byte aligned with an 8-byte
 * header. The heap starts after __heap_base and grows with memory.grow in 64 KB pages. When the runtime refuses
 * to grow (the app's memory limit: `mem_kb` in app.toml), malloc returns NULL.
 *
 * Not here: floating point (%f is printed as text), wide characters, locales, file streams (use pal_open and
 * pal_write), errno. Unknown conversions are copied to the output unchanged.
 *
 * Host tests (tests/unit/test_sdk_libc.c) define PALANU_LIBC_NATIVE_TEST: every name below is then renamed
 * pltest_* so the host libc is not touched, the heap comes from a static arena, and the test supplies pal_write. */
#ifndef PALANU_LIBC_H
#define PALANU_LIBC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "palanu.h"

#ifdef PALANU_LIBC_NATIVE_TEST
#define memcpy pltest_memcpy
#define memmove pltest_memmove
#define memset pltest_memset
#define memcmp pltest_memcmp
#define strlen pltest_strlen
#define strcmp pltest_strcmp
#define strncmp pltest_strncmp
#define strcpy pltest_strcpy
#define strncpy pltest_strncpy
#define strchr pltest_strchr
#define strrchr pltest_strrchr
#define strstr pltest_strstr
#define atoi pltest_atoi
#define strtol pltest_strtol
#define abs pltest_abs
#define isdigit pltest_isdigit
#define isalpha pltest_isalpha
#define isalnum pltest_isalnum
#define isspace pltest_isspace
#define isupper pltest_isupper
#define islower pltest_islower
#define isxdigit pltest_isxdigit
#define toupper pltest_toupper
#define tolower pltest_tolower
#define malloc pltest_malloc
#define free pltest_free
#define realloc pltest_realloc
#define calloc pltest_calloc
#define vsnprintf pltest_vsnprintf
#define snprintf pltest_snprintf
#define vprintf pltest_vprintf
#define printf pltest_printf
#define puts pltest_puts
#define putchar pltest_putchar
#define qsort pltest_qsort
#define bsearch pltest_bsearch
#define rand pltest_rand
#define srand pltest_srand
#define exit pltest_exit
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

/* ---- strings and memory ---- */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);

/* ---- ctype (ASCII only) ---- */
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int toupper(int c);
int tolower(int c);

/* ---- numbers ---- */
int abs(int v);
int atoi(const char *s);
long strtol(const char *s, char **end, int base); /* base 0, 2..36; clamps on overflow */

/* ---- heap: malloc family over linear memory (see the header comment) ---- */
void *malloc(size_t n);
void free(void *p);
void *realloc(void *p, size_t n);
void *calloc(size_t n, size_t size);

/* ---- text output. %d %i %u %o %x %X %p %c %s %% with flags - + space 0 # width, precision (integers and %s),
 * and length h hh l ll z. Writes never overflow buf; the return value is the length the full output needs. ---- */
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int snprintf(char *buf, size_t cap, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap); /* to fd 1 through pal_write */
int printf(const char *fmt, ...);
int puts(const char *s);                  /* adds the newline */
int putchar(int c);

/* ---- sorting and searching ---- */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)); /* heapsort: not stable */
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

/* ---- random: xorshift32, seeded from pal_now_ms on the first call unless srand was called ---- */
int rand(void);
void srand(unsigned int seed);

/* ---- process ---- */
__attribute__((noreturn)) void exit(int code); /* pal_exit(code) */

#endif /* PALANU_LIBC_H */

#if defined(PALANU_LIBC_IMPLEMENTATION) && !defined(PALANU_LIBC_IMPL_DONE)
#define PALANU_LIBC_IMPL_DONE

#define PLC_FN static __attribute__((unused))

#ifndef PALANU_LIBC_NATIVE_TEST
extern char __heap_base[]; /* set by the linker: the first byte after static data and the stack */
#endif

/* ================================================================ strings and memory */
/* The builtins lower to memory.copy and memory.fill with bulk memory, so these functions never call themselves. */
void *memcpy(void *dst, const void *src, size_t n) { return __builtin_memcpy(dst, src, n); }
void *memmove(void *dst, const void *src, size_t n) { return __builtin_memmove(dst, src, n); }
void *memset(void *dst, int c, size_t n) { return __builtin_memset(dst, c, n); }

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return (int)x - (int)y;
        if (!x) break;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0; /* C semantics: pads with zeros */
    return dst;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == ch) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, n) == 0) return (char *)hay;
    return NULL;
}

/* ================================================================ ctype */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

/* ================================================================ numbers */
int abs(int v) { return v < 0 ? (int)(0u - (unsigned)v) : v; }

PLC_FN int pl_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

long strtol(const char *s, char **end, int base) {
    const char *p = s;
    unsigned long max;
    unsigned long acc = 0;
    int neg = 0, any = 0, overflow = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && pl_digit((unsigned char)p[2]) < 16) {
        base = 16;
        p += 2;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        if (end) *end = (char *)s;
        return 0;
    }
    max = (unsigned long)(~0UL >> 1) + (neg ? 1UL : 0UL); /* magnitude limit: LONG_MAX, or LONG_MIN's magnitude */
    for (;; p++) {
        int d = pl_digit((unsigned char)*p);
        if (d >= base) break;
        any = 1;
        if (overflow) continue;
        if (acc > (max - (unsigned long)d) / (unsigned long)base) overflow = 1;
        else acc = acc * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)(any ? p : s);
    if (overflow) return neg ? (long)(-(long)(~0UL >> 1) - 1) : (long)(~0UL >> 1);
    if (!any) return 0;
    return neg ? (long)(0UL - acc) : (long)acc;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

/* ================================================================ heap */
#define PL_PAGE 65536u
#define PL_HDR 8u   /* block header: size and tag; the payload starts after it, 8-byte aligned */
#define PL_MIN 32u  /* smallest block: a free block holds two list pointers after the header */
#define PL_USED 0x55aa55aau
#define PL_FREE 0xfeedfaceu

#ifdef PALANU_LIBC_NATIVE_TEST
#ifndef PALANU_LIBC_TEST_ARENA
#define PALANU_LIBC_TEST_ARENA (1u << 20)
#endif
static unsigned char pltest_arena[PALANU_LIBC_TEST_ARENA] __attribute__((aligned(16)));
static size_t pltest_arena_used;
#endif

typedef struct pl_blk {
    uint32_t size; /* the whole block, header included, a multiple of 8 */
    uint32_t tag;  /* PL_USED or PL_FREE */
    struct pl_blk *next, *prev; /* free blocks only; the free list is sorted by address */
} pl_blk_t;

static pl_blk_t *pl_free_head;
static uintptr_t pl_heap_end; /* end of the memory the heap owns so far */
static int pl_heap_ready;

/* Asks the system for pages more memory at the end of the heap. Returns its start, or 0 when refused. */
PLC_FN uintptr_t pl_sys_grow(uint32_t pages) {
    uintptr_t base;
#ifdef PALANU_LIBC_NATIVE_TEST
    if ((size_t)pages > (sizeof pltest_arena - pltest_arena_used) / PL_PAGE) return 0;
    base = (uintptr_t)pltest_arena + pltest_arena_used;
    pltest_arena_used += (size_t)pages * PL_PAGE;
#else
    int old = __builtin_wasm_memory_grow(0, (int)pages);
    if (old < 0) return 0;
    base = (uintptr_t)old * PL_PAGE;
#endif
    pl_heap_end = base + (uintptr_t)pages * PL_PAGE;
    return base;
}

PLC_FN void pl_list_remove(pl_blk_t *b) {
    if (b->prev) b->prev->next = b->next;
    else pl_free_head = b->next;
    if (b->next) b->next->prev = b->prev;
    b->next = b->prev = NULL;
}

/* Puts a block on the free list in address order and merges it with free neighbours. */
PLC_FN void pl_list_insert(pl_blk_t *b) {
    pl_blk_t *p = NULL, *n = pl_free_head;
    while (n && (uintptr_t)n < (uintptr_t)b) {
        p = n;
        n = n->next;
    }
    b->tag = PL_FREE;
    b->prev = p;
    b->next = n;
    if (n) n->prev = b;
    if (p) p->next = b;
    else pl_free_head = b;
    if (n && (uintptr_t)b + b->size == (uintptr_t)n) {
        b->size += n->size;
        pl_list_remove(n);
    }
    if (p && (uintptr_t)p + p->size == (uintptr_t)b) {
        p->size += b->size;
        pl_list_remove(b);
    }
}

PLC_FN int pl_heap_init(void) {
    if (pl_heap_ready) return 1;
    pl_heap_ready = 1;
#ifndef PALANU_LIBC_NATIVE_TEST
    uintptr_t start = ((uintptr_t)__heap_base + 7u) & ~(uintptr_t)7u;
    uintptr_t end = (uintptr_t)__builtin_wasm_memory_size(0) * PL_PAGE;
    pl_heap_end = end;
    if (end >= start + PL_MIN) {
        pl_blk_t *b = (pl_blk_t *)start;
        b->size = (uint32_t)(end - start);
        pl_list_insert(b);
    }
#endif
    return 1;
}

/* Block size for a request of n payload bytes, or 0 when n is too large. */
PLC_FN uint32_t pl_need(size_t n) {
    if (n > 0x7fffffffu) return 0;
    uint32_t need = (((uint32_t)n + 7u) & ~7u) + PL_HDR;
    return need < PL_MIN ? PL_MIN : need;
}

/* Cuts a free block down to need bytes; the tail goes back on the free list. */
PLC_FN void pl_split(pl_blk_t *b, uint32_t need) {
    if (b->size - need < PL_MIN) return;
    pl_blk_t *rest = (pl_blk_t *)((char *)b + need);
    rest->size = b->size - need;
    b->size = need;
    pl_list_insert(rest);
}

void *malloc(size_t n) {
    uint32_t need = pl_need(n);
    if (!need || !pl_heap_init()) return NULL;
    for (;;) {
        pl_blk_t *b = pl_free_head;
        while (b && b->size < need) b = b->next;
        if (b) {
            pl_list_remove(b);
            pl_split(b, need);
            b->tag = PL_USED;
            return (char *)b + PL_HDR;
        }
        uint32_t pages = (need + PL_PAGE - 1) / PL_PAGE;
        uintptr_t base = pl_sys_grow(pages);
        if (!base) return NULL;
        pl_blk_t *g = (pl_blk_t *)base;
        g->size = pages * PL_PAGE;
        pl_list_insert(g); /* merges with a free block that ends where the new memory starts */
    }
}

void free(void *p) {
    if (!p) return;
    pl_blk_t *b = (pl_blk_t *)((char *)p - PL_HDR);
    if (b->tag != PL_USED) return; /* freed already, or not from this heap */
    pl_list_insert(b);
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) {
        free(p);
        return NULL;
    }
    pl_blk_t *b = (pl_blk_t *)((char *)p - PL_HDR);
    uint32_t need = pl_need(n);
    if (b->tag != PL_USED || !need) return NULL;
    if (b->size >= need) {
        if (b->size - need >= PL_MIN) {
            pl_blk_t *rest = (pl_blk_t *)((char *)b + need);
            rest->size = b->size - need;
            b->size = need;
            pl_list_insert(rest);
        }
        return p;
    }
    /* grow in place when the block after it is free and big enough */
    pl_blk_t *nx = (pl_blk_t *)((char *)b + b->size);
    if ((uintptr_t)nx + PL_HDR <= pl_heap_end && nx->tag == PL_FREE && b->size + nx->size >= need) {
        pl_list_remove(nx);
        b->size += nx->size;
        pl_split(b, need);
        return p;
    }
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size - PL_HDR);
    free(p);
    return q;
}

void *calloc(size_t n, size_t size) {
    if (size && n > ((size_t)-1) / size) return NULL;
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

/* ================================================================ text output */
#define PLF_LEFT 1
#define PLF_ZERO 2
#define PLF_PLUS 4
#define PLF_SPACE 8
#define PLF_ALT 16

/* Output goes either into buf (snprintf) or through a 128-byte buffer to fd 1 (printf). len counts every character,
 * so the return value is the length of the full output even when it was cut. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int to_stdout;
    size_t tn;
    char tmp[128];
} pl_out_t;

PLC_FN void pl_write_all(const char *p, size_t n) {
    while (n) {
        int32_t w = pal_write(1, p, (uint32_t)n);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

PLC_FN void pl_flush(pl_out_t *o) {
    if (o->tn) {
        pl_write_all(o->tmp, o->tn);
        o->tn = 0;
    }
}

PLC_FN void pl_put(pl_out_t *o, char c) {
    if (o->to_stdout) {
        o->tmp[o->tn++] = c;
        if (o->tn == sizeof o->tmp) pl_flush(o);
    } else if (o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

PLC_FN void pl_pad(pl_out_t *o, size_t n, char c) {
    while (n--) pl_put(o, c);
}

PLC_FN void pl_puts_n(pl_out_t *o, const char *s, size_t n) {
    while (n--) pl_put(o, *s++);
}

PLC_FN void pl_str(pl_out_t *o, const char *s, size_t n, int width, unsigned flags) {
    size_t pad = (size_t)width > n ? (size_t)width - n : 0;
    if (!(flags & PLF_LEFT)) pl_pad(o, pad, ' ');
    pl_puts_n(o, s, n);
    if (flags & PLF_LEFT) pl_pad(o, pad, ' ');
}

/* One integer: sign, prefix (0x for %p and %#x), zeros for the precision, digits. */
PLC_FN void pl_number(pl_out_t *o, int neg, unsigned long long mag, unsigned base, int upper, int prec, int width,
                      unsigned flags, const char *prefix) {
    static const char lo[] = "0123456789abcdef", hi[] = "0123456789ABCDEF";
    const char *set = upper ? hi : lo;
    char dig[24];
    int nd = 0, isz = mag == 0;
    do {
        dig[nd++] = set[mag % base];
        mag /= base;
    } while (mag);
    if (prec == 0 && isz) nd = 0; /* C: a zero with precision 0 prints nothing */
    char sign = neg ? '-' : (flags & PLF_PLUS) ? '+' : (flags & PLF_SPACE) ? ' ' : 0;
    size_t zeros = prec > nd ? (size_t)(prec - nd) : 0;
    size_t plen = prefix ? strlen(prefix) : 0;
    size_t total = (sign ? 1u : 0u) + plen + zeros + (size_t)nd;
    size_t pad = (size_t)width > total ? (size_t)width - total : 0;
    int zero_pad = (flags & PLF_ZERO) && !(flags & PLF_LEFT) && prec < 0;
    if (!(flags & PLF_LEFT) && !zero_pad) pl_pad(o, pad, ' ');
    if (sign) pl_put(o, sign);
    if (plen) pl_puts_n(o, prefix, plen);
    if (zero_pad) pl_pad(o, pad, '0');
    pl_pad(o, zeros, '0');
    while (nd) pl_put(o, dig[--nd]);
    if (flags & PLF_LEFT) pl_pad(o, pad, ' ');
}

PLC_FN void pl_vformat(pl_out_t *o, const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') {
            pl_put(o, *fmt++);
            continue;
        }
        const char *spec = fmt++;
        unsigned flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= PLF_LEFT;
            else if (*fmt == '0') flags |= PLF_ZERO;
            else if (*fmt == '+') flags |= PLF_PLUS;
            else if (*fmt == ' ') flags |= PLF_SPACE;
            else if (*fmt == '#') flags |= PLF_ALT;
            else break;
        }
        int width = 0, prec = -1, size = 0; /* size: 0 int, 1 long (and z), 2 long long, -1 short, -2 char */
        while (isdigit((unsigned char)*fmt)) {
            if (width < 100000) width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            while (isdigit((unsigned char)*fmt)) {
                if (prec < 100000) prec = prec * 10 + (*fmt - '0');
                fmt++;
            }
        }
        if (*fmt == 'h') {
            fmt++;
            size = -1;
            if (*fmt == 'h') {
                fmt++;
                size = -2;
            }
        } else if (*fmt == 'l') {
            fmt++;
            size = 1;
            if (*fmt == 'l') {
                fmt++;
                size = 2;
            }
        } else if (*fmt == 'z') {
            fmt++;
            size = 1;
        }
        char conv = *fmt;
        if (!conv) { /* a format that ends in the middle of a spec: copied as it is */
            pl_puts_n(o, spec, strlen(spec));
            break;
        }
        fmt++;
        switch (conv) {
        case '%':
            pl_put(o, '%');
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            pl_str(o, &c, 1, width, flags);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t n = 0;
            if (!s) s = "(null)";
            while (s[n] && (prec < 0 || (int)n < prec)) n++;
            pl_str(o, s, n, width, flags);
            break;
        }
        case 'd':
        case 'i': {
            long long v = size == 2 ? va_arg(ap, long long) : size == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
            if (size == -1) v = (short)v;
            else if (size == -2) v = (signed char)v;
            int neg = v < 0;
            unsigned long long mag = neg ? 0ull - (unsigned long long)v : (unsigned long long)v;
            pl_number(o, neg, mag, 10, 0, prec, width, flags, NULL);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long v = size == 2 ? va_arg(ap, unsigned long long)
                                   : size == 1 ? (unsigned long long)va_arg(ap, unsigned long)
                                               : (unsigned long long)va_arg(ap, unsigned int);
            if (size == -1) v = (unsigned short)v;
            else if (size == -2) v = (unsigned char)v;
            const char *pre = NULL;
            if ((flags & PLF_ALT) && v && conv == 'x') pre = "0x";
            if ((flags & PLF_ALT) && v && conv == 'X') pre = "0X";
            pl_number(o, 0, v, conv == 'u' ? 10u : conv == 'o' ? 8u : 16u, conv == 'X', prec, width, flags, pre);
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);
            pl_number(o, 0, (unsigned long long)(uintptr_t)p, 16, 0, -1, width, flags, "0x");
            break;
        }
        default: /* unsupported (%f, %*d, ...): copied as text */
            pl_put(o, '%');
            pl_puts_n(o, spec + 1, (size_t)(fmt - spec - 1));
            break;
        }
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    pl_out_t o = {.buf = buf, .cap = cap};
    pl_vformat(&o, fmt, ap);
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    pl_out_t o = {.to_stdout = 1};
    pl_vformat(&o, fmt, ap);
    pl_flush(&o);
    return (int)o.len;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    pl_write_all(s, strlen(s));
    pl_write_all("\n", 1);
    return 1;
}

int putchar(int c) {
    char ch = (char)c;
    pl_write_all(&ch, 1);
    return (unsigned char)c;
}

/* ================================================================ sorting, searching, random, exit */
PLC_FN void pl_swap(char *a, char *b, size_t n) {
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

PLC_FN void pl_sift(char *base, size_t root, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        pl_swap(base + root * size, base + child * size, size);
        root = child;
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *a = base;
    if (n < 2 || size == 0) return;
    for (size_t i = n / 2; i-- > 0;) pl_sift(a, i, n, size, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        pl_swap(a, a + end * size, size);
        pl_sift(a, 0, end, size, cmp);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    const char *a = base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = a + mid * size;
        int c = cmp(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

static uint32_t pl_rand_state;
static int pl_rand_seeded;

void srand(unsigned int seed) {
    pl_rand_state = seed ? seed : 0x9e3779b9u;
    pl_rand_seeded = 1;
}

int rand(void) {
    if (!pl_rand_seeded) srand((unsigned)pal_now_ms());
    uint32_t x = pl_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    pl_rand_state = x;
    return (int)(x & 0x7fffffffu);
}

void exit(int code) {
    pal_exit(code);
    for (;;) {
    }
}

#endif /* PALANU_LIBC_IMPLEMENTATION */
