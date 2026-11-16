/* Settings pages: Notifications (do not disturb, the recent sources, clear all), Sounds and Battery (the reference rows), the battery
 * and power extras, Storage (the reference flash and card rows) and the storage details (the flash in bytes, the card slot as the
 * board reports it). */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "nema/vfs.h"
#include "pal/sd.h"
#include "pal/board.h"
#include "pal/notify.h"
#include "pal/theme.h"
#include "settings_int.h"

/* ---- Notifications ---- */

static pal_menu_row_t NOTIFY_STORE[8] NEMA_EXT_BSS;
static pal_menu_row_t APPS_STORE[12] NEMA_EXT_BSS;
static psys_notify_t s_recent[8] NEMA_EXT_BSS;

static void unread_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%d", psys_notify_unread_count());
}

static void count_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%d", psys_notify_count());
}

static void clear_run(void *ctx) {
    (void)ctx;
    psys_notify_clear();
    settings_toast("Notifications cleared");
    pal_menu_refresh(settings_menu());
}

static void build_notify(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Do not disturb", .icon = "notifications",
                                        .ctx = (void *)"notify.dnd", .get = settings_bool_get, .set = settings_bool_set,
                                        .hint = "No toasts and no beep; the list still fills"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Beep on notify", .icon = "default",
                         .ctx = (void *)"notify.beep", .get = settings_bool_get, .set = settings_bool_set,
                         .hint = "Uses the buzzer when the board has one"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Unread", .text = unread_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "In the list", .text = count_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Recent sources", .icon = "apps", .open = settings_open_ctx,
                         .ctx = &PG_NOTIFY_APPS, .hint = "The newest notifications and who posted them"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Clear all", .icon = "lock", .danger = true, .run = clear_run,
                         .confirm = "Remove every notification from the list.", .ok_label = "Clear"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_NOTIFY = {.title = "Notifications", .store = NOTIFY_STORE, .cap = 8, .build = build_notify, .live = true};

static void recent_title(void *ctx, char *out, size_t cap) {
    const psys_notify_t *n = (const psys_notify_t *)ctx;
    snprintf(out, cap, "%s", n->source);
}

static void build_apps(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Per-app mute", .text = settings_text_static,
                                        .ctx = (void *)"Not supported yet", .hint = "Mute needs a per-app setting in notify.c"};
    pal_menu_add(p, &r);
    int n = psys_notify_list(s_recent, 8);
    for (int i = 0; i < n; i++) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = s_recent[i].title[0] ? s_recent[i].title : "(no title)",
                             .text = recent_title, .ctx = &s_recent[i]};
        pal_menu_add(p, &r);
    }
    if (n == 0) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "No notifications", .text = settings_text_static,
                             .ctx = (void *)"Nothing has been posted"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_NOTIFY_APPS = {.title = "Recent sources", .store = APPS_STORE, .cap = 12, .build = build_apps};

/* ---- Sounds: Input and Output. The board's buzzer is the output and it has no level
 * meter (the reference meters need a peak level the board does not report), so an output row shows the device alone. */

static void beep_run(void *ctx) {
    (void)ctx;
    pal_beep(120);
}

static pal_menu_row_t SOUND_STORE[8] NEMA_EXT_BSS;

static void build_sound(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Input"};
    pal_menu_add(p, &r);
    if (pal_has(PAL_CAP_MIC)) r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Microphone"};
    else r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No input devices"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Output"};
    pal_menu_add(p, &r);
    if (pal_has(PAL_CAP_BUZZER)) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Buzzer"};
        pal_menu_add(p, &r);
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Test Beep", .icon = "default", .run = beep_run};
        pal_menu_add(p, &r);
    } else {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No output devices"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_SOUND = {.title = "Sounds", .store = SOUND_STORE, .cap = 16, .build = build_sound};

/* ---- Battery: Level and Charging. The board reports the level only; it has no charge
 * state, so Charging says N/A rather than guessing. ---- */

static void level_text(void *ctx, char *out, size_t cap) {
    int pct = ctx ? pal_dev_battery_percent(ctx) : pal_battery();
    if (pct < 0) snprintf(out, cap, "unknown");
    else snprintf(out, cap, "%d%%", pct);
}

static void charging_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "N/A");
}

static pal_menu_row_t BATTERY_STORE[6] NEMA_EXT_BSS;

