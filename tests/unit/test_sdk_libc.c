/* Host test of sdk/palanu_libc.h: every function, snprintf edge cases, printf through pal_write, and a malloc
 * stress run with checksums, coalescing, realloc, and out-of-memory. The header is compiled natively with the
 * PALANU_LIBC_NATIVE_TEST mode (static arena heap, names renamed pltest_*); pal_write is stubbed here. */
#pragma clang diagnostic ignored "-Wunknown-attributes" /* palanu.h declares wasm import attributes */
#define PALANU_LIBC_NATIVE_TEST 1
#define PALANU_LIBC_IMPLEMENTATION 1
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <unistd.h>

#include "palanu_libc.h"

/* ---- stubs for the imports the header uses ---- */
static char cap_buf[8192];
static size_t cap_len;
static int capturing;
static jmp_buf exit_jb;
static int exit_code = -1;

int32_t pal_write(int32_t fd, const void *buf, uint32_t len) {
    if (capturing) {
        for (uint32_t i = 0; i < len && cap_len < sizeof cap_buf - 1; i++) cap_buf[cap_len++] = ((const char *)buf)[i];
    } else {
        ssize_t w = write(fd, buf, len);
        (void)w;
    }
    return (int32_t)len;
}

void pal_exit(int32_t code) {
    exit_code = code;
    longjmp(exit_jb, 1);
}

int64_t pal_now_ms(void) { return 12345; }

/* ---- tiny test harness ---- */
static int checks, failed;

