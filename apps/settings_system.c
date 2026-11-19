/* Settings pages: Date and time, Profile (Identity: user name, device name, the Password), Updates, Developer (the two standard
 * rows, then the System rows), and About (the reference firmware and capability rows, with the details under Developer > System).
 * The clock and the PIN go through the settings table or the time and dpm functions they own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "pal/version.h"
#include "pal/board.h"
#include "pal/bootcount.h"
#include "pal/desktop.h"
#include "pal/ota.h"
#include "pal/settings.h"
#include "pal/svc.h"
#include "pal/sysinfo.h"
#include "pal/time.h"
#include "settings_int.h"



/* ---- Date and time ---- */

static const int32_t TZ_VALS[] = {-480, -300, 0, 60, 330, 420, 480, 540};
static const char *const TZ_LABELS[] = {"UTC-8", "UTC-5", "UTC", "UTC+1", "UTC+5:30", "UTC+7", "UTC+8", "UTC+9", NULL};

/* The current offset is shown as the nearest choice (a hand-edited value may fall between two of them). */
static int tz_get(void *ctx) {
    (void)ctx;
    int cur = pal_time_tz_minutes(), best = 0;
    for (int i = 1; i < 8; i++)
        if (abs(TZ_VALS[i] - cur) < abs(TZ_VALS[best] - cur)) best = i;
    return best;
}

static void tz_set(void *ctx, int index) {
    char t[16];
    snprintf(t, sizeof(t), "%d", (int)TZ_VALS[index]);
    (void)psys_settings_set("time.tz", t);
    (void)ctx;
}

static void local_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_tm_t tm;
    pal_time_local(&tm);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", tm.year, tm.month, tm.day, tm.hour, tm.minute);
}

static void ntp_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    if (!psys_svc_find("time", &in)) {
        snprintf(out, cap, "not in this build");
        return;
    }
    snprintf(out, cap, "%s", psys_svc_state_name(in.state));
}

static void date_now(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_tm_t tm;
    pal_time_local(&tm);
    snprintf(out, cap, "%04d-%02d-%02d", tm.year, tm.month, tm.day);
}

static void time_now(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_tm_t tm;
    pal_time_local(&tm);
    snprintf(out, cap, "%02d:%02d", tm.hour, tm.minute);
}

/* Sets the clock to the local date and time given, keeping the rest. Local to UTC uses the time zone setting. */
static void set_local(const pal_tm_t *local) {
    pal_tm_t t = *local;
    uint32_t epoch = pal_time_to_epoch(&t) - (uint32_t)(pal_time_tz_minutes() * 60);
    pal_time_set(epoch);
    pal_desktop_invalidate_all(); /* the clock and the status bar read the new time */
}

static bool parse_digits(const char *s, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

static bool apply_date(void *ctx, const char *text) {
    (void)ctx;
    if (strlen(text) != 10 || text[4] != '-' || text[7] != '-') return false;
    int y, m, d;
    if (!parse_digits(text, 4, &y) || !parse_digits(text + 5, 2, &m) || !parse_digits(text + 8, 2, &d)) return false;
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 2000) return false;
    pal_tm_t tm;
    pal_time_local(&tm);
    tm.year = y, tm.month = m, tm.day = d;
    set_local(&tm);
    return true;
}

static bool apply_time(void *ctx, const char *text) {
    (void)ctx;
    if (strlen(text) != 5 || text[2] != ':') return false;
    int h, mi;
    if (!parse_digits(text, 2, &h) || !parse_digits(text + 3, 2, &mi)) return false;
    if (h > 23 || mi > 59) return false;
    pal_tm_t tm;
    pal_time_local(&tm);
    tm.hour = h, tm.minute = mi, tm.second = 0;
    set_local(&tm);
    return true;
}

static pal_menu_row_t DATETIME_STORE[12] NEMA_EXT_BSS;

static void build_datetime(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "Local time", .icon = "clock", .text = local_text,
                        .hint = "No battery clock: set it after a restart"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Time zone", .icon = "clock", .ctx = NULL,
                         .choices = TZ_LABELS, .get = tz_get, .set = tz_set, .hint = "Nearest common offset", .key = "time.tz"};
    pal_menu_add(p, &r);
    /* hidden until the clock screens read it: a 12-hour clock needs an AM/PM suffix in the home and lock layouts, and a
     * switch that changes nothing is worse than no switch */
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "24-hour clock", .ctx = (void *)"ui.clock24",
                         .get = settings_bool_get, .set = settings_bool_set, .hidden = true,
                         .hint = "Saved; the clock screens read it when wired"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Network time", .icon = "wifi", .text = ntp_text,
                         .hint = "SNTP runs while Wi-Fi has an address"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TEXT, .label = "Set date", .text = date_now, .apply_text = apply_date,
                         .max_len = 10, .title = "Date (YYYY-MM-DD)", .hint = "Keeps the current time"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TEXT, .label = "Set time", .text = time_now, .apply_text = apply_time,
                         .max_len = 5, .title = "Time (HH:MM)", .hint = "Local time, 24 hours"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_DATETIME = {.title = "Date & time", .store = DATETIME_STORE, .cap = 12, .build = build_datetime,
                               .live = true};

