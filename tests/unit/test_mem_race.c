/* Admission under contention: several tasks allocate and free internal RAM against a small floor, with scheduling
 * jitter inside the kernel's locks to widen race windows. After every operation each task checks, under the kernel
 * lock, that the committed free memory is still at or above the floor. The check runs under the lock, so it never sees
 * the transient trial allocations that admission makes.
 *
 * Its own binary on purpose: test_mem makes critical allocations that dip below the floor on purpose. */
#include <stdint.h>
#include <stdlib.h>

#include "nema/nema.h"
#include "test.h"

#define THREADS 4
#define ITERATIONS 10000
#define FLOOR (64u * 1024u)
#define SLACK (96u * 1024u) /* free bytes left above the floor before the threads start */

static nema_owner_t OW;
static nema_port_sem_t *done, *start;
static int violations; /* written with atomic adds from the tasks */

static void hammer(void *arg) {
    nema_port_sem_take(start, -1);
    unsigned seed = (unsigned)(uintptr_t)arg * 7919u + 1u;
    void *held[8] = {0};
    for (int i = 0; i < ITERATIONS; i++) {
        seed = seed * 1103515245u + 12345u;
        size_t size = 1024u + (seed >> 8) % (24u * 1024u);
        int slot = (int)((seed >> 4) & 7u);
        if (held[slot]) {
            nema_free(held[slot]);
            held[slot] = NULL;
        }
        held[slot] = nema_alloc(NEMA_MEM_INTERNAL, OW, size); /* may be refused: that is allowed */
        if (!nema_mem_internal_can_spare(0)) __atomic_add_fetch(&violations, 1, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < 8; i++) nema_free(held[i]);
    nema_port_sem_give(done);
}

TEST(floor_holds_under_concurrent_admission) {
    nema_mem_set_floor(FLOOR);
    size_t free_now = nema_port_mem_free(NEMA_MEM_INTERNAL);
    CHECK(free_now > FLOOR + SLACK + 8192);
    /* critical hog: leaves exactly SLACK above the floor, so the race is decided in the first few allocations */
    void *hog = nema_alloc_ex(NEMA_MEM_INTERNAL, OW, free_now - FLOOR - SLACK, 0, NEMA_ALLOC_CRITICAL);
    CHECK(hog != NULL);
    CHECK(nema_mem_internal_can_spare(0));
    done = nema_port_sem_create(0, THREADS);
    start = nema_port_sem_create(0, THREADS);
    for (int i = 0; i < THREADS; i++) {
        nema_task_desc_t d = {.name = "hammer", .fn = hammer, .arg = (void *)(uintptr_t)(i + 1), .stack = 4096,
                              .prio = 5, .core = -1, .role = NEMA_TASK_SERVICE, .owner = OW};
        CHECK(nema_task_create(&d) == NEMA_OK);
    }
    for (int i = 0; i < THREADS; i++) nema_port_sem_give(start);
    for (int i = 0; i < THREADS; i++) CHECK(nema_port_sem_take(done, 20000) == NEMA_OK);
    if (violations) printf("    %d committed states below the floor\n", violations);
    CHECK_EQ(violations, 0);
    nema_free(hog);
    nema_port_sem_destroy(done);
    nema_port_sem_destroy(start);
    nema_mem_set_floor(NEMA_MEM_FLOOR_DEFAULT);
}

int main(void) {
    setenv("NEMA_HOST_JITTER", "50", 1); /* microseconds; read by the host port on first lock */
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("racetest");
    RUN(floor_holds_under_concurrent_admission);
    return TEST_DONE();
}
