#include <stdint.h>
#include <string.h>

#include "nema/nema.h"
#include "test.h"

static nema_owner_t OW;

TEST(accounting_follows_alloc_and_free) {
    nema_owner_stat_t s;
    void *p = nema_alloc(NEMA_MEM_EXTERNAL, OW, 100);
    CHECK(p != NULL);
    CHECK(nema_mem_owner_stat(OW, &s));
    CHECK_EQ(s.used[NEMA_MEM_EXTERNAL], 100);
    CHECK_EQ(s.allocs, 1);
    CHECK_EQ(nema_alloc_size(p), 100);
    nema_free(p);
    nema_mem_owner_stat(OW, &s);
    CHECK_EQ(s.used[NEMA_MEM_EXTERNAL], 0);
    CHECK_EQ(s.allocs, 0);
    CHECK(s.peak >= 100);
}

TEST(any_prefers_external) {
    nema_owner_stat_t s;
    void *p = nema_alloc(NEMA_MEM_ANY, OW, 64);
    nema_mem_owner_stat(OW, &s);
    CHECK_EQ(s.used[NEMA_MEM_EXTERNAL], 64);
    CHECK_EQ(s.used[NEMA_MEM_INTERNAL], 0);
    nema_free(p);
}

TEST(aligned_allocation_is_aligned_and_freeable) {
    void *p = nema_alloc_ex(NEMA_MEM_EXTERNAL, OW, 1000, 64, 0);
    CHECK(p != NULL);
    CHECK_EQ((uintptr_t)p % 64, 0);
    memset(p, 0xAB, 1000);
    nema_free(p);
    void *q = nema_alloc_ex(NEMA_MEM_INTERNAL, OW, 33, 32, NEMA_ALLOC_ZERO);
    CHECK_EQ((uintptr_t)q % 32, 0);
    CHECK_EQ(((uint8_t *)q)[32], 0);
    nema_free(q);
}

TEST(internal_floor_is_enforced) {
    size_t floor = nema_mem_floor();
    size_t free_now = nema_port_mem_free(NEMA_MEM_INTERNAL);
    CHECK(free_now > floor + 8192);
    /* one byte too many past the floor fails ... */
    void *too_big = nema_alloc(NEMA_MEM_INTERNAL, OW, free_now - floor + 1);
    CHECK(too_big == NULL);
    /* ... unless the caller is critical */
    void *crit = nema_alloc_ex(NEMA_MEM_INTERNAL, OW, free_now - floor + 1024, 0, NEMA_ALLOC_CRITICAL);
    CHECK(crit != NULL);
    nema_free(crit);
    /* a block that leaves the floor intact works */
    void *ok = nema_alloc(NEMA_MEM_INTERNAL, OW, 4096);
    CHECK(ok != NULL);
    nema_free(ok);
    nema_owner_stat_t s;
    nema_mem_owner_stat(OW, &s);
    CHECK(s.fails >= 1);
}

/* A fake reclaimer: counts its calls and, while `hog` is set, frees it. Registered once, for every test below. */
static struct {
    int calls;
    void *hog;
} R;

static size_t fake_reclaim(void *ctx, size_t wanted) {
    (void)ctx, (void)wanted;
    R.calls++;
    if (!R.hog) return 0;
    size_t n = nema_alloc_size(R.hog);
    nema_free(R.hog);
    R.hog = NULL;
    return n;
}

TEST(reclaimer_runs_before_refusing) {
    size_t floor = nema_mem_floor();
    size_t free_now = nema_port_mem_free(NEMA_MEM_INTERNAL);
    /* critical, so the hog itself is admitted; it leaves only 2 KB above the floor */
    R.hog = nema_alloc_ex(NEMA_MEM_INTERNAL, OW, free_now - floor - 2048, 0, NEMA_ALLOC_CRITICAL);
    CHECK(R.hog != NULL);
    int before = R.calls;
    void *p = nema_alloc(NEMA_MEM_INTERNAL, OW, 64 * 1024); /* refused until the reclaimer frees the hog */
    CHECK(p != NULL);
    CHECK(R.calls > before);
    CHECK(R.hog == NULL);
    nema_free(p);
}

TEST(refusal_stays_refused_when_reclaimer_frees_nothing) {
    size_t floor = nema_mem_floor();
    size_t free_now = nema_port_mem_free(NEMA_MEM_INTERNAL);
    R.hog = NULL;
    int before = R.calls;
    void *p = nema_alloc(NEMA_MEM_INTERNAL, OW, free_now); /* more than the floor allows */
    CHECK(p == NULL);
    CHECK(R.calls > before); /* the refusal was not immediate: reclaimers ran first */
    (void)floor;
}

/* Host note: the port simulates the largest free block as 3/4 of the free bytes. On the board it is the real largest
 * block; the arithmetic below is the same either way, the numbers are what differ. */
TEST(large_request_needs_one_big_free_block) {
    size_t floor = nema_mem_floor();
    size_t free_now = nema_port_mem_free(NEMA_MEM_INTERNAL);
    size_t keep = 200u * 1024u;
    CHECK(free_now > keep + floor + 8192);
    void *hog = nema_alloc_ex(NEMA_MEM_INTERNAL, OW, free_now - keep, 0, NEMA_ALLOC_CRITICAL);
    CHECK(hog != NULL);
    /* 160 KB fits under the floor (200 KB free, floor 32 KB) but not as one block (largest is about 150 KB) */
    void *big = nema_alloc(NEMA_MEM_INTERNAL, OW, 160u * 1024u);
    CHECK(big == NULL);
    /* a 3 KB request is below the block rule and still goes through */
    void *small = nema_alloc(NEMA_MEM_INTERNAL, OW, 3u * 1024u);
    CHECK(small != NULL);
    nema_free(small);
    nema_free(hog);
    void *big2 = nema_alloc(NEMA_MEM_INTERNAL, OW, 160u * 1024u);
    CHECK(big2 != NULL);
    nema_free(big2);
}

TEST(report_names_owners) {
    void *p = nema_alloc(NEMA_MEM_EXTERNAL, OW, 512);
    char buf[2048];
    size_t n = nema_mem_report(buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "internal") != NULL);
    CHECK(strstr(buf, "memtest") != NULL);
    CHECK(strstr(buf, "floor") != NULL);
    CHECK(strstr(buf, "quota") != NULL);
    nema_free(p);
}

TEST(owner_register_is_idempotent) {
    CHECK_EQ(nema_owner_register("memtest"), OW);
    CHECK(nema_owner_register("other-sys") != OW);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("memtest");
    CHECK(nema_mem_add_reclaimer(fake_reclaim, NULL, 0) == NEMA_OK);
    RUN(accounting_follows_alloc_and_free);
    RUN(any_prefers_external);
    RUN(aligned_allocation_is_aligned_and_freeable);
    RUN(internal_floor_is_enforced);
    RUN(reclaimer_runs_before_refusing);
    RUN(refusal_stays_refused_when_reclaimer_frees_nothing);
    RUN(large_request_needs_one_big_free_block);
    RUN(report_names_owners);
    RUN(owner_register_is_idempotent);
    return TEST_DONE();
}
