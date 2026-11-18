/* Athena's home widgets: a registry of widgets, each with a variant per theme (its `style` is a theme id, NULL for every theme), and
 * the two widgets the home shows for now, a clock and a calendar, in a Cupertino and a Material version. A widget takes a span of the
 * home's grid (cols x rows) and builds itself in the box it gets; tick runs once a second while the home is in front. Listing, adding,
 * moving and removing widgets come later: the home shows ATH_HOME_WIDGETS in that order. */
#include <stdio.h>
#include <string.h>

#include "athena_int.h"
#include "pal/settings.h"

const char *const ATH_HOME_WIDGETS[] = {"clock", "calendar", NULL};

static const char *const MONTHS[] = {"January", "February", "March",     "April",   "May",      "June",
                                     "July",    "August",   "September", "October", "November", "December"};
static const char *const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

static lv_obj_t *box_style(lv_obj_t *box, uint32_t bg, int radius) {
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    return box;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *text) {
    lv_obj_t *l = ath_text(parent, f, lv_color_hex(color));
    lv_label_set_text(l, text);
    return l;
}

/* ---- clock, Cupertino: an analog face on a black tile, an orange second hand ---- */

static struct {
    lv_obj_t *hand[3];
    lv_point_precise_t pts[3][2];
    int cx, cy, r, last_sec;
} AC;

static void hand_to(int i, int deg, int len) {
    AC.pts[i][0] = (lv_point_precise_t){AC.cx, AC.cy};
    AC.pts[i][1] = (lv_point_precise_t){AC.cx + len * lv_trigo_sin((int16_t)deg) / LV_TRIGO_SIN_MAX,
                                        AC.cy - len * lv_trigo_cos((int16_t)deg) / LV_TRIGO_SIN_MAX};
    lv_line_set_points(AC.hand[i], AC.pts[i], 2);
}

static void analog_build(lv_obj_t *box, int w, int h) {
    box_style(box, 0x1C1C1E, 16);
    int d = (w < h ? w : h) - 14;
    lv_obj_t *face = lv_obj_create(box);
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, d, d);
    lv_obj_center(face);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    AC.cx = AC.cy = d / 2;
    AC.r = d / 2;
    for (int n = 1; n <= 12; n++) { /* the numerals on a ring inside the edge */
        char t[4];
        snprintf(t, sizeof(t), "%d", n);
        lv_obj_t *l = label(face, &lv_font_montserrat_12, 0x1C1C1E, t);
        int rr = AC.r - 11, deg = n * 30;
        lv_obj_align(l, LV_ALIGN_CENTER, rr * lv_trigo_sin((int16_t)deg) / LV_TRIGO_SIN_MAX,
                     -rr * lv_trigo_cos((int16_t)deg) / LV_TRIGO_SIN_MAX);
    }
    static const uint32_t COLOR[3] = {0x1C1C1E, 0x1C1C1E, 0xFF9500};
    static const int WIDTH[3] = {4, 3, 1};
    for (int i = 0; i < 3; i++) {
        AC.hand[i] = lv_line_create(face);
        lv_obj_set_style_line_width(AC.hand[i], WIDTH[i], 0);
        lv_obj_set_style_line_color(AC.hand[i], lv_color_hex(COLOR[i]), 0);
        lv_obj_set_style_line_rounded(AC.hand[i], true, 0);
    }
    lv_obj_t *dot = lv_obj_create(face);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF9500), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    AC.last_sec = -1;
}

