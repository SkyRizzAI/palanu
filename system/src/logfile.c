/* The log on flash (the log-file service, docs/STATE.md): what the kernel logs is appended to /data/log/system.log, in batches
 * (every 10 s, or sooner when 2 KB are waiting) so the flash is written seldom. At 128 KB the file becomes system.log.1 (the one
 * before is dropped), so the log survives a restart and a crash and takes at most 256 KB. The setting log.file turns it off. */
#include <stdio.h>
#include <string.h>

#include "nema/log.h"
#include "nema/nema.h"
#include "nema/task.h"
#include "nema/vfs.h"
#include "pal/settings.h"

#define DIR_LOG "/data/log"
#define FILE_LOG DIR_LOG "/system.log"
#define FILE_OLD DIR_LOG "/system.log.1"
#define ROTATE_AT (128u * 1024u)
#define BATCH 2048u
#define EVERY_MS 10000u

static volatile bool s_run, s_running;
static uint64_t s_pos;

static void append(const char *buf, size_t n) {
    nema_stat_t st;
    if (nema_fs_stat(FILE_LOG, &st) == NEMA_OK && st.size + n > ROTATE_AT) {
        (void)nema_fs_unlink(FILE_OLD);
        (void)nema_fs_rename(FILE_LOG, FILE_OLD);
    }
    nema_obj_t *f;
    if (nema_fs_open(FILE_LOG, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_APPEND, &f) != NEMA_OK) return;
    (void)nema_obj_write(f, buf, n, 2000);
    nema_obj_unref(f);
}

static void flush(char *buf) {
    for (;;) {
        uint64_t lost = 0;
        size_t n = nema_log_read(&s_pos, buf, BATCH, &lost);
        if (lost) {
            char note[64];
            int k = snprintf(note, sizeof(note), "-- %llu bytes of log were dropped before they were saved --\n", (unsigned long long)lost);
            append(note, (size_t)k);
        }
        if (n == 0) return;
        append(buf, n);
        if (n < BATCH) return;
    }
}

static void task(void *arg) {
    (void)arg;
    char *buf = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, BATCH);
    if (buf) {
        (void)nema_fs_mkdir(DIR_LOG);
        append("-- boot --\n", 11);
        uint32_t waited = 0;
        while (s_run) {
            nema_port_sleep_ms(500);
            waited += 500;
            if (waited >= EVERY_MS || nema_log_total() - s_pos >= BATCH || !s_run) {
                waited = 0;
                flush(buf);
            }
        }
        flush(buf);
        nema_free(buf);
    }
    s_running = false;
}

nema_err_t psys_logfile_start(void) {
    if (s_running) return NEMA_OK;
    if (!psys_settings_int("log.file", 1)) return NEMA_OK; /* off: the service runs and writes nothing */
    s_run = true;
    s_running = true;
    s_pos = 0; /* from the start of the ring: the boot log is in it */
    nema_task_desc_t d = {.name = "logfile", .fn = task, .stack = 4096, .prio = 2, .core = -1, .role = NEMA_TASK_SYSTEM};
    nema_err_t e = nema_task_create(&d);
    if (e != NEMA_OK) s_running = s_run = false;
    return e;
}

void psys_logfile_stop(void) {
    s_run = false;
    for (int i = 0; i < 40 && s_running; i++) nema_port_sleep_ms(50); /* its last batch goes to the file */
}
