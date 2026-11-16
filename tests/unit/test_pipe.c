#include <string.h>

#include "nema/nema.h"
#include "test.h"

static nema_owner_t OW;

TEST(basic_read_write) {
    nema_obj_t *r, *w;
    CHECK(nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w) == NEMA_OK);
    CHECK_EQ(nema_obj_write(w, "hello", 5, 0), 5);
    char buf[16] = {0};
    CHECK_EQ(nema_obj_read(r, buf, sizeof(buf), 0), 5);
    CHECK(strcmp(buf, "hello") == 0);
    CHECK_EQ(nema_obj_read(r, buf, sizeof(buf), 0), NEMA_ERR_AGAIN);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

TEST(full_pipe_returns_partial_then_would_block) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    char big[32];
    memset(big, 'x', sizeof(big));
    CHECK_EQ(nema_obj_write(w, big, sizeof(big), 0), 16);
    CHECK_EQ(nema_obj_write(w, big, 1, 0), NEMA_ERR_AGAIN);
    char out[8];
    CHECK_EQ(nema_obj_read(r, out, sizeof(out), 0), 8);
    CHECK_EQ(nema_obj_write(w, big, 8, 0), 8);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

TEST(ring_wraps_correctly) {
    nema_obj_t *r, *w;
    nema_pipe_create(8, NEMA_MEM_ANY, OW, &r, &w);
    char out[8];
    for (int round = 0; round < 20; round++) {
        char in[6];
        for (int i = 0; i < 6; i++) in[i] = (char)('a' + (round + i) % 26);
        CHECK_EQ(nema_obj_write(w, in, 6, 0), 6);
        CHECK_EQ(nema_obj_read(r, out, 6, 0), 6);
        CHECK(memcmp(in, out, 6) == 0);
    }
    nema_obj_unref(r);
    nema_obj_unref(w);
}

TEST(eof_after_writer_closes) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_obj_write(w, "ab", 2, 0);
    nema_obj_unref(w);
    char b[4];
    CHECK_EQ(nema_obj_read(r, b, 4, 0), 2);
    CHECK_EQ(nema_obj_read(r, b, 4, 0), 0); /* end of stream */
    CHECK((r->ops->poll(r, NEMA_POLL_IN) & NEMA_POLL_HUP) != 0);
    nema_obj_unref(r);
}

TEST(write_to_closed_reader_is_broken_pipe) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_obj_unref(r);
    CHECK_EQ(nema_obj_write(w, "x", 1, 0), NEMA_ERR_PIPE);
    nema_obj_unref(w);
}

TEST(read_times_out) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    char b[4];
    uint64_t t0 = nema_port_time_us();
    CHECK_EQ(nema_obj_read(r, b, 4, 40), NEMA_ERR_TIMEOUT);
    uint64_t dt = nema_port_time_us() - t0;
    CHECK(dt >= 35000 && dt < 400000);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

static nema_obj_t *g_w;
static void delayed_writer(void *arg) {
    (void)arg;
    nema_port_sleep_ms(60);
    nema_obj_write(g_w, "wake", 4, 0);
}

TEST(blocking_read_is_woken_by_writer_task) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    g_w = w;
    nema_task_desc_t d = {.name = "writer", .fn = delayed_writer, .stack = 4096, .role = NEMA_TASK_SYSTEM, .owner = OW};
    CHECK(nema_task_create(&d) == NEMA_OK);
    char b[8] = {0};
    CHECK_EQ(nema_obj_read(r, b, sizeof(b), 2000), 4);
    CHECK(strcmp(b, "wake") == 0);
    nema_port_sleep_ms(30);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

TEST(blocked_writer_resumes_when_reader_drains) {
    nema_obj_t *r, *w;
    nema_pipe_create(8, NEMA_MEM_ANY, OW, &r, &w);
    char big[8];
    memset(big, 'z', sizeof(big));
    nema_obj_write(w, big, 8, 0);
    /* nothing drains: a 30 ms blocking write must time out without losing the pipe */
    CHECK_EQ(nema_obj_write(w, "q", 1, 30), NEMA_ERR_TIMEOUT);
    char out[8];
    nema_obj_read(r, out, 8, 0);
    CHECK_EQ(nema_obj_write(w, "q", 1, 30), 1);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

TEST(pipe_memory_returns) {
    nema_owner_stat_t s;
    nema_mem_owner_stat(OW, &s);
    CHECK_EQ(s.allocs, 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("pipetest");
    RUN(basic_read_write);
    RUN(full_pipe_returns_partial_then_would_block);
    RUN(ring_wraps_correctly);
    RUN(eof_after_writer_closes);
    RUN(write_to_closed_reader_is_broken_pipe);
    RUN(read_times_out);
    RUN(blocking_read_is_woken_by_writer_task);
    RUN(blocked_writer_resumes_when_reader_drains);
    RUN(pipe_memory_returns);
    return TEST_DONE();
}
