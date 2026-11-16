/* Shell history kept across restarts. Every interactive shell (Terminal tabs, the serial console) adds the lines it runs
 * here; a new shell starts with the last of them, so the up arrow reaches commands from before the restart.
 *
 * Writing is lazy and never in the way: pal_history_add only changes memory. The file is rewritten by a shell while it
 * waits at its prompt with nothing typed for PAL_HISTORY_IDLE_MS (pal_history_flush_in_ms tells it how long to wait), and
 * when it exits. No task of its own, so no stack in internal RAM. The file is replaced whole (written to a temporary file,
 * then renamed), so a power cut leaves the old history or the new one, never half of it. Lines run in the last
 * PAL_HISTORY_IDLE_MS before a power cut can be lost.
 *
 * It lives in internal flash (/data), not on the microSD card: a card can be missing or swapped, and the history is only a
 * few KB. LittleFS spreads the writes, and batching keeps them to one per idle pause. */
#ifndef PAL_HISTORY_H
#define PAL_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"

#define PAL_HISTORY_MAX 100          /* lines kept, oldest dropped first */
#define PAL_HISTORY_LINE 200         /* bytes per line, the NUL included (the shell's line limit) */
#define PAL_HISTORY_IDLE_MS 2000     /* a quiet prompt this long writes the file */
#define PAL_HISTORY_RETRY_MS 30000   /* after a failed write */
#define PAL_HISTORY_FILE "/data/sh/history"

/* Adds a line (cut to PAL_HISTORY_LINE - 1). Empty lines, lines of spaces, a repeat of the last line, and lines starting
 * with a space (the shell convention for "do not remember this") are not kept. Memory only. */
void pal_history_add(const char *line);

int pal_history_count(void);
/* Line i, 0 the oldest. False when i is out of range. */
bool pal_history_get(int i, char *out, size_t cap);

/* -1 when the file is up to date; otherwise how many ms of quiet are still needed before pal_history_flush (0: now). */
int32_t pal_history_flush_in_ms(void);
/* Writes the file when it is not up to date. NEMA_OK also when there was nothing to write or another shell is writing. */
nema_err_t pal_history_flush(void);

/* Tests: forget everything and use path for the file (NULL: PAL_HISTORY_FILE); a clock in microseconds (NULL: the
 * system's). The next call loads the file again. */
void pal_history_reset_for_test(const char *path, uint64_t (*now_us)(void));

#endif
