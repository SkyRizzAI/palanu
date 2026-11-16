/* PalanuOS radio manager: Wi-Fi and BLE are off at boot and start on demand (docs/RADIO.md).
 *
 * A radio runs while it has holders. The first request starts it after an admission check (free internal RAM and the
 * largest free block, against the radio's expected cost); the last release stops it after a grace period, so a short
 * gap between uses does not restart it. Starting and stopping call the board's ops, which bring the whole radio up or
 * take it fully down. Every call is safe from any task; start and stop block the caller for a second or two.
 */
#ifndef PAL_RADIO_H
#define PAL_RADIO_H

#include "nema/types.h"

typedef enum {
    PAL_RADIO_WIFI = 0,
    PAL_RADIO_BLE,
    PAL_RADIO_COUNT,
} pal_radio_t;

typedef enum {
    PAL_RADIO_OFF = 0,
    PAL_RADIO_STARTING,
    PAL_RADIO_ON,
    PAL_RADIO_STOPPING,
    PAL_RADIO_FAILED, /* the last start failed; the next request tries again */
} pal_radio_state_t;

#define PAL_RADIO_HOLDERS 8
#define PAL_RADIO_HOLDER_LEN 16 /* includes the terminating zero */
#define PAL_RADIO_ERR_LEN 120

typedef struct {
    pal_radio_state_t state;
    uint8_t nholders;
    char holders[PAL_RADIO_HOLDERS][PAL_RADIO_HOLDER_LEN]; /* empty slots hold "" */
    bool grace_pending;                                    /* no holders; stops when the grace period ends */
    char error[PAL_RADIO_ERR_LEN];                         /* last refusal or failure; "" when none */
} pal_radio_info_t;

typedef struct {
    nema_err_t (*start)(pal_radio_t radio); /* full bring-up; blocking; never called with the manager lock held */
    nema_err_t (*stop)(pal_radio_t radio);  /* full teardown; blocking; must also work after a failed start */
    size_t (*detail)(pal_radio_t radio, char *out, size_t cap); /* optional one-line status; may be NULL */
} pal_radio_ops_t;

/* Install the board's radio ops. Call before pal_boot(), which requests the radios the settings ask for. */
void pal_radio_set_ops(const pal_radio_ops_t *ops);

/* Add a holder and start the radio if it is off. Returns NEMA_OK, NEMA_ERR_NOMEM when admission refuses (see
 * pal_radio_last_error), or the start error. A holder that is already present is not added twice. */
nema_err_t pal_radio_request(pal_radio_t radio, const char *holder);

/* Remove a holder. When no holders are left the radio stops after the grace period. NEMA_ERR_NOENT if the holder
 * is not present. */
nema_err_t pal_radio_release(pal_radio_t radio, const char *holder);

/* Drop every holder and stop the radio now. */
void pal_radio_force_off(pal_radio_t radio);

pal_radio_state_t pal_radio_state(pal_radio_t radio);
void pal_radio_info(pal_radio_t radio, pal_radio_info_t *out); /* a consistent copy */
/* The last refusal or failure message. Valid until the radio's next change; "" when none. */
const char *pal_radio_last_error(pal_radio_t radio);
size_t pal_radio_cost_kb(pal_radio_t radio);     /* expected internal RAM while on */
size_t pal_radio_detail(pal_radio_t radio, char *out, size_t cap); /* 0 when the board has no detail */
const char *pal_radio_name(pal_radio_t radio);
const char *pal_radio_state_name(pal_radio_state_t state);

/* Test seams for the host unit tests. The board uses the defaults. */
typedef void (*pal_radio_mem_fn)(size_t *free_bytes, size_t *largest_bytes); /* internal RAM readings */
void pal_radio_set_mem_source(pal_radio_mem_fn fn); /* NULL restores the real readings */
void pal_radio_set_grace_ms(uint32_t ms);           /* default 10000; 0 stops at once */
/* The pause between a start or stop of one radio and the next one of the other (default 1200 ms; 0 for tests). The board's
 * Wi-Fi and Bluetooth bring-up must not overlap: both started in the same instant at boot, an interrupt watchdog reset came
 * once in about twelve boots, nine milliseconds after Wi-Fi's start returned (found on the board, 2026-11-14). Starts and
 * stops of the radios also run one at a time. Between two operations on the same radio, or long after the last, no wait. */
void pal_radio_set_settle_ms(uint32_t ms);

#endif
