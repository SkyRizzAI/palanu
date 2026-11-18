/* Settings pages: Display, Appearance, and Controls (with the read-only key map). Every value is a settings key or a live reading;
 * the keys go through psys_settings_set (settings_api.c). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/anim.h"
#include "pal/board.h"
#include "pal/display.h"
#include "pal/input.h"
#include "pal/kui.h"
#include "pal/perf.h"
#include "pal/theme.h"
#include "pal/ui.h"
#include "settings_int.h"

/* ---- Display ---- */

static const int32_t SLEEP_VALS[] = {15, 30, 60, 300, 0};
static const char *const SLEEP_LABELS[] = {"15s", "30s", "1min", "5min", "Off", NULL};
static const settings_map_t MAP_SLEEP = {"display.sleep", NULL, SLEEP_VALS, 5};

static const settings_map_t MAP_LOCK = {"display.lock", NULL, SLEEP_VALS, 5};

/* UI Scale: every screen's zoom (pal/kui.h). 1x shows the most, 2x is the size hhv2's screens were drawn for. */
static const char *const SCALE_TEXTS[] = {"1x", "1.25x", "1.5x", "1.75x", "2x", "2.25x", "2.5x", NULL};
static const settings_map_t MAP_SCALE = {"ui.scale", SCALE_TEXTS, NULL, 7};

/* The terminal's font (term.font): its columns on hhv2's 320 px. Wide terminal UIs and installers' progress bars want 60+. */
static const int32_t TERM_FONT_VALS[] = {0, 1, 2};
static const char *const TERM_FONT_LABELS[] = {"Small", "Medium", "Large", NULL};
static const settings_map_t MAP_TERM_FONT = {"term.font", NULL, TERM_FONT_VALS, 3};

static const char *const ROT_LABELS[] = {"0", "90", "180", "270", NULL};
static const char *const ROT_TEXTS[] = {"0", "90", "180", "270", NULL};
static const settings_map_t MAP_ROT = {"display.rotation", ROT_TEXTS, NULL, 4};

static void info_logical(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_canvas_t *c = pal_display_canvas(); /* the screens' canvas: the panel divided by the UI scale */
    int q = kui_scale_q();
    snprintf(out, cap, "%dx%d", c ? c->w * KUI_Q / q : 0, c ? c->h * KUI_Q / q : 0);
}

static void info_panel(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const pal_board_desc_t *b = pal_board_info();
    snprintf(out, cap, "%dx%d", b->display_w, b->display_h);
}

static void info_scale(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s (board %s)", PAL_UI_SCALE_NAMES[pal_ui_scale()], PAL_UI_SCALE_NAMES[pal_ui_scale_default()]);
}

/* The reference Display page: Sleep After, Lock Screen After, Debug FPS, Status Bar, UI Scale,
 * Rotation (the Panel section), then the Screen section with the panel's size and scale. */
static const char *SCALE_LABELS[] = {"1x", "1.25x", "1.5x", "1.75x", "2x", "2.25x", "2.5x", NULL};