static void fail_at(int line, const char *what) {
    char b[256];
    int n = snprintf(b, sizeof b, "FAIL line %d: %s\n", line, what);
    write(2, b, (size_t)n);
    failed++;
}
#define CHECK(c)                  \
    do {                          \
        checks++;                 \
        if (!(c)) fail_at(__LINE__, #c); \
    } while (0)

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static uint32_t trng = 0x2545F491u;
static uint32_t tr(void) {
    trng ^= trng << 13;
    trng ^= trng >> 17;
    trng ^= trng << 5;
    return trng;
}

/* ---- memory and strings ---- */
static void test_memory_strings(void) {
    char a[16], b[16];
    memset(a, 'x', sizeof a);
    CHECK(a[0] == 'x' && a[15] == 'x');
    memcpy(b, "hello", 6);
    CHECK(streq(b, "hello"));
    char ov[] = "abcdefgh";
    memmove(ov + 2, ov, 5); /* overlap forward */
    CHECK(streq(ov, "ababcdefgh") || memcmp(ov, "ababcde", 7) == 0);
    char ov2[] = "abcdefgh";
    memmove(ov2, ov2 + 2, 5); /* overlap backward */
    CHECK(memcmp(ov2, "cdefgfgh", 5) == 0);
    CHECK(memcmp("abc", "abd", 3) < 0);
    CHECK(memcmp("abd", "abc", 3) > 0);
    CHECK(memcmp("abc", "abd", 2) == 0);
    CHECK(memcmp("", "", 0) == 0);

    CHECK(strlen("") == 0 && strlen("abc") == 3);
    CHECK(strcmp("a", "b") < 0 && strcmp("b", "a") > 0 && strcmp("same", "same") == 0);
    CHECK(strcmp("abc", "ab") > 0);
    CHECK(strncmp("abcd", "abce", 3) == 0 && strncmp("abcd", "abce", 4) < 0);
    CHECK(strncmp("ab", "abc", 5) < 0);
    CHECK(strncmp("x", "y", 0) == 0);

    char d[8];
    strcpy(d, "hi");
    CHECK(streq(d, "hi"));
    memset(d, 'z', sizeof d);
    strncpy(d, "abc", 6);
    CHECK(d[0] == 'a' && d[2] == 'c' && d[3] == 0 && d[4] == 0 && d[5] == 0 && d[6] == 'z' && d[7] == 'z');
    memset(d, 'z', sizeof d);
    strncpy(d, "abcdefghij", 4); /* truncated: no terminator, C semantics */
    CHECK(d[3] == 'd' && d[4] == 'z');

    const char *s = "hello world";
    CHECK(strchr(s, 'o') == s + 4);
    CHECK(strchr(s, 'q') == NULL);
    CHECK(strchr(s, 0) == s + 11);
    CHECK(strrchr(s, 'o') == s + 7);
    CHECK(strrchr(s, 'q') == NULL);
    CHECK(strstr(s, "world") == s + 6);
    CHECK(strstr(s, "xyz") == NULL);
    CHECK(strstr(s, "") == s);
    CHECK(strstr("aaab", "aab") == &"aaab"[1]);
}

static void test_ctype_numbers(void) {
    CHECK(isdigit('0') && isdigit('9') && !isdigit('a'));
    CHECK(isalpha('a') && isalpha('Z') && !isalpha('1'));
    CHECK(isalnum('5') && isalnum('q') && !isalnum('-'));
    CHECK(isspace(' ') && isspace('\t') && isspace('\n') && isspace('\r') && !isspace('x'));
    CHECK(isupper('A') && !isupper('a') && islower('a') && !islower('A'));
    CHECK(isxdigit('f') && isxdigit('F') && isxdigit('9') && !isxdigit('g'));
    CHECK(toupper('a') == 'A' && toupper('1') == '1' && tolower('Q') == 'q' && tolower('.') == '.');
    CHECK(abs(-5) == 5 && abs(5) == 5 && abs(0) == 0);

    CHECK(atoi("  -42xyz") == -42);
    CHECK(atoi("+7") == 7);
    CHECK(atoi("abc") == 0);
    CHECK(strtol("0x1F", NULL, 16) == 31);
    CHECK(strtol("0x10", NULL, 0) == 16);
    CHECK(strtol("010", NULL, 0) == 8);
    CHECK(strtol("1011", NULL, 2) == 11);
    CHECK(strtol("zz", NULL, 36) == 1295);
    CHECK(strtol("-0x7f", NULL, 0) == -127);
    char *end = NULL;
    const char *t = "123abc";
    CHECK(strtol(t, &end, 10) == 123 && end == t + 3);
    t = "xyz";
    CHECK(strtol(t, &end, 10) == 0 && end == t);
    CHECK(strtol("12", NULL, 1) == 0); /* bad base */
    CHECK(strtol("9999999999999999999999", NULL, 10) == (long)(~0UL >> 1));
    CHECK(strtol("-9999999999999999999999", NULL, 10) == -(long)(~0UL >> 1) - 1);
}

/* ---- heap ---- */
static void test_heap_basic(void) {
    CHECK(malloc(0) != NULL);
    for (size_t n = 1; n <= 100; n++) {
        void *p = malloc(n);
        CHECK(p != NULL && ((uintptr_t)p % 8) == 0);
        free(p);
    }
    unsigned char *c = calloc(64, 4);
    CHECK(c != NULL);
    int zero = 1;
    for (int i = 0; i < 256; i++) zero &= c[i] == 0;
    CHECK(zero);
    free(c);
    CHECK(calloc((size_t)-1, 2) == NULL);

    char *p = NULL;
    CHECK(realloc(NULL, 10) != NULL);
    p = malloc(10);
    memcpy(p, "0123456789", 10);
    char *q = realloc(p, 0);
    CHECK(q == NULL);
    p = malloc(16);
    memcpy(p, "keep", 5);
    char *big = realloc(p, 5000);
    CHECK(big != NULL && memcmp(big, "keep", 5) == 0);
    free(big);

    /* shrink keeps the pointer; grow into a free neighbour keeps the pointer */
    char *a = malloc(400), *b = malloc(400);
    CHECK(a && b);
    memset(a, 'a', 400);
    CHECK(realloc(a, 100) == a);
    free(b);
    char *a2 = realloc(a, 600);
    CHECK(a2 == a && a2[0] == 'a' && a2[99] == 'a');
    free(a2);

    /* the heap grows in 64 KB pages on the first request */
    size_t before = pltest_arena_used;
    void *g = malloc(200000);
    CHECK(g != NULL && pltest_arena_used - before == 4 * PL_PAGE);
    free(g);
}

static void test_heap_coalesce_oom(void) {
    enum { N = 100 };
    void *blk[N];
    for (int i = 0; i < N; i++) blk[i] = malloc(1000);
    int all = 1;
    for (int i = 0; i < N; i++) all &= blk[i] != NULL;
    CHECK(all);
    /* free in a shuffled order: a block coalesces with both neighbours when they are free */
    int order[N];
    for (int i = 0; i < N; i++) order[i] = i;
    for (int i = N - 1; i > 0; i--) {
        int j = (int)(tr() % (uint32_t)(i + 1));
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int i = 0; i < N; i++) free(blk[order[i]]);
    void *one = malloc(90000); /* needs one free block of 90 KB: only possible after coalescing */
    CHECK(one != NULL);
    free(one);

    /* out of memory: a request beyond the arena returns NULL and leaves the heap usable */
    void *huge = malloc(2u << 20);
    CHECK(huge == NULL);
    int count = 0;
    void *fill[64];
    for (count = 0; count < 64; count++) {
        fill[count] = malloc(60000);
        if (!fill[count]) break;
    }
    CHECK(count > 5 && count < 64);
    CHECK(malloc(60000 * 40) == NULL);
    char *keep = malloc(32);
    CHECK(keep == NULL || keep != NULL); /* may or may not fit: only checked that it returns */
    free(keep);
    char *old = fill[0];
    if (count > 0) {
        memset(old, 0x5a, 60000);
        CHECK(realloc(old, 2u << 20) == NULL);
        CHECK(((unsigned char *)old)[59999] == 0x5a); /* a failed realloc keeps the block */
    }
    for (int i = 0; i < count; i++) free(fill[i]);
    CHECK(malloc(500000) != NULL);
    /* double free is ignored */
    void *d = malloc(40);
    free(d);
    free(d);
    CHECK(malloc(40) != NULL);
}

static void test_heap_stress(void) {
    enum { SLOTS = 256, OPS = 20000 };
    static unsigned char *ptr[SLOTS];
    static uint32_t len[SLOTS], gen[SLOTS];
    int oom = 0;
    int bad = 0;
    for (int i = 0; i < SLOTS; i++) ptr[i] = NULL, len[i] = 0, gen[i] = 0;
    for (int op = 0; op < OPS; op++) {
        int s = (int)(tr() % SLOTS);
        uint32_t r = tr() % 100;
        if (ptr[s] && r < 40) { /* check and free */
            for (uint32_t i = 0; i < len[s]; i++)
                if (ptr[s][i] != (unsigned char)(s * 31 + i * 7 + gen[s])) bad++;
            free(ptr[s]);
            ptr[s] = NULL;
        } else if (ptr[s] && r < 60) { /* realloc to a new size, checking the kept prefix */
            uint32_t nl = tr() % 3000;
            unsigned char *np = realloc(ptr[s], nl ? nl : 1);
            if (!np) {
                oom++;
                continue;
            }
            uint32_t keep = len[s] < nl ? len[s] : nl;
            for (uint32_t i = 0; i < keep; i++)
                if (np[i] != (unsigned char)(s * 31 + i * 7 + gen[s])) bad++;
            ptr[s] = np;
            len[s] = nl ? nl : 1;
            for (uint32_t i = keep; i < len[s]; i++) np[i] = (unsigned char)(s * 31 + i * 7 + gen[s]);
        } else if (!ptr[s]) {
            uint32_t nl = 1 + tr() % 2000;
            unsigned char *np = malloc(nl);
            if (!np) {
                oom++;
                continue;
            }
            ptr[s] = np;
            len[s] = nl;
            gen[s] = tr() & 0xff;
            for (uint32_t i = 0; i < nl; i++) np[i] = (unsigned char)(s * 31 + i * 7 + gen[s]);
        }
    }
    for (int i = 0; i < SLOTS; i++) {
        if (!ptr[i]) continue;
        for (uint32_t k = 0; k < len[i]; k++)
            if (ptr[i][k] != (unsigned char)(i * 31 + k * 7 + gen[i])) bad++;
        free(ptr[i]);
        ptr[i] = NULL;
    }
    CHECK(bad == 0);
    CHECK(oom < OPS / 2);
    /* after everything is freed the heap is one block again: a large request fits without new pages */
    size_t used = pltest_arena_used;
    void *big = malloc(300000);
    CHECK(big != NULL && pltest_arena_used == used);
    free(big);
    if (failed == 0) {
        char m[96];
        snprintf(m, sizeof m, "  stress: %d ops, %d out-of-memory, %u KB of arena used\n", OPS, oom,
                 (unsigned)(pltest_arena_used / 1024));
        pal_write(1, m, strlen(m));
    }
}

/* ---- snprintf edge cases ---- */
static void test_snprintf(void) {
    char b[64];
    int n;
    n = snprintf(b, 0, "abc");
    CHECK(n == 3);
    memset(b, '#', sizeof b);
    n = snprintf(b, 4, "abcdef");
    CHECK(n == 6 && streq(b, "abc"));
    n = snprintf(NULL, 0, "%d", 123);
    CHECK(n == 3);
    n = snprintf(b, 1, "xyz");
    CHECK(n == 3 && b[0] == 0);
    n = snprintf(b, sizeof b, "%d|%i|%u", -7, 42, 4000000000u);
    CHECK(n == 2 + 1 + 2 + 1 + 10 && streq(b, "-7|42|4000000000"));
    CHECK(snprintf(b, sizeof b, "%5d|%-5d|%05d|%-05d|", 42, 42, -42, 42) >= 0 && streq(b, "   42|42   |-0042|42   |"));
    CHECK(snprintf(b, sizeof b, "%+d % d %+d", 5, 5, -5) >= 0 && streq(b, "+5  5 -5"));
    CHECK(snprintf(b, sizeof b, "%.3d|%.0d|%5.2d|", 7, 0, 3) >= 0 && streq(b, "007||   03|"));
    CHECK(snprintf(b, sizeof b, "%x %X %#x %#X %#x", 255, 255, 255, 255, 0) >= 0 && streq(b, "ff FF 0xff 0XFF 0"));
    CHECK(snprintf(b, sizeof b, "%08x|%-6x|", 0xbeef, 0xa) >= 0 && streq(b, "0000beef|a     |"));
    CHECK(snprintf(b, sizeof b, "%c%c|%3c|%-3c|", 'o', 'k', 'x', 'y') >= 0 && streq(b, "ok|  x|y  |"));
    CHECK(snprintf(b, sizeof b, "%s|%5s|%-5s|%.2s|%10.3s|", "ab", "ab", "ab", "abcdef", "abcdef") >= 0 &&
          streq(b, "ab|   ab|ab   |ab|       abc|"));
    CHECK(snprintf(b, sizeof b, "%s", (const char *)NULL) >= 0 && streq(b, "(null)"));
    CHECK(snprintf(b, sizeof b, "100%%") >= 0 && streq(b, "100%"));
    CHECK(snprintf(b, sizeof b, "%ld %lu", -123456L, 123456UL) >= 0 && streq(b, "-123456 123456"));
    CHECK(snprintf(b, sizeof b, "%lld", LLONG_MIN) >= 0 && streq(b, "-9223372036854775808"));
    CHECK(snprintf(b, sizeof b, "%llu %llx", 0xffffffffffffffffULL, 0x123456789abcULL) >= 0 &&
          streq(b, "18446744073709551615 123456789abc"));
    CHECK(snprintf(b, sizeof b, "%hd %hhu", 70000, 257) >= 0 && streq(b, "4464 1"));
    CHECK(snprintf(b, sizeof b, "%zu", (size_t)77) >= 0 && streq(b, "77"));
    CHECK(snprintf(b, sizeof b, "%p", (void *)0) >= 0 && streq(b, "0x0"));
    CHECK(snprintf(b, sizeof b, "%p", (void *)0x1000) >= 0 && streq(b, "0x1000"));
    CHECK(snprintf(b, sizeof b, "%f|%*d", 1.0, 3) >= 0 && streq(b, "%f|%*d"));
    CHECK(snprintf(b, sizeof b, "tail %") >= 0 && streq(b, "tail %"));
    CHECK(snprintf(b, sizeof b, "%d %s", 1, "two") == 5);
    /* truncation returns the full length and keeps the buffer terminated */
    char small[5];
    n = snprintf(small, sizeof small, "%d", 1234567);
    CHECK(n == 7 && streq(small, "1234"));
    n = snprintf(b, sizeof b, "%0*d", 3, 1);
    CHECK(n >= 0);
}

/* ---- printf family through pal_write(1, ...) ---- */
static void test_printf(void) {
    capturing = 1;
    cap_len = 0;
    int n = printf("%d-%s\n", 5, "x");
    CHECK(n == 4);
    CHECK(cap_len == 4 && memcmp(cap_buf, "5-x\n", 4) == 0);
    cap_len = 0;
    CHECK(puts("hi") >= 0 && cap_len == 3 && memcmp(cap_buf, "hi\n", 3) == 0);
    cap_len = 0;
    CHECK(putchar('A') == 'A' && cap_len == 1 && cap_buf[0] == 'A');
    /* longer than the 128-byte batch: flushes in pieces and still returns the full length */
    char longs[300];
    memset(longs, 'q', sizeof longs);
    longs[299] = 0;
    cap_len = 0;
    n = printf("%s", longs);
    CHECK(n == 299 && cap_len == 299 && cap_buf[0] == 'q' && cap_buf[298] == 'q');
    cap_len = 0;
    n = printf("%c%s|%5.1f", 'z', "", 0.0); /* no floats: the spec is copied as text */
    CHECK(n == 7 && cap_len == 7 && memcmp(cap_buf, "z|%5.1f", 7) == 0);
    capturing = 0;
}

/* ---- qsort, bsearch, rand, exit ---- */
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

typedef struct {
    int key;
    char tag[8];
} rec_t;

static int cmp_rec(const void *a, const void *b) { return cmp_int(a, b); }

static void test_sort_search_rand_exit(void) {
    enum { N = 300 };
    int v[N];
    long sum = 0;
    for (int i = 0; i < N; i++) {
        v[i] = (int)(tr() % 1000) - 500;
        sum += v[i];
    }
    qsort(v, N, sizeof v[0], cmp_int);
    int sorted = 1;
    long sum2 = 0;
    for (int i = 0; i < N; i++) {
        sum2 += v[i];
        if (i && v[i - 1] > v[i]) sorted = 0;
    }
    CHECK(sorted && sum == sum2);
    int key = v[77];
    int *hit = bsearch(&key, v, N, sizeof v[0], cmp_int);
    CHECK(hit != NULL && *hit == key);
    key = 100000;
    CHECK(bsearch(&key, v, N, sizeof v[0], cmp_int) == NULL);
    qsort(v, 0, sizeof v[0], cmp_int);
    qsort(v, 1, sizeof v[0], cmp_int);

    rec_t recs[5] = {{5, "e"}, {1, "a"}, {4, "d"}, {2, "b"}, {3, "c"}};
    qsort(recs, 5, sizeof recs[0], cmp_rec);
    CHECK(recs[0].key == 1 && recs[4].key == 5 && recs[2].tag[0] == 'c');
    int k3 = 3;
    rec_t *r3 = bsearch(&k3, recs, 5, sizeof recs[0], cmp_rec);
    CHECK(r3 == &recs[2]);

    srand(42);
    int first = rand();
    srand(42);
    CHECK(rand() == first && first >= 0 && first <= RAND_MAX);
    int distinct = 0;
    for (int i = 0; i < 50; i++) distinct += rand() != first;
    CHECK(distinct > 40);
    srand(0);
    CHECK(rand() >= 0);

    if (setjmp(exit_jb) == 0) {
        exit(7);
        CHECK(0);
    } else {
        CHECK(exit_code == 7);
    }
}

int main(void) {
    test_memory_strings();
    test_ctype_numbers();
    test_heap_basic();
    test_heap_coalesce_oom();
    test_heap_stress();
    test_snprintf();
    test_printf();
    test_sort_search_rand_exit();
    char summary[96];
    int n = snprintf(summary, sizeof summary, "sdk libc: %d checks, %d failed\n", checks, failed);
    pal_write(1, summary, (uint32_t)n);
    return failed ? 1 : 0;
}