static void build_battery(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Battery"};
    pal_menu_add(p, &r);
    if (pal_has(PAL_CAP_BATTERY)) {
        int nb = pal_dev_count(PAL_DEV_BATTERY);
        for (int i = 0; i < nb && i < 2; i++) { /* one Level row per battery the board declares; the name tells them apart when there are several */
            const pal_device_t *d = pal_dev_get(PAL_DEV_BATTERY, i);
            r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = nb > 1 ? d->name : "Level", .icon = "battery",
                                 .text = level_text, .ctx = (void *)d};
            pal_menu_add(p, &r);
        }
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Charging", .text = charging_text};
        pal_menu_add(p, &r);
    } else {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No battery driver"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_BATTERY = {.title = "Battery", .store = BATTERY_STORE, .cap = 6, .build = build_battery, .live = true};

/* ---- Battery and power (PalanuOS extras, under Developer > System): the voltage and the stored power profile ---- */

static const char *const PROFILE_TEXTS[] = {"performance", "balanced", "saver", NULL};
static const char *const PROFILE_LABELS[] = {"Performance", "Balanced", "Saver", NULL};
static const settings_map_t MAP_PROFILE = {"power.profile", PROFILE_TEXTS, NULL, 3};

static void volt_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    int mv = pal_battery_mv();
    if (mv < 0) snprintf(out, cap, "unknown");
    else snprintf(out, cap, "%d.%02d V", mv / 1000, (mv % 1000) / 10);
}

static pal_menu_row_t POWER_STORE[8] NEMA_EXT_BSS;

static void build_power(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Battery"};
    pal_menu_add(p, &r);
    if (pal_has(PAL_CAP_BATTERY)) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Voltage", .text = volt_text};
        pal_menu_add(p, &r);
    } else {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "No battery driver"};
        pal_menu_add(p, &r);
    }
    /* hidden until a power service applies it: the profile is stored and nothing reads it (docs/RELEASE.md) */
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Profile", .hidden = true};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Power profile", .icon = "power", .ctx = (void *)&MAP_PROFILE,
                         .choices = PROFILE_LABELS, .get = settings_map_get, .set = settings_map_set, .hidden = true,
                         .hint = "Stored now; the power service applies it later"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_POWER = {.title = "Battery & power", .store = POWER_STORE, .cap = 8, .build = build_power, .live = true};

/* ---- Storage: the internal flash and the SD card. ---- */

/* The flash of the data filesystem: total and free bytes, when the medium reports them (nema_fs_statfs). */
static bool flash_space(uint64_t *total, uint64_t *used) {
    uint64_t t = 0, f = 0;
    if (nema_fs_statfs("/data", &t, &f) != NEMA_OK || t == 0) return false;
    *total = t;
    *used = t >= f ? t - f : 0;
    return true;
}

/* The reference byte format (fmtBytes64): whole GB, MB, KB or B. */
static void ref_bytes(uint64_t b, char *out, size_t cap) {
    const uint64_t KB = 1024u, MB = KB * 1024u, GB = MB * 1024u;
    if (b >= GB) snprintf(out, cap, "%llu GB", (unsigned long long)(b / GB));
    else if (b >= MB) snprintf(out, cap, "%llu MB", (unsigned long long)(b / MB));
    else if (b >= KB) snprintf(out, cap, "%llu KB", (unsigned long long)(b / KB));
    else snprintf(out, cap, "%llu B", (unsigned long long)b);
}

static void flash_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    uint64_t t, u;
    if (!flash_space(&t, &u)) {
        snprintf(out, cap, "not reported");
        return;
    }
    char a[24], b[24];
    ref_bytes(u, a, sizeof(a));
    ref_bytes(t, b, sizeof(b));
    snprintf(out, cap, "%s / %s", a, b);
}

static int flash_pct(void *ctx) {
    (void)ctx;
    uint64_t t, u;
    if (!flash_space(&t, &u)) return 0;
    return (int)(u * 100u / t);
}

/* The card (pal/sd.h): used and total when it is mounted, the reference "Not mounted" otherwise. */
static bool card_space(uint64_t *total, uint64_t *used) {
    uint64_t t = 0, f = 0;
    if (!pal_sd_space(&t, &f) || t == 0) return false;
    *total = t;
    *used = t >= f ? t - f : 0;
    return true;
}

static void card_short(void *ctx, char *out, size_t cap) {
    (void)ctx;
    uint64_t t, u;
    if (!pal_sd_present()) snprintf(out, cap, "No slot");
    else if (!pal_sd_mounted()) snprintf(out, cap, "Not mounted");
    else if (!card_space(&t, &u)) snprintf(out, cap, "Mounted");
    else {
        char a[24], b[24];
        ref_bytes(u, a, sizeof(a));
        ref_bytes(t, b, sizeof(b));
        snprintf(out, cap, "%s / %s", a, b);
    }
}

