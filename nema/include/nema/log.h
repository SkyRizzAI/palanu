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

/* Copy the most recent text (whole lines) into dst. Returns bytes written. */
size_t nema_log_snapshot(char *dst, size_t cap);

#endif
