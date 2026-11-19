/* The device as a keyboard for another computer or phone (ADR-0018): the platform supplies the transport (Bluetooth LE HID on the
 * board, a recorder in the simulator); the system maps PalanuOS keys to USB HID usages (US layout). The Keyboard app
 * (apps/hidkbd.c) sends every key it gets; holding the cycle key leaves it, as it leaves every app. */
#ifndef PAL_HID_H
#define PAL_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "nema/types.h"
#include "pal/input.h"

typedef enum { PAL_HID_OFF = 0, PAL_HID_WAITING, PAL_HID_CONNECTED } pal_hid_state_t;

/* HID modifier bits of a keyboard report (USB HID usage page 7, 0xE0 to 0xE7). */
#define PAL_HID_MOD_LCTRL 0x01u
#define PAL_HID_MOD_LSHIFT 0x02u
#define PAL_HID_MOD_LALT 0x04u

typedef struct {
    nema_err_t (*start)(const char *name); /* advertise as a keyboard under this name; it takes the radio while on */
    void (*stop)(void);
    pal_hid_state_t (*state)(void);
    /* One keyboard report: the modifiers and up to six keys held (0 = none). An all-zero report releases everything. */
    nema_err_t (*report)(uint8_t mods, const uint8_t keys[6]);
    void (*peer)(char *out, size_t cap); /* who is connected, for the screen; may be NULL */
} pal_hid_ops_t;

void pal_hid_set(const pal_hid_ops_t *ops);
const pal_hid_ops_t *pal_hid(void); /* NULL where there is no transport */

/* A PalanuOS key as a HID usage and modifiers (US layout). False for keys with no HID usage (a code point outside ASCII, the
 * system keys). */
bool pal_hid_from_key(const pal_key_t *k, uint8_t *usage, uint8_t *mods);
/* Press and release k: two reports. NEMA_ERR_INVAL when it has no usage, NEMA_ERR_STATE when nothing is connected. */
nema_err_t pal_hid_type(const pal_key_t *k);

#endif