static const pal_menu_row_t DISPLAY_ROWS[] = {
    {.kind = PAL_MENU_SECTION, .label = "Panel"},
    {.kind = PAL_MENU_CHOICE, .label = "Sleep After", .icon = "power", .ctx = (void *)&MAP_SLEEP,
     .choices = SLEEP_LABELS, .get = settings_map_get, .set = settings_map_set,
     .hint = "The backlight turns off after this idle time"},
    {.kind = PAL_MENU_CHOICE, .label = "Lock Screen After", .icon = "lock", .ctx = (void *)&MAP_LOCK,
     .choices = SLEEP_LABELS, .get = settings_map_get, .set = settings_map_set,
     .hint = "Counted from the moment the screen sleeps"},
    {.kind = PAL_MENU_TOGGLE, .label = "Debug FPS", .icon = "monitor", .ctx = (void *)"perf.overlay",
     .get = settings_bool_get, .set = settings_bool_set, .hint = "Frame time and key latency over the screen"},
    {.kind = PAL_MENU_TOGGLE, .label = "Status Bar", .icon = "info", .ctx = (void *)"ui.statusbar",
     .get = settings_bool_get, .set = settings_bool_set, .hint = "The clock and status icons on top", .key = "ui.statusbar"},
    {.kind = PAL_MENU_CHOICE, .label = "UI Scale", .icon = "settings", .ctx = (void *)&MAP_SCALE,
     .choices = SCALE_LABELS, .get = settings_map_get, .set = settings_map_set,
     .hint = "Zoom: 1x shows the most, 2x is the board's size, 2.5x the biggest", .key = "ui.scale"},
    {.kind = PAL_MENU_CHOICE, .label = "Terminal Font", .icon = "terminal", .ctx = (void *)&MAP_TERM_FONT,
     .choices = TERM_FONT_LABELS, .get = settings_map_get, .set = settings_map_set,
     .hint = "Small 5x8: 64 columns, Medium 6x12: 53, Large 8x16: 40"},
    /* the backlight: here for every session (the classic control center has it too) */
    {.kind = PAL_MENU_SLIDER, .label = "Brightness", .icon = "brightness", .ctx = (void *)"display.brightness", .get = settings_int_get,
     .set = settings_int_set, .min = 5, .max = 100, .step = 5, .hint = "The backlight"},
    /* hidden until the panel turns: the setting is stored but the driver and the canvas do not rotate (docs/RELEASE.md) */
    {.kind = PAL_MENU_CHOICE, .label = "Rotation", .icon = "default", .ctx = (void *)&MAP_ROT, .hidden = true,
     .choices = ROT_LABELS, .get = settings_map_get, .set = settings_map_set,
     .hint = "Degrees the screen is turned"},
    {.kind = PAL_MENU_SECTION, .label = "Screen"},
    {.kind = PAL_MENU_INFO, .label = "Logical", .ctx = NULL, .text = info_logical},
    {.kind = PAL_MENU_INFO, .label = "Physical", .ctx = NULL, .text = info_panel},
    {.kind = PAL_MENU_INFO, .label = "Scale", .ctx = NULL, .text = info_scale},
};

pal_menu_page_t PG_DISPLAY = {.title = "Display", .rows = DISPLAY_ROWS, .n = sizeof(DISPLAY_ROWS) / sizeof(DISPLAY_ROWS[0])};

/* ---- Appearances: Theme, Dark Mode, Desktop, Desktop Setting, Launcher, Asset
 * Pack, Font. The theme, animation and font lists come from their tables, so the pages are built on each push. ---- */

#define THEME_SLOTS 8
static const char *s_theme_labels[THEME_SLOTS + 1]; /* what the row shows */
static const char *s_theme_names[THEME_SLOTS + 1];  /* the stored names (ui.theme takes a name) */
static settings_map_t MAP_THEME = {"ui.theme", s_theme_names, NULL, 0};

/* The reference labels for the palettes (Monochrome, Palanu, Orange). */
static const char *theme_label(const char *name) {
    if (!strcmp(name, "mono")) return "Monochrome";
    if (!strcmp(name, "palanu")) return "Palanu";
    if (!strcmp(name, "orange")) return "Orange";
    return name;
}

/* The wallpaper list: the desktop animations (PAL_UI_WALL_NAMES, the ui.wallpaper choices). */
static const char *s_wall_texts[PAL_UI_WALL_COUNT];
static const char *s_wall_labels[PAL_UI_WALL_COUNT + 1];
static settings_map_t MAP_WALL = {"ui.wallpaper", s_wall_texts, NULL, PAL_UI_WALL_COUNT};

static void fill_walls(void) {
    int n = 0;
    for (; n < PAL_UI_WALL_COUNT; n++) {
        s_wall_texts[n] = PAL_UI_WALL_NAMES[n];
        s_wall_labels[n] = PAL_UI_WALL_NAMES[n];
    }
    s_wall_labels[n] = NULL;
}

static const char *const LAUNCH_TEXTS[] = {"strip", "grid", "compact", "list", NULL};
static const char *const LAUNCH_LABELS[] = {"Strip", "Grid", "Compact", "List", NULL};
static const settings_map_t MAP_LAUNCH = {"ui.launcher", LAUNCH_TEXTS, NULL, 4};

/* The animation wallpapers: anim:NAME for each registered animation (Desktop Setting > Wallpaper). */
#define ANIM_SLOTS 12
static const char *s_anim_texts[ANIM_SLOTS + 1];
static const char *s_anim_labels[ANIM_SLOTS + 1];
static char s_anim_text_buf[ANIM_SLOTS][PAL_ANIM_NAME_MAX + 8] NEMA_EXT_BSS;
static char s_anim_label_buf[ANIM_SLOTS][PAL_ANIM_NAME_MAX] NEMA_EXT_BSS;
static settings_map_t MAP_ANIM = {"ui.wallpaper", s_anim_texts, NULL, 0};

