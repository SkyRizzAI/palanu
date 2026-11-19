/* Nema log: one ring buffer in memory plus an optional console sink. */
#ifndef NEMA_LOG_H
#define NEMA_LOG_H

#include "nema/types.h"

typedef enum { NEMA_LOG_ERROR = 0, NEMA_LOG_WARN, NEMA_LOG_INFO, NEMA_LOG_DEBUG } nema_log_level_t;

void nema_log(nema_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define NEMA_LOGE(tag, ...) nema_log(NEMA_LOG_ERROR, tag, __VA_ARGS__)
#define NEMA_LOGW(tag, ...) nema_log(NEMA_LOG_WARN, tag, __VA_ARGS__)
#define NEMA_LOGI(tag, ...) nema_log(NEMA_LOG_INFO, tag, __VA_ARGS__)
#define NEMA_LOGD(tag, ...) nema_log(NEMA_LOG_DEBUG, tag, __VA_ARGS__)

/* Console sink: on by default. Turn it off when a shell owns the console. */
void nema_log_set_console(bool on);
void nema_log_set_level(nema_log_level_t max_level);
/* One more reader of every line, after the ring and the serial console: the system console shows the boot with it. The line
 * has no newline; the sink runs on the logging task and must not log. NULL removes it. */
typedef void (*nema_log_sink_t)(nema_log_level_t level, const char *line, size_t len);
void nema_log_set_sink(nema_log_sink_t sink);

/* Bytes logged since boot: a reader keeps a position and asks for what came after it (nema_log_read). */
uint64_t nema_log_total(void);
/* Copies text logged at or after *pos, up to cap bytes, into dst and moves *pos past it. When the ring no longer holds some of it,
 * the copy starts at the oldest byte it holds and *lost (may be NULL) gets the bytes skipped. Returns the bytes copied. */
size_t nema_log_read(uint64_t *pos, char *dst, size_t cap, uint64_t *lost);

/* Copy the most recent text (whole lines) into dst. Returns bytes written. */
size_t nema_log_snapshot(char *dst, size_t cap);

#endif