/* ---- Profile: Identity, with the user name, the device name and the Password (the lock
 * screen PIN page). The developer mode is a System row of Developer. ---- */

static void dev_done(void *ctx, bool ok) {
    (void)ctx;
    if (ok) (void)psys_settings_set("dev.mode", "1");
    pal_menu_refresh(settings_menu());
}

static int dev_get(void *ctx) {
    (void)ctx;
    return settings_key_int("dev.mode", 0) != 0;
}

/* Turning it off is at once; turning it on asks first, so the switch stays off until the answer is Turn on. */
static void dev_set(void *ctx, int v) {
    (void)ctx;
    if (!v) {
        (void)psys_settings_set("dev.mode", "0");
        pal_menu_refresh(settings_menu());
        return;
    }
    settings_confirm("Developer mode",
                     "Unsigned apps and the shell's network tools can run. Only turn this on for your own work.",
                     "Turn on", dev_done, NULL);
}

static bool pin_is_set(void) {
    char t[16];
    settings_key_text("security.pin", t, sizeof(t));
    return strcmp(t, "set") == 0;
}

static void pin_status(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", pin_is_set() ? "set" : "not set");
}

/* The Password row: the reference dots when a PIN is set, "(not set)" otherwise. */
static void password_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", pin_is_set() ? "* * * *" : "(not set)");
}

static pal_menu_row_t PROFILE_STORE[6] NEMA_EXT_BSS;

static void build_profile(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Identity"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TEXT, .label = "User Name", .icon = "info", .ctx = (void *)"profile.user",
                         .text = settings_text_get, .apply_text = settings_text_apply, .max_len = 23};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TEXT, .label = "Device Name", .icon = "info", .ctx = (void *)"device.name",
                         .text = settings_text_get, .apply_text = settings_text_apply, .max_len = 31};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Password", .icon = "lock", .text = password_text, .open = settings_open_ctx,
                         .ctx = &PG_PIN, .key = "security.pin"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_PROFILE = {.title = "Profile", .store = PROFILE_STORE, .cap = 6, .build = build_profile};

/* ---- the PIN: set or change (typed twice on the virtual keyboard, masked), or clear ---- */

static char s_pin1[9];

static void pin_second(void *ctx, bool ok, const char *text) {
    (void)ctx;
    if (!ok) return;
    if (strcmp(s_pin1, text ? text : "") != 0) {
        memset(s_pin1, 0, sizeof(s_pin1));
        settings_toast("PINs do not match");
        return;
    }
    if (settings_text_apply((void *)"security.pin", s_pin1)) settings_toast("PIN set");
    memset(s_pin1, 0, sizeof(s_pin1));
    pal_menu_refresh(settings_menu()); /* "Change PIN", and Clear PIN can be used now */
}

static void pin_first(void *ctx, bool ok, const char *text) {
    (void)ctx;
    if (!ok) return;
    size_t n = text ? strlen(text) : 0;
    if (n < 4 || n > 8) {
        settings_toast("A PIN is 4 to 8 digits");
        return;
    }
    snprintf(s_pin1, sizeof(s_pin1), "%s", text);
    pal_menu_ask_text(settings_menu(), "Confirm the PIN", "", 8, true, pin_second, NULL);
}

static void pin_set_run(void *ctx) {
    (void)ctx;
    pal_menu_ask_text(settings_menu(), "New PIN (4 to 8 digits)", "", 8, true, pin_first, NULL);
}

static void pin_clear_run(void *ctx) {
    (void)ctx;
    if (settings_text_apply((void *)"security.pin", "")) settings_toast("PIN cleared");
    pal_menu_refresh(settings_menu());
}

static pal_menu_row_t PIN_STORE[6] NEMA_EXT_BSS;