static void analog_tick(lv_obj_t *box, const pal_tm_t *t) {
    (void)box;
    if (t->second == AC.last_sec) return;
    AC.last_sec = t->second;
    hand_to(0, (t->hour % 12) * 30 + t->minute / 2, AC.r * 45 / 100);
    hand_to(1, t->minute * 6, AC.r * 70 / 100);
    hand_to(2, t->second * 6, AC.r * 80 / 100);
    bool sec = psys_settings_int("athena.seconds", 1) != 0; /* off in the golden images: their seconds are not fixed */
    if (sec) lv_obj_remove_flag(AC.hand[2], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(AC.hand[2], LV_OBJ_FLAG_HIDDEN);
}

/* ---- clock, Material: big digits, the hour over the minute, the day in a pill ---- */

static struct {
    lv_obj_t *hh, *mm, *day;
    int last;
} DC;

static void digital_build(lv_obj_t *box, int w, int h) {
    (void)w, (void)h;
    box_style(box, 0xE0F2F1, 24);
    DC.hh = label(box, &lv_font_montserrat_48, 0x004D40, "");
    lv_obj_align(DC.hh, LV_ALIGN_TOP_LEFT, 12, 4);
    DC.mm = label(box, &lv_font_montserrat_48, 0x009688, "");
    lv_obj_align(DC.mm, LV_ALIGN_TOP_LEFT, 12, 52);
    lv_obj_t *pill = lv_obj_create(box);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 40, 20);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    box_style(pill, 0x009688, LV_RADIUS_CIRCLE);
    DC.day = label(pill, &lv_font_montserrat_12, 0xFFFFFF, "");
    lv_obj_center(DC.day);
    DC.last = -1;
}

static void digital_tick(lv_obj_t *box, const pal_tm_t *t) {
    (void)box;
    int m = t->hour * 60 + t->minute;
    if (m == DC.last) return;
    DC.last = m;
    lv_label_set_text_fmt(DC.hh, "%02d", t->hour);
    lv_label_set_text_fmt(DC.mm, "%02d", t->minute);
    char d[4] = {0};
    for (int i = 0; i < 3; i++) d[i] = (char)(DAYS[t->wday % 7][i] & ~0x20); /* "MON" */
    lv_label_set_text(DC.day, d);
}

/* ---- calendar: the month's days, today marked ---- */

static int days_in(int month, int year) {
    static const int D[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : D[(month + 11) % 12];
}

static struct {
    lv_obj_t *box;
    int day;
    int w, h;
    bool material;
} CAL;

/* The month as a grid of numbers in [x, x + cw * 7) from y down; today on a marker of color mark. */
static void month_grid(lv_obj_t *box, const pal_tm_t *t, int x, int y, int cw, int rh, bool header, uint32_t ink, uint32_t muted,
                       uint32_t mark, int mark_radius) {
    static const char *const HDR[] = {"S", "M", "T", "W", "T", "F", "S"};
    if (header) {
        for (int c = 0; c < 7; c++) {
            lv_obj_t *l = label(box, &lv_font_montserrat_12, muted, HDR[c]);
            lv_obj_set_width(l, cw);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, x + c * cw, y);
        }
        y += rh;
    }
    int first = ((t->wday - (t->day - 1)) % 7 + 7) % 7; /* the weekday of the 1st */
    int n = days_in(t->month, t->year);
    for (int d = 1; d <= n; d++) {
        int cell = first + d - 1, c = cell % 7, r = cell / 7;
        int px = x + c * cw, py = y + r * rh;
        bool today = d == t->day;
        if (today) {
            lv_obj_t *m = lv_obj_create(box);
            lv_obj_remove_style_all(m);
            lv_obj_set_size(m, cw - 2, rh);
            lv_obj_set_pos(m, px + 1, py - 1);
            box_style(m, mark, mark_radius);
        }
        char s[4];
        snprintf(s, sizeof(s), "%d", d);
        lv_obj_t *l = label(box, &lv_font_montserrat_12, today ? 0xFFFFFF : ink, s);
        lv_obj_set_width(l, cw);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, px, py);
    }
}

