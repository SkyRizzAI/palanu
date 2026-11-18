/* The settings schema (pal/settings_schema.h). Owners: NULL is the system's (every session shows it); "classic" and "athena" are
 * the looks of those sessions. The ranges and names repeat what settings_kv.c and settings_api.c accept; test_settings_reach.c
 * checks that every key of those tables has a descriptor here. */
#include <string.h>

#include "pal/board.h"
#include "pal/desktops.h"
#include "pal/settings_schema.h"

static const char *const ON_OFF_AUTO[] = {"off", "on", "auto", NULL};
static const char *const ON_OFF_AUTO_L[] = {"Off", "On", "Automatic", NULL};
static const char *const ON_OFF[] = {"off", "on", NULL};
static const char *const ON_OFF_L[] = {"Off", "On", NULL};
static const char *const PROFILES[] = {"performance", "balanced", "saver", NULL};
static const char *const PROFILES_L[] = {"Performance", "Balanced", "Power saver", NULL};
static const char *const LOGS[] = {"error", "warn", "info", "debug", NULL};
static const char *const LOGS_L[] = {"Errors", "Warnings", "Info", "Debug", NULL};
static const char *const TARGETS[] = {"desktop", "console", NULL};
static const char *const TARGETS_L[] = {"Desktop", "Console", NULL};
static const char *const ROTATIONS[] = {"0", "90", "180", "270", NULL};
static const char *const ATHENA_THEMES[] = {"cupertino", "material", NULL};
static const char *const ATHENA_THEMES_L[] = {"Cupertino", "Material", NULL};

#define SYS NULL
#define CLASSIC "classic"
#define ATHENA "athena"