static void build_pin(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "Status", .text = pin_status};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = pin_is_set() ? "Change PIN" : "Set PIN", .icon = "lock",
                         .run = pin_set_run, .hint = "Typed twice, masked"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Clear PIN", .icon = "lock", .danger = true,
                         .run = pin_clear_run, .disabled = !pin_is_set(), .confirm = "The lock screen opens with two Enters.",
                         .ok_label = "Clear", .hint = "Only when a PIN is set"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_PIN = {.title = "Password", .store = PIN_STORE, .cap = 6, .build = build_pin};

/* ---- Updates ---- */

static void os_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "PalanuOS %s", PAL_OS_VERSION);
}

static void build_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const pal_ota_ops_t *o = pal_ota();
    snprintf(out, cap, "%s", o && o->version ? o->version() : "n/a");
}

static void slot_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const pal_ota_ops_t *o = pal_ota();
    snprintf(out, cap, "%s", o && o->running ? o->running() : "-");
}

static void ota_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", pal_ota() ? "ready" : "not on this build");
}

static pal_menu_row_t UPDATES_STORE[8] NEMA_EXT_BSS;

static void build_updates(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "Firmware", .icon = "power", .text = os_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Build", .icon = "info", .text = build_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Running slot", .text = slot_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Update over Link", .text = ota_text,
                         .hint = "Install from Forge over Palanu Link"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_UPDATES = {.title = "Updates", .store = UPDATES_STORE, .cap = 8, .build = build_updates};

/* ---- Developer (the standard rows, then the PalanuOS system rows under System) ---- */

static void na_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", "N/A");
}

static const char *const TARGET_TEXTS[] = {"desktop", "console", NULL};
static const char *const TARGET_LABELS[] = {"Desktop", "Console", NULL};
static const settings_map_t MAP_TARGET = {"boot.target", TARGET_TEXTS, NULL, 2};

static const char *const LOG_TEXTS[] = {"error", "warn", "info", "debug", NULL};
static const char *const LOG_LABELS[] = {"Errors", "Warnings", "Info", "Debug", NULL};
static const settings_map_t MAP_LOG = {"log.level", LOG_TEXTS, NULL, 4};

static void safe_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", psys_svc_safe_mode() ? "on" : "off");
}

static void safe_clear_run(void *ctx) {
    (void)ctx;
    settings_toast(psys_bootcount_clear() == NEMA_OK ? "Safe mode clears at the next restart" : "Cannot clear here");
}

static void heap_run(void *ctx) {
    (void)ctx;
    settings_toast(pal_sys_heap_check() ? "Heap OK" : "Heap damaged: see the console");
}

static void restart_run(void *ctx) {
    (void)ctx;
    pal_reboot();
}

/* The reference rows first. Stop Display Server and Reboot to Bootloader have no PalanuOS action (there is no display server, and the
 * board has no bootloader entry; Restart is the reboot), so they are N/A and do nothing. */