static void fill_anims(void) {
    int n = 0; /* the wallpapers ui.wallpaper accepts (PAL_UI_WALL_NAMES), not every registered animation */
    for (int i = 0; i < PAL_UI_WALL_COUNT && n < ANIM_SLOTS; i++, n++) {
        snprintf(s_anim_text_buf[n], sizeof(s_anim_text_buf[n]), "anim:%s", PAL_UI_WALL_NAMES[i]);
        snprintf(s_anim_label_buf[n], sizeof(s_anim_label_buf[n]), "%s", PAL_UI_WALL_NAMES[i]);
        s_anim_texts[n] = s_anim_text_buf[n];
        s_anim_labels[n] = s_anim_label_buf[n];
    }
    s_anim_texts[n] = NULL;
    s_anim_labels[n] = NULL;
    MAP_ANIM.n = n;
}

static const char *const FIT_TEXTS[] = {"fit", "center", "stretch", "crop", NULL};
static const settings_map_t MAP_FIT = {"ui.wall_fit", FIT_TEXTS, NULL, 4};

static const char *const ANCHOR_TEXTS[] = {"top-left", "top", "top-right", "left", "center", "right", "bottom-left",
                                           "bottom", "bottom-right", NULL};
static const settings_map_t MAP_ANCHOR = {"ui.wall_anchor", ANCHOR_TEXTS, NULL, 9};

static const char *const ASSET_TEXTS[] = {"palanu", NULL};
static const settings_map_t MAP_ASSET = {"ui.assets", ASSET_TEXTS, NULL, 1};

#define FONT_SLOTS 8
static const char *s_font_names[FONT_SLOTS + 1];
static settings_map_t MAP_FONT = {"ui.font", NULL, NULL, 0};

static pal_menu_row_t APPEARANCE_STORE[12] NEMA_EXT_BSS;
static pal_menu_row_t DESKTOP_STORE[6] NEMA_EXT_BSS;

static void fill_fonts(void) {
    int n = kui_font_pack_count();
    if (n > FONT_SLOTS) n = FONT_SLOTS;
    for (int i = 0; i < n; i++) s_font_names[i] = kui_font_pack_at(i);
    s_font_names[n] = NULL;
    MAP_FONT.texts = s_font_names;
    MAP_FONT.n = n;
}

static void build_desktop_setting(pal_menu_page_t *p);

static void build_appearance(pal_menu_page_t *p) {
    pal_menu_clear(p);
    int n = pal_theme_count();
    if (n > THEME_SLOTS) n = THEME_SLOTS;
    for (int i = 0; i < n; i++) {
        s_theme_names[i] = pal_theme_name(i);
        s_theme_labels[i] = theme_label(pal_theme_name(i));
    }
    s_theme_labels[n] = NULL;
    s_theme_names[n] = NULL;
    MAP_THEME.n = n;
    fill_fonts();
    fill_walls();
    pal_menu_row_t r;
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Look"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Theme", .icon = "moon", .ctx = &MAP_THEME,
                         .choices = s_theme_labels, .get = settings_map_get, .set = settings_map_set};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Dark Mode", .icon = "moon", .ctx = (void *)"ui.dark",
                         .get = settings_bool_get, .set = settings_bool_set};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Font", .icon = "text", .ctx = (void *)&MAP_FONT,
                         .choices = s_font_names, .get = settings_map_get, .set = settings_map_set,
                         .hint = "Text font of every screen"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Asset Pack", .icon = "package", .ctx = (void *)&MAP_ASSET,
                         .choices = ASSET_TEXTS, .get = settings_map_get, .set = settings_map_set};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Home"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Desktop", .icon = "default", .ctx = (void *)&MAP_WALL,
                         .choices = s_wall_labels, .get = settings_map_get, .set = settings_map_set,
                         .hint = "The home screen background"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Desktop Setting", .icon = "default",
                         .open = settings_open_ctx, .ctx = &PG_DESKTOP_SETTING, .hint = "Wallpaper, fit and anchor"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Launcher", .icon = "apps", .ctx = (void *)&MAP_LAUNCH,
                         .choices = LAUNCH_LABELS, .get = settings_map_get, .set = settings_map_set,
                         .hint = "How the app list is drawn"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_APPEARANCE = {.title = "Appearance", .store = APPEARANCE_STORE, .cap = 12, .build = build_appearance};

