/* The Logs app's model (no drawing): the lines of the nema log ring and of the service logs, parsed into entries, merged in time
 * order, filtered by level and component, and the "clear" marker. Lines have the form "E (1234) tag: message" (the level letter
 * E, W, I or D, the uptime in milliseconds, the component, the message); service lines are written in the same form. Tests:
 * tests/unit/test_logs.c. */
#ifndef LOGS_MODEL_H
#define LOGS_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGS_MAX 400          /* entries kept (the newest) */
#define LOGS_COMP 16          /* component names kept, truncated */
#define LOGS_SRC_RING 0       /* the nema log ring */
#define LOGS_SRC_SVC 1        /* a service's log (psys_svc_log_read) */

/* Level filters, lowest first: all lines; info and above; warnings and above; errors only. */
enum { LOGS_F_ALL = 0, LOGS_F_INFO, LOGS_F_WARN, LOGS_F_ERROR, LOGS_F_COUNT };
const char *logs_filter_name(int f);

typedef struct {
    uint32_t off;               /* the line in its text buffer */
    uint16_t len;               /* its length, newline excluded */
    uint8_t src;               /* LOGS_SRC_* */
    uint8_t level;              /* 0 error, 1 warn, 2 info, 3 debug (nema_log_level_t) */
    uint32_t ms;                /* uptime in milliseconds */
    char comp[LOGS_COMP];
} logs_ref_t;

typedef struct {
    uint8_t level;
    uint32_t ms;
    char comp[LOGS_COMP];
    const char *msg;
    size_t msglen;
} logs_line_t;

/* Parses one line. False when it does not have the form above. */
bool logs_parse(const char *line, size_t len, logs_line_t *out);
/* "HH:MM:SS" from uptime milliseconds (hours wrap at 100). */
void logs_hms(uint32_t ms, char *out, size_t cap);
/* The offset after the last occurrence of marker (a whole line) in buf, or 0 when marker is empty or absent. */
size_t logs_after_marker(const char *buf, size_t len, const char *marker);
/* Appends the lines of buf[from..len) as entries after n. Returns the new count (entries past max are dropped, oldest first). */
int logs_add_lines(logs_ref_t *refs, int n, int max, const char *buf, size_t len, size_t from, uint8_t src);
/* Sorts entries by time (stable). */
void logs_sort(logs_ref_t *refs, int n);
/* Entries that pass the filters, as indices into refs. Returns the count. */
int logs_visible(const logs_ref_t *refs, int n, int level_filter, const char *comp, int *vis, int max);
/* Distinct components in refs, in first-seen order. Returns the count. */
int logs_components(const logs_ref_t *refs, int n, char names[][LOGS_COMP], int max);
/* The first visible row for follow mode: the last rows of the list. */
int logs_follow_top(int nvis, int rows);

#endif