static const pal_setting_desc_t S[] = {
    /* Display */
    {.key = "display.brightness", .kind = PAL_SETTING_INT, .label = "Brightness", .group = "Display", .owner = SYS, .min = 5, .max = 100, .step = 5},
    {.key = "display.sleep", .kind = PAL_SETTING_INT, .label = "Screen off after (s)", .group = "Display", .help = "0 keeps it on", .owner = SYS, .max = 600, .step = 15},
    {.key = "display.lock", .kind = PAL_SETTING_INT, .label = "Lock after (s)", .group = "Display", .help = "0 never locks by itself", .owner = SYS, .max = 600, .step = 15},
    {.key = "display.rotation", .kind = PAL_SETTING_CHOICE, .label = "Rotation", .group = "Display", .owner = SYS, .names = ROTATIONS, .internal = true},
    {.key = "term.font", .kind = PAL_SETTING_INT, .label = "Terminal font size", .group = "Display", .help = "0 small, 1 medium, 2 large", .owner = SYS, .max = 2, .step = 1},
    /* Controls */
    {.key = "input.repeat_delay", .kind = PAL_SETTING_INT, .label = "Key repeat delay (ms)", .group = "Controls", .owner = SYS, .min = 150, .max = 1000, .step = 10},
    {.key = "input.repeat_rate", .kind = PAL_SETTING_INT, .label = "Key repeat every (ms)", .group = "Controls", .owner = SYS, .min = 20, .max = 200, .step = 5},
    /* Connections */
    {.key = "radio.wifi", .kind = PAL_SETTING_CHOICE, .label = "Wi-Fi", .group = "Connections", .owner = SYS, .names = ON_OFF_AUTO, .labels = ON_OFF_AUTO_L, .caps = PAL_CAP_WIFI},
    {.key = "radio.ble", .kind = PAL_SETTING_CHOICE, .label = "Bluetooth", .group = "Connections", .owner = SYS, .names = ON_OFF, .labels = ON_OFF_L, .caps = PAL_CAP_BLE},
    {.key = "link.uart.trust", .kind = PAL_SETTING_BOOL, .label = "Trust the cable", .group = "Connections", .help = "A client on the USB cable needs no pairing", .owner = SYS},
    /* Notifications */
    {.key = "notify.dnd", .kind = PAL_SETTING_BOOL, .label = "Do not disturb", .group = "Notifications", .owner = SYS},
    {.key = "notify.beep", .kind = PAL_SETTING_BOOL, .label = "Sound", .group = "Notifications", .owner = SYS},
    /* Security and profile */
    {.key = "security.pin", .kind = PAL_SETTING_PIN, .label = "Lock PIN", .group = "Security", .help = "4 to 8 digits; empty clears it", .owner = SYS},
    {.key = "profile.user", .kind = PAL_SETTING_TEXT, .label = "Your name", .group = "Profile", .owner = SYS, .max = 23},
    {.key = "device.name", .kind = PAL_SETTING_TEXT, .label = "Device name", .group = "Profile", .owner = SYS, .max = 31},
    /* Date and time */
    {.key = "time.tz", .kind = PAL_SETTING_INT, .label = "Time zone (minutes from UTC)", .group = "Date & time", .owner = SYS, .min = -720, .max = 840, .step = 15},
    /* Battery */
    {.key = "power.profile", .kind = PAL_SETTING_CHOICE, .label = "Power profile", .group = "Battery", .owner = SYS, .names = PROFILES, .labels = PROFILES_L, .internal = true},
    /* Desktop */
    {.key = "desktop.env", .kind = PAL_SETTING_CHOICE, .label = "Desktop session", .group = "Desktop", .help = "Switches now and at every start", .owner = SYS, .dynamic = true},
    /* Developer */
    {.key = "dev.mode", .kind = PAL_SETTING_BOOL, .label = "Developer mode", .group = "Developer", .owner = SYS},
    {.key = "log.level", .kind = PAL_SETTING_CHOICE, .label = "Log level", .group = "Developer", .owner = SYS, .names = LOGS, .labels = LOGS_L},
    {.key = "boot.target", .kind = PAL_SETTING_CHOICE, .label = "Boot target", .group = "Developer", .help = "The screen in front after a restart", .owner = SYS, .names = TARGETS, .labels = TARGETS_L},
    {.key = "boot.quiet", .kind = PAL_SETTING_BOOL, .label = "Quiet boot", .group = "Developer", .help = "A splash over the boot log", .owner = SYS},
    {.key = "perf.overlay", .kind = PAL_SETTING_BOOL, .label = "Frame timing overlay", .group = "Developer", .owner = SYS, .internal = true},
    /* The classic session's look */
    {.key = "ui.theme", .kind = PAL_SETTING_CHOICE, .label = "Theme", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    {.key = "ui.dark", .kind = PAL_SETTING_BOOL, .label = "Dark mode", .group = "Appearance", .owner = CLASSIC},
    {.key = "ui.size", .kind = PAL_SETTING_INT, .label = "Size", .group = "Appearance", .owner = CLASSIC, .max = 1, .step = 1},
    {.key = "ui.scale", .kind = PAL_SETTING_CHOICE, .label = "UI scale", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    {.key = "ui.font", .kind = PAL_SETTING_CHOICE, .label = "Font", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    {.key = "ui.assets", .kind = PAL_SETTING_CHOICE, .label = "Asset pack", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    {.key = "ui.statusbar", .kind = PAL_SETTING_BOOL, .label = "Status bar", .group = "Appearance", .owner = CLASSIC},
    {.key = "ui.legend", .kind = PAL_SETTING_BOOL, .label = "Key legend", .group = "Appearance", .owner = CLASSIC},
    {.key = "ui.clock24", .kind = PAL_SETTING_BOOL, .label = "24-hour clock", .group = "Appearance", .owner = CLASSIC, .internal = true},
    {.key = "ui.launcher", .kind = PAL_SETTING_CHOICE, .label = "Launcher", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    {.key = "ui.wallpaper", .kind = PAL_SETTING_CHOICE, .label = "Wallpaper", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    {.key = "ui.lock_anim", .kind = PAL_SETTING_CHOICE, .label = "Lock animation", .group = "Appearance", .owner = CLASSIC, .dynamic = true, .internal = true},
    {.key = "ui.wall_fit", .kind = PAL_SETTING_CHOICE, .label = "Wallpaper fit", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    {.key = "ui.wall_anchor", .kind = PAL_SETTING_CHOICE, .label = "Wallpaper anchor", .group = "Appearance", .owner = CLASSIC, .dynamic = true},
    /* The Athena session's look */
    {.key = "athena.seconds", .kind = PAL_SETTING_BOOL, .label = "Second hand", .group = "Athena", .owner = ATHENA, .internal = true},
    {.key = "athena.theme", .kind = PAL_SETTING_CHOICE, .label = "Theme", .group = "Athena", .help = "Cupertino or Material", .owner = ATHENA, .names = ATHENA_THEMES, .labels = ATHENA_THEMES_L},
};

size_t pal_settings_schema_count(void) { return sizeof(S) / sizeof(S[0]); }
const pal_setting_desc_t *pal_settings_schema_get(size_t i) { return i < pal_settings_schema_count() ? &S[i] : NULL; }

const pal_setting_desc_t *pal_settings_schema_find(const char *key) {
    for (size_t i = 0; key && i < pal_settings_schema_count(); i++)
        if (!strcmp(S[i].key, key)) return &S[i];
    return NULL;
}

bool pal_setting_visible(const char *key) {
    const pal_setting_desc_t *d = pal_settings_schema_find(key);
    return !d || !d->owner || !strcmp(d->owner, pal_desktops_active());
}