static void calendar_fill(const pal_tm_t *t) {
    lv_obj_clean(CAL.box);
    CAL.day = t->day;
    if (!CAL.material) { /* Cupertino: the day large on the left in red, the month's grid on the right */
        lv_obj_t *wd = label(CAL.box, &lv_font_montserrat_12, 0xFF3B30, "");
        char up[4] = {0};
        for (int i = 0; i < 3; i++) up[i] = (char)(DAYS[t->wday % 7][i] & ~0x20); /* "MON" */
        lv_label_set_text(wd, up);
        lv_obj_set_pos(wd, 12, 12);
        lv_obj_t *dn = label(CAL.box, &lv_font_montserrat_48, 0xFF3B30, "");
        lv_label_set_text_fmt(dn, "%d", t->day);
        lv_obj_set_pos(dn, 6, 28);
        lv_obj_t *mo = label(CAL.box, &lv_font_montserrat_12, 0x1C1C1E, "");
        lv_label_set_text_fmt(mo, "%.3s %d", MONTHS[(t->month + 11) % 12], t->year);
        lv_obj_set_pos(mo, 12, CAL.h - 24);
        int cw = CAL.w * 68 / 100 / 7, gx = CAL.w - 7 * cw - 3; /* the grid takes two thirds of the box */
        month_grid(CAL.box, t, gx, 8, cw, 17, true, 0x1C1C1E, 0x8E8E93, 0xFF3B30, LV_RADIUS_CIRCLE);
    } else { /* Material: the month and year as a title, the weekday letters, the grid; today on a teal square */
        lv_obj_t *ti = label(CAL.box, &lv_font_montserrat_12, 0x00796B, "");
        lv_label_set_text_fmt(ti, "%.3s %d", MONTHS[(t->month + 11) % 12], t->year);
        lv_obj_set_width(ti, CAL.w);
        lv_obj_set_style_text_align(ti, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(ti, 0, 6);
        int cw = (CAL.w - 12) / 7;
        month_grid(CAL.box, t, 6, 22, cw, 16, true, 0x263238, 0x607D8B, 0x009688, 4);
    }
}

static void calendar_build(lv_obj_t *box, int w, int h, bool material) {
    CAL.box = box, CAL.w = w, CAL.h = h, CAL.material = material, CAL.day = -1;
    if (material) box_style(box, 0xE0F2F1, 24);
    else box_style(box, 0xFFFFFF, 16);
}
static void cal_cupertino_build(lv_obj_t *box, int w, int h) { calendar_build(box, w, h, false); }
static void cal_material_build(lv_obj_t *box, int w, int h) { calendar_build(box, w, h, true); }

static void calendar_tick(lv_obj_t *box, const pal_tm_t *t) {
    (void)box;
    if (t->day != CAL.day) calendar_fill(t);
}

/* ---- the registry ---- */

static const ath_widget_t WIDGETS[] = {
    {.id = "clock", .style = "cupertino", .cols = 2, .rows = 2, .build = analog_build, .tick = analog_tick},
    {.id = "clock", .style = "material", .cols = 2, .rows = 2, .build = digital_build, .tick = digital_tick},
    {.id = "calendar", .style = "cupertino", .cols = 3, .rows = 2, .build = cal_cupertino_build, .tick = calendar_tick},
    {.id = "calendar", .style = "material", .cols = 3, .rows = 2, .build = cal_material_build, .tick = calendar_tick},
};

/* The widget id's variant for the running theme: one for that theme, else one for every theme. */
const ath_widget_t *ath_widget_find(const char *id) {
    const ath_widget_t *any = NULL;
    for (size_t i = 0; i < sizeof(WIDGETS) / sizeof(WIDGETS[0]); i++) {
        if (strcmp(WIDGETS[i].id, id) != 0) continue;
        if (WIDGETS[i].style && !strcmp(WIDGETS[i].style, ath_T->id)) return &WIDGETS[i];
        if (!WIDGETS[i].style) any = &WIDGETS[i];
    }
    return any;
}

/* The date as the lock screen writes it ("Monday, 16 November"). */
void ath_date_text(const pal_tm_t *t, char *out, size_t cap) {
    snprintf(out, cap, "%s, %d %s", DAYS[t->wday % 7], t->day, MONTHS[(t->month + 11) % 12]);
}