static void build_desktop_setting(pal_menu_page_t *p) {
    pal_menu_clear(p);
    fill_anims();
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Wallpaper"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Wallpaper", .ctx = (void *)&MAP_ANIM,
                         .choices = s_anim_labels, .get = settings_map_get, .set = settings_map_set,
                         .hint = "The animation played on the desktop"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Fit", .ctx = (void *)&MAP_FIT,
                         .choices = FIT_TEXTS, .get = settings_map_get, .set = settings_map_set};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Anchor", .ctx = (void *)&MAP_ANCHOR,
                         .choices = ANCHOR_TEXTS, .get = settings_map_get, .set = settings_map_set};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_DESKTOP_SETTING = {.title = "Desktop Setting", .store = DESKTOP_STORE, .cap = 6,
                                      .build = build_desktop_setting};

/* ---- Controls: the board, the seven actions with the key that makes each on this board, and the
 * gestures. The PalanuOS repeat settings, the key map and the keyboard note are under Developer > System > Keys and repeat. ---- */

typedef struct {
    const char *name;
    uint8_t sym; /* the pal_keysym_t the action sends; PAL_KEY_NONE when the board has no key for it */
} ctl_action_t;

static const ctl_action_t CTL_ACTIONS[] = {
    {"Prev", PAL_KEY_LEFT},   {"Next", PAL_KEY_RIGHT},  {"Activate", PAL_KEY_ENTER}, {"Back", PAL_KEY_ESC},
    {"AdjustUp", PAL_KEY_UP}, {"AdjustDown", PAL_KEY_DOWN}, {"Menu", PAL_KEY_NONE},
};

static const char *special_name(uint8_t sym) {
    switch (sym) {
    case PAL_KEY_ENTER: return "Enter";
    case PAL_KEY_ESC: return "Esc";
    case PAL_KEY_LEFT: return "Left";
    case PAL_KEY_RIGHT: return "Right";
    case PAL_KEY_UP: return "Up";
    case PAL_KEY_DOWN: return "Down";
    default: return "Key";
    }
}

/* The printed key that makes sym on the running board: a special key by its name, or a letter with Fn (the hhv2 arrows are
 * Fn with W A S D). False when no key makes it. */
static bool key_hint(uint8_t sym, char *out, size_t cap) {
    const pal_keyboard_t *kb = pal_board_info()->keyboard;
    if (!kb || sym == PAL_KEY_NONE) return false;
    int n = kb->rows * kb->cols;
    for (int i = 0; i < n; i++)
        if (kb->keys[i].kind == PAL_PK_SPECIAL && kb->keys[i].base == sym) {
            snprintf(out, cap, "%s", special_name(sym));
            return true;
        }
    for (int i = 0; i < n; i++)
        if (kb->keys[i].kind == PAL_PK_CHAR && kb->keys[i].base && kb->keys[i].fn == sym) {
            snprintf(out, cap, "Fn+%c", (char)toupper((unsigned char)kb->keys[i].base));
            return true;
        }
    return false;
}

static void action_text(void *ctx, char *out, size_t cap) {
    const uint8_t sym = ((const ctl_action_t *)ctx)->sym;
    if (!key_hint(sym, out, cap)) snprintf(out, cap, "N/A");
}

static void board_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const char *name = pal_board_info()->name;
    snprintf(out, cap, "%s", name && name[0] ? name : "-");
}

static void buttons_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    const pal_keyboard_t *kb = pal_board_info()->keyboard;
    int count = 0;
    for (int i = 0; kb && i < kb->rows * kb->cols; i++)
        if (kb->keys[i].kind != PAL_PK_NONE) count++;
    snprintf(out, cap, "%d", count);
}

static void gesture_long_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, ">= %ums", (unsigned)pal_input_long_press_ms());
}

static void gesture_chord_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "N/A"); /* PalanuOS has no chord gesture */
}

static pal_menu_row_t CONTROLS_STORE[20] NEMA_EXT_BSS;

static void build_controls(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Board"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Board", .text = board_text};
    pal_menu_add(p, &r);
    if (pal_board_info()->keyboard) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Buttons", .text = buttons_text};
        pal_menu_add(p, &r);
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Actions"};
    pal_menu_add(p, &r);
    for (size_t i = 0; i < sizeof(CTL_ACTIONS) / sizeof(CTL_ACTIONS[0]); i++) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = CTL_ACTIONS[i].name, .text = action_text,
                             .ctx = (void *)&CTL_ACTIONS[i]};
        pal_menu_add(p, &r);
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Gestures"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Long Press", .text = gesture_long_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Chord", .text = gesture_chord_text};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_CONTROLS = {.title = "Controls", .store = CONTROLS_STORE, .cap = 20, .build = build_controls};

