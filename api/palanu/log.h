/* Logging. Lines go to the system log (kept in memory, and printed on the console when that is on), tagged with the
 * app's id, so the log shows which app wrote each line. */
#ifndef PALANU_API_LOG_H
#define PALANU_API_LOG_H

#include "annot.h"

#define PAL_LOG_ERROR 0
#define PAL_LOG_WARN 1
#define PAL_LOG_INFO 2
#define PAL_LOG_DEBUG 3

PAL_SINCE(2) PAL_WASM(log)
PAL_DOC("Writes msg (NUL-terminated) to the system log at a PAL_LOG_* level. Returns 0, or a negative error for an unknown level.")
int32_t pal_log(int32_t level, const char *msg);

#endif