static int card_pct(void *ctx) {
    (void)ctx;
    uint64_t t, u;
    return card_space(&t, &u) ? (int)(u * 100u / t) : 0;
}

static void eject_done(void *ctx, bool ok) {
    (void)ctx;
    if (!ok) return;
    settings_run_job(JOB_SD_EJECT, NULL, NULL, "Ejecting card");
}

/* Mount when there is none (the slot has no card-detect line, so a card put in later is found here or with `sd mount`);
 * Eject asks first, as the reference does. */
static void card_run(void *ctx) {
    (void)ctx;
    if (settings_job_busy()) return;
    if (pal_sd_mounted())
        settings_confirm("Eject SD card", "Files still open on the card will fail. Remove the card after this.", "Eject",
                         eject_done, NULL);
    else settings_run_job(JOB_SD_MOUNT, NULL, NULL, "Looking for a card");
}

static pal_menu_row_t STORAGE_STORE[8] NEMA_EXT_BSS;

static void build_storage(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Storage"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Internal Flash", .text = flash_text};
    pal_menu_add(p, &r);
    /* a gauge: a slider without a setter draws the read-only bar (the reference progress row under the flash) */
    r = (pal_menu_row_t){.kind = PAL_MENU_SLIDER, .get = flash_pct, .min = 0, .max = 100, .step = 1};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "SD Card", .text = card_short};
    pal_menu_add(p, &r);
    if (!pal_sd_present()) return;
    if (pal_sd_mounted()) {
        r = (pal_menu_row_t){.kind = PAL_MENU_SLIDER, .get = card_pct, .min = 0, .max = 100, .step = 1};
        pal_menu_add(p, &r);
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = pal_sd_mounted() ? "Eject SD Card" : "Mount SD Card",
                         .icon = "files", .run = card_run};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_STORAGE = {.title = "Storage", .store = STORAGE_STORE, .cap = 8, .build = build_storage, .live = true};

/* ---- Storage details (PalanuOS extras, under Developer > System): the flash in bytes, and the card slot ---- */

/* Short sizes with one decimal: "512 KB", "3.4 MB", "318.2 GB". */
static void size_text(uint64_t b, char *out, size_t cap) {
    const uint64_t KB = 1024u, MB = KB * 1024u, GB = MB * 1024u;
    if (b >= GB) snprintf(out, cap, "%u.%u GB", (unsigned)(b / GB), (unsigned)((b % GB) / (GB / 10u)));
    else if (b >= MB) snprintf(out, cap, "%u.%u MB", (unsigned)(b / MB), (unsigned)((b % MB) / (MB / 10u)));
    else snprintf(out, cap, "%u KB", (unsigned)(b / KB));
}

static void used_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    uint64_t t, u;
    if (!flash_space(&t, &u)) snprintf(out, cap, "not reported");
    else size_text(u, out, cap);
}

static void free_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    uint64_t t, u;
    if (!flash_space(&t, &u)) snprintf(out, cap, "-");
    else size_text(t - u, out, cap);
}

static void total_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    uint64_t t, u;
    if (!flash_space(&t, &u)) snprintf(out, cap, "-");
    else size_text(t, out, cap);
}

static void card_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_sd_info_t in;
    if (!pal_sd_present()) snprintf(out, cap, "no card slot");
    else if (!pal_sd_info(&in)) snprintf(out, cap, "no card mounted");
    else {
        char sz[24];
        size_text(in.size_bytes, sz, sizeof(sz));
        snprintf(out, cap, "%s, %s, %lu MHz", in.name[0] ? in.name : "card", sz, (unsigned long)(in.speed_khz / 1000u));
    }
}

static pal_menu_row_t STORAGE_DETAIL_STORE[10] NEMA_EXT_BSS;

static void build_storage_detail(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Internal flash"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SLIDER, .label = "Flash", .icon = "files", .get = flash_pct, .min = 0, .max = 100,
                         .step = 1};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Used", .text = used_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Free", .text = free_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Total", .text = total_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Card"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "SD card", .icon = "files", .text = card_text,
                         .hint = "Mount and eject: Settings > Storage, or the sd command"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_STORAGE_DETAIL = {.title = "Storage details", .store = STORAGE_DETAIL_STORE, .cap = 10,
                                     .build = build_storage_detail, .live = true};