/* ---- Keys and repeat (PalanuOS extras, under Developer > System): the repeat timing, the long press, the key map ---- */

static void info_longpress(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%u ms", (unsigned)pal_input_long_press_ms());
}

static const pal_menu_row_t KEYS_ROWS[] = {
    {.kind = PAL_MENU_SUBMENU, .label = "Key map", .icon = "terminal", .ctx = (void *)&PG_KEYMAP,
     .open = settings_open_ctx, .hint = "The printed symbols and the Fn layer"},
    {.kind = PAL_MENU_SECTION, .label = "Repeat"},
    {.kind = PAL_MENU_SLIDER, .label = "Repeat delay", .icon = "clock", .ctx = (void *)"input.repeat_delay",
     .get = settings_int_get, .set = settings_int_set, .min = 120, .max = 1000, .step = 20,
     .hint = "Milliseconds before a held key repeats"},
    {.kind = PAL_MENU_SLIDER, .label = "Repeat rate", .icon = "clock", .ctx = (void *)"input.repeat_rate",
     .get = settings_int_get, .set = settings_int_set, .min = 20, .max = 200, .step = 5,
     .hint = "Milliseconds between repeats"},
    {.kind = PAL_MENU_INFO, .label = "Long press", .ctx = NULL, .text = info_longpress,
     .hint = "The cycle key: tap switches app, hold goes home"},
    {.kind = PAL_MENU_SECTION, .label = "Keyboard"},
    {.kind = PAL_MENU_INFO, .label = "On-screen keyboard", .ctx = (void *)"Fn Fn (tap twice)",
     .text = settings_text_static, .hint = "Enter sends Enter and closes it"},
};

pal_menu_page_t PG_KEYS = {.title = "Keys and repeat", .rows = KEYS_ROWS, .n = sizeof(KEYS_ROWS) / sizeof(KEYS_ROWS[0])};

/* The layers of the hhv2 keyboard (docs/KEYMAP.md). Read-only: the table is in boards/hhv2. */
static const pal_menu_row_t KEYMAP_ROWS[] = {
    {.kind = PAL_MENU_SECTION, .label = "Base"},
    {.kind = PAL_MENU_INFO, .label = "Letters and keys", .wrap = true, .ctx = (void *)"Letters, . and space; del, enter, shift, tab, ctrl and alt as printed.",
     .text = settings_text_static},
    {.kind = PAL_MENU_SECTION, .label = "Nm (tap once or hold)"},
    {.kind = PAL_MENU_INFO, .label = "Symbols", .wrap = true,
     .ctx = (void *)"Q #  W 1  E 2  R 3  T (  Y )  U _  I -  O +  P @  A *  S 4  D 5  F 6  G /  H :  J ;  K '  L \"  Z 7  X 8  C 9  V %  B ?  N !  M ,  . $  space 0",
     .text = settings_text_static},
    {.kind = PAL_MENU_SECTION, .label = "Nm + Shift"},
    {.kind = PAL_MENU_INFO, .label = "Extras", .wrap = true,
     .ctx = (void *)"Q [  W ]  E {  R }  T <  Y >  U =  I |  O \\  P ~  A &  S ^  D `",
     .text = settings_text_static},
    {.kind = PAL_MENU_SECTION, .label = "Fn"},
    {.kind = PAL_MENU_INFO, .label = "Navigation", .wrap = true,
     .ctx = (void *)"W A S D arrows  Q Home  E End  R PgUp  F PgDn  del Delete. Fn twice: on-screen keyboard.",
     .text = settings_text_static},
    {.kind = PAL_MENU_SECTION, .label = "Cycle and Alt"},
    {.kind = PAL_MENU_INFO, .label = "Cycle key", .wrap = true,
     .ctx = (void *)"Tap: switch app. Hold: home. Alt+N new tab, Alt+W close, Alt+Tab next.", .text = settings_text_static},
};

pal_menu_page_t PG_KEYMAP = {.title = "Key map", .rows = KEYMAP_ROWS, .n = sizeof(KEYMAP_ROWS) / sizeof(KEYMAP_ROWS[0])};