static const pal_menu_row_t DEV_ROWS[] = {
    {.kind = PAL_MENU_SUBMENU, .label = "Stop Display Server", .disabled = true, .text = na_text},
    {.kind = PAL_MENU_SUBMENU, .label = "Reboot to Bootloader", .disabled = true, .text = na_text},
    {.kind = PAL_MENU_SECTION, .label = "System"},
    {.kind = PAL_MENU_CHOICE, .label = "Boot target", .icon = "terminal", .ctx = (void *)&MAP_TARGET,
     .choices = TARGET_LABELS, .get = settings_map_get, .set = settings_map_set,
     .hint = "The screen in front after a restart"},
    {.kind = PAL_MENU_TOGGLE, .label = "Quiet boot", .icon = "terminal", .ctx = (void *)"boot.quiet", .get = settings_bool_get,
     .set = settings_bool_set, .hint = "A splash over the boot log; any key shows the log"},
    {.kind = PAL_MENU_CHOICE, .label = "Log level", .icon = "logs", .ctx = (void *)&MAP_LOG, .choices = LOG_LABELS,
     .get = settings_map_get, .set = settings_map_set, .hint = "What the device logs to the console"},
    {.kind = PAL_MENU_TOGGLE, .label = "Save log to flash", .icon = "logs", .ctx = (void *)"log.file", .get = settings_bool_get,
     .set = settings_bool_set, .key = "log.file", .hint = "/data/log/system.log, kept across restarts (from the next start)"},
    {.kind = PAL_MENU_TOGGLE, .label = "Developer mode", .icon = "monitor", .get = dev_get, .set = dev_set, .key = "dev.mode",
     .hint = "Turning it on asks first"},
    {.kind = PAL_MENU_INFO, .label = "Safe mode", .text = safe_text},
    {.kind = PAL_MENU_ACTION, .label = "Clear safe mode", .icon = "monitor", .run = safe_clear_run,
     .hint = "Applies at the next restart"},
    {.kind = PAL_MENU_SUBMENU, .label = "Services", .icon = "services", .open = settings_open_ctx, .ctx = &PG_SERVICES,
     .hint = "State, restart and start at boot"},
    {.kind = PAL_MENU_ACTION, .label = "Heap check", .icon = "monitor", .run = heap_run,
     .hint = "Checks the block headers of every heap"},
    {.kind = PAL_MENU_SUBMENU, .label = "Notifications", .icon = "notifications", .open = settings_open_ctx, .ctx = &PG_NOTIFY},
    {.kind = PAL_MENU_SUBMENU, .label = "Date & time", .icon = "clock", .open = settings_open_ctx, .ctx = &PG_DATETIME},
    {.kind = PAL_MENU_SUBMENU, .label = "Updates", .icon = "power", .open = settings_open_ctx, .ctx = &PG_UPDATES},
    {.kind = PAL_MENU_SUBMENU, .label = "Bluetooth", .icon = "bluetooth", .open = settings_open_ctx, .ctx = &PG_BLUETOOTH},
    {.kind = PAL_MENU_SUBMENU, .label = "Battery & power", .icon = "battery", .open = settings_open_ctx, .ctx = &PG_POWER},
    {.kind = PAL_MENU_SUBMENU, .label = "Keys and repeat", .icon = "terminal", .open = settings_open_ctx, .ctx = &PG_KEYS},
    {.kind = PAL_MENU_SUBMENU, .label = "Palanu Link", .icon = "link", .open = settings_open_ctx, .ctx = &PG_REMOTE_LINK},
    {.kind = PAL_MENU_SUBMENU, .label = "Storage details", .icon = "files", .open = settings_open_ctx, .ctx = &PG_STORAGE_DETAIL},
    {.kind = PAL_MENU_SUBMENU, .label = "About details", .icon = "info", .open = settings_open_ctx, .ctx = &PG_ABOUT_DETAIL},
    {.kind = PAL_MENU_ACTION, .label = "Restart", .icon = "power", .danger = true, .run = restart_run,
     .confirm = "The device restarts now.", .ok_label = "Restart"},
};

pal_menu_page_t PG_DEVELOPER = {.title = "Developer", .rows = DEV_ROWS, .n = sizeof(DEV_ROWS) / sizeof(DEV_ROWS[0])};

/* ---- About: Firmware (board, platform, version, uptime) and the capabilities the board has ---- */

static void board_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const pal_board_desc_t *b = pal_board_info();
    snprintf(out, cap, "%s", b->name && b->name[0] ? b->name : "-");
}

static void platform_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const pal_board_desc_t *b = pal_board_info();
    snprintf(out, cap, "%s", b->chip && b->chip[0] ? b->chip : "-");
}

static void version_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", PAL_OS_VERSION);
}

/* The reference uptime: "2m 2s", with the hours first when there are some. */
static void uptime_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    uint64_t s = pal_sys_uptime_us() / 1000000u;
    unsigned h = (unsigned)(s / 3600u), m = (unsigned)(s / 60u % 60u), sec = (unsigned)(s % 60u);
    if (h) snprintf(out, cap, "%uh %um %us", h, m, sec);
    else snprintf(out, cap, "%um %us", m, sec);
}

/* The capabilities the board sets (pal_has), one row each, in this order. */
static const struct {
    pal_cap_t bit;
    const char *name;
} CAP_ROWS[] = {{PAL_CAP_DISPLAY, "Display"},     {PAL_CAP_KEYBOARD, "Keyboard"}, {PAL_CAP_BATTERY, "Battery"},
                {PAL_CAP_SD, "SD card"},          {PAL_CAP_BUZZER, "Buzzer"},     {PAL_CAP_RGB_LED, "RGB LED"},
                {PAL_CAP_MIC, "Microphone"},      {PAL_CAP_USB_DEVICE, "USB device"}, {PAL_CAP_WIFI, "Wi-Fi"},
                {PAL_CAP_BLE, "Bluetooth LE"},    {PAL_CAP_PSRAM, "PSRAM"},       {PAL_CAP_TOUCH, "Touch"},
                {PAL_CAP_CAMERA, "Camera"},       {PAL_CAP_SENSORS, "Sensors"}};

static pal_menu_row_t ABOUT_STORE[48] NEMA_EXT_BSS;

