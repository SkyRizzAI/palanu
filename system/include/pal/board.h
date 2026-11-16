/* The board descriptor: what a board is, in one table. The system and apps ask for capabilities with pal_has(),
 * never for a board name. A new board is one descriptor (see boards/_template and docs/PORTING-A-BOARD.md) plus its
 * drivers. Every hook is optional: a hook the board does not have stays NULL and the capability bit stays clear. */
#ifndef PAL_BOARD_H
#define PAL_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/device.h"
#include "pal/input.h"

/* Capabilities, one bit each. A board sets the bits for hardware it has and leaves the rest clear. */
typedef uint32_t pal_cap_t;
#define PAL_CAP_DISPLAY (1u << 0)    /* a framebuffer panel: pal_display_init runs */
#define PAL_CAP_KEYBOARD (1u << 1)   /* keys reach pal_input_init through the descriptor's keyboard table */
#define PAL_CAP_BATTERY (1u << 2)    /* the supply voltage can be measured (battery hooks) */
#define PAL_CAP_SD (1u << 3)         /* a removable card slot */
#define PAL_CAP_BUZZER (1u << 4)     /* beep hook */
#define PAL_CAP_RGB_LED (1u << 5)    /* rgb hook */
#define PAL_CAP_MIC (1u << 6)        /* a microphone */
#define PAL_CAP_USB_DEVICE (1u << 7) /* the USB device port (the board is seen as a USB peripheral) */
#define PAL_CAP_WIFI (1u << 8)       /* Wi-Fi radio */
#define PAL_CAP_BLE (1u << 9)        /* Bluetooth LE radio */
#define PAL_CAP_PSRAM (1u << 10)     /* external RAM (PAL_MEM_EXTERNAL) */
#define PAL_CAP_TOUCH (1u << 11)     /* a touch panel on top of the display */
#define PAL_CAP_CAMERA (1u << 12)
#define PAL_CAP_SENSORS (1u << 13)   /* motion, light or environment sensors */

typedef enum { /* panel pixel format */
    PAL_PIXFMT_NONE = 0,
    PAL_PIXFMT_MONO,   /* 1 bit per pixel */
    PAL_PIXFMT_RGB565, /* 16 bits per pixel: the framebuffer palette is RGB565 */
    PAL_PIXFMT_RGB888, /* 24 bits per pixel */
} pal_pixfmt_t;

/* Board hooks. The system calls them only when the matching capability is set (see pal_beep and friends). */
typedef struct {
    void (*beep)(int freq_hz, int ms);            /* PAL_CAP_BUZZER */
    void (*rgb)(uint8_t r, uint8_t g, uint8_t b); /* PAL_CAP_RGB_LED */
    int (*battery_percent)(void);                 /* PAL_CAP_BATTERY; -1 when unknown */
    int (*battery_mv)(void);                      /* PAL_CAP_BATTERY; supply voltage in millivolts, -1 when unknown */
    void (*reboot)(void);
} pal_board_ops_t;

typedef struct {
    const char *name; /* "hhv2", "sim" */
    const char *chip; /* "ESP32-S31", "host" */
    pal_cap_t caps;   /* OR of PAL_CAP_* */
    int display_w, display_h; /* 0 when the board has no display */
    pal_pixfmt_t display_format;
    const pal_keyboard_t *keyboard; /* NULL when the board has no keyboard */
    pal_board_ops_t ops; /* ops.beep and ops.rgb are deprecated: declare devices below; they are registered as one device each when devices is NULL */
    const pal_device_t *devices; /* pal/device.h: LEDs, buzzers and so on; NULL for none */
    int ndevices;
    /* The key cap the legend bar prints for each pal_legend_action_t (pal/legend.h), indexed by the action: PAL_ACT_COUNT
     * entries. NULL, or NULL for an action, hides that legend on this board. boards/hhv2/hhv2_legend.c has the hhv2 table. */
    const char *const *legend_keys;
    /* The UI scale this board's screen looks right at, in quarters (pal/kui.h, KUI_Q): 8 is 2x, the size the screens were
     * drawn for on a 320x240 panel. 0: 2x. The user can change it (ui.scale). */
    int ui_scale_q;
} pal_board_desc_t;

/* Make desc the running board. Copies the descriptor; call once, before anything asks pal_has(). */
void pal_board_set(const pal_board_desc_t *desc);
/* The running board's descriptor. Before pal_board_set it describes a board with no capabilities. */
const pal_board_desc_t *pal_board_info(void);
/* Same as pal_board_info(); kept for existing callers. */
const pal_board_desc_t *pal_board(void);

/* True when every bit of cap is set on the running board. pal_has(0) is false. */
bool pal_has(pal_cap_t cap);

/* The helpers below check their capability first. Without it they do nothing (or return -1). */
void pal_beep(int ms);
int pal_battery_mv(void); /* -1 when the board cannot measure it */
void pal_rgb(uint8_t r, uint8_t g, uint8_t b);
int pal_battery(void);
void pal_reboot(void);

#endif