static void build_about(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Firmware"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Board", .text = board_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Platform", .text = platform_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Version", .text = version_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Uptime", .icon = "clock", .text = uptime_text};
    pal_menu_add(p, &r);
    bool any = false;
    for (size_t i = 0; i < sizeof(CAP_ROWS) / sizeof(CAP_ROWS[0]); i++) {
        if (!pal_has(CAP_ROWS[i].bit)) continue;
        if (!any) {
            r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Capabilities"};
            pal_menu_add(p, &r);
            any = true;
        }
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = CAP_ROWS[i].name};
        pal_menu_add(p, &r);
    }
    /* the devices the board declares (pal/device.h), one row each: the name and the id */
    const pal_board_desc_t *b = pal_board_info();
    int shown = 0;
    for (int i = 0; i < b->ndevices && shown < 16; i++) {
        if (b->devices[i].kind == PAL_DEV_LED || b->devices[i].kind == PAL_DEV_BUZZER) continue; /* the LED and Device pages list these */
        if (!shown++) {
            r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Devices"};
            pal_menu_add(p, &r);
        }
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = b->devices[i].name, .text = settings_text_static,
                             .ctx = (void *)b->devices[i].id};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_ABOUT = {.title = "About", .store = ABOUT_STORE, .cap = 48, .build = build_about, .live = true};

/* ---- About details (PalanuOS extras, under Developer > System): the build, the memory, the license and the notices ---- */

static void os_build_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const pal_ota_ops_t *o = pal_ota();
    snprintf(out, cap, "PalanuOS %s", PAL_OS_VERSION);
    if (o && o->version) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, ", build %s", o->version());
    }
}

static void ram_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%lu of %lu KB free", (unsigned long)(pal_sys_mem_free(PAL_MEM_INTERNAL) / 1024u),
             (unsigned long)(pal_sys_mem_total(PAL_MEM_INTERNAL) / 1024u));
}

static void psram_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    size_t total = pal_sys_mem_total(PAL_MEM_EXTERNAL);
    if (!total) {
        snprintf(out, cap, "not fitted");
        return;
    }
    snprintf(out, cap, "%lu of %lu KB free", (unsigned long)(pal_sys_mem_free(PAL_MEM_EXTERNAL) / 1024u),
             (unsigned long)(total / 1024u));
}

static pal_menu_row_t ABOUT_DETAIL_STORE[10] NEMA_EXT_BSS;

static void build_about_detail(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "System"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Build", .icon = "info", .text = os_build_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Internal RAM", .icon = "monitor", .text = ram_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "PSRAM", .text = psram_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Flash size", .text = settings_text_static,
                         .ctx = (void *)"not reported"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "License", .icon = "about", .text = settings_text_static,
                         .ctx = (void *)"MIT"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Third-party notices", .icon = "info", .open = settings_open_ctx,
                         .ctx = &PG_ABOUT_NOTICES, .hint = "Libraries in this build (see NOTICE)"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_ABOUT_DETAIL = {.title = "About details", .store = ABOUT_DETAIL_STORE, .cap = 10,
                                   .build = build_about_detail, .live = true};

static const pal_menu_row_t NOTICE_ROWS[] = {
    {.kind = PAL_MENU_INFO, .label = "WAMR", .text = settings_text_static, .ctx = (void *)"WebAssembly runtime (apps)"},
    {.kind = PAL_MENU_INFO, .label = "Monocypher", .text = settings_text_static, .ctx = (void *)"Keys, signatures, AEAD"},
    {.kind = PAL_MENU_INFO, .label = "miniz (tinfl)", .text = settings_text_static, .ctx = (void *)"Inflate for packages"},
    {.kind = PAL_MENU_INFO, .label = "Spleen", .text = settings_text_static, .ctx = (void *)"Bitmap fonts"},
    {.kind = PAL_MENU_INFO, .label = "GNU Unifont", .text = settings_text_static, .ctx = (void *)"Fallback glyphs (OFL 1.1)"},
    {.kind = PAL_MENU_INFO, .label = "Nerd Fonts", .text = settings_text_static, .ctx = (void *)"Terminal icons (MIT, FA CC BY 4.0)"},
    {.kind = PAL_MENU_INFO, .label = "LittleFS", .text = settings_text_static, .ctx = (void *)"Flash filesystem"},
    {.kind = PAL_MENU_SECTION, .label = "Licenses"},
    {.kind = PAL_MENU_INFO, .label = "Full texts", .wrap = true, .text = settings_text_static,
     .ctx = (void *)"See the NOTICE file in the source tree for each library's license."},
};

pal_menu_page_t PG_ABOUT_NOTICES = {.title = "Third-party notices", .rows = NOTICE_ROWS,
                                    .n = sizeof(NOTICE_ROWS) / sizeof(NOTICE_ROWS[0])};
