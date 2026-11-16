/* paltui on the host: a fake backend feeds scripted input at fake times and keeps the output; the output goes
 * into the system terminal emulator (64x30) and the test checks what a user would see: the grid, the highlight, the
 * cursor, and that the wire carries only changes. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/term.h"
#include "test.h"

#define PALTUI_IMPLEMENTATION
#include "paltui/paltui.h"

typedef struct {
    int at;           /* fake ms when the step arrives */
    const char *in;   /* bytes, or NULL */
    int cols, rows;   /* a resize step when cols > 0 */
} step_t;

static struct {
    const step_t *s;
    int n, i, now, cols, rows, caps, outn, fed;
    char out[1 << 16];
} F;
static pal_term_t *T;
static tui_t ui;

static int32_t f_write(const void *b, uint32_t n) {
    if ((size_t)F.outn + n > sizeof(F.out)) n = (uint32_t)(sizeof(F.out) - (size_t)F.outn);
    memcpy(F.out + F.outn, b, n);
    F.outn += (int)n;
    return (int32_t)n;
}
static int32_t f_read(void *buf, uint32_t cap, int32_t timeout) {
    if (F.i >= F.n) {
        if (timeout < 0) return TUI_IO_WAKE_QUIT; /* the script is over: a stop */
        if (timeout > 0) F.now += timeout;
        return 0;
    }
    const step_t *s = &F.s[F.i];
    if (s->at > F.now) {
        if (timeout >= 0 && s->at - F.now > timeout) { F.now += timeout; return 0; }
        F.now = s->at;
    }
    F.i++;
    if (s->cols > 0) {
        F.cols = s->cols;
        F.rows = s->rows;
        return TUI_IO_WAKE_RESIZE;
    }
    uint32_t n = (uint32_t)strlen(s->in);
    if (n > cap) n = cap;
    memcpy(buf, s->in, n);
    return (int32_t)n;
}
static int32_t f_size(int *c, int *r) {
    *c = F.cols;
    *r = F.rows;
    return 0;
}
static int32_t f_caps(void) { return F.caps; }
static int32_t f_now(void) { return F.now; }
static const tui_io_t FAKE = {f_write, f_read, f_size, f_caps, f_now};

#define MONO (TUI_CAP_UTF8 | TUI_CAP_ALT)
#define COLOR256 (TUI_CAP_256 | TUI_CAP_UTF8 | TUI_CAP_ALT | TUI_CAP_16)

/* Starts a session at 64x30 with the given script; the terminal emulator mirrors the screen. */
static void start(const step_t *steps, int n, int caps) {
    memset(&F, 0, sizeof(F));
    F.s = steps;
    F.n = n;
    F.cols = 64;
    F.rows = 30;
    F.caps = caps;
    pal_term_destroy(T);
    T = pal_term_create(64, 30, 0, NEMA_OWNER_KERNEL);
    tui_open(&ui, &FAKE);
    F.fed = 0;
}
static void feed(void) {
    pal_term_write(T, F.out + F.fed, (size_t)(F.outn - F.fed));
    F.fed = F.outn;
}
static int count(const char *needle) {
    int c = 0;
    size_t m = strlen(needle);
    for (int i = 0; i + (int)m <= F.outn; i++)
        if (memcmp(F.out + i, needle, m) == 0) c++;
    return c;
}
static int has(const char *needle) { return count(needle) > 0; }
static int row_is(int row, const char *want) {
    char b[256];
    pal_term_row_text(T, row, b, sizeof(b));
    if (strcmp(b, want) != 0) printf("    row %d: got \"%s\" want \"%s\"\n", row, b, want);
    return strcmp(b, want) == 0;
}
#define ROW(r, s) CHECK(row_is((r), (s)))
static pal_cell_t cell(int c, int r) { return *pal_term_cell(T, c, r); }

/* ---- the app under test: a menu, a name field, a password field, a status line ---- */
static const char *const ITEMS[] = {"alpha", "beta", "gamma", "delta"};
typedef struct {
    int sel;
    char name[64], secret[32];
} app_t;
static void draw(app_t *a) {
    int cols, rows;
    tui_size(&ui, &cols, &rows);
    tui_region(&ui, 0, 0, cols, rows - 1);
    tui_title(&ui, "Hosts");
    tui_menu(&ui, "menu", ITEMS, 4, &a->sel);
    tui_input(&ui, "name", a->name, sizeof(a->name));
    tui_password(&ui, "secret", a->secret, sizeof(a->secret));
    tui_status(&ui, "status line");
}
/* One app frame: wait up to ms for input, draw, emit, show the emulator the new bytes. */
static int app_frame(app_t *a, int32_t ms) {
    int more = tui_frame(&ui, ms);
    draw(a);
    tui_end(&ui);
    feed();
    return more;
}

static const step_t NONE[] = {{0, NULL, 0, 0}};

TEST(first_frame_shows_title_menu_fields_and_status) {
    start(NONE, 0, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    ROW(0, "Hosts");
    ROW(1, "> alpha");
    ROW(2, "  beta");
    ROW(3, "  gamma");
    ROW(4, "  delta");
    ROW(29, " status line");
    CHECK(has("\x1b[?1049h")); /* alternate screen */
    CHECK(has("\x1b[?25l"));   /* cursor hidden */
    CHECK(has("\x1b[?2004h")); /* bracketed paste on */
    CHECK(has("\x1b[?1049h") && pal_term_alt_screen(T));
    /* the selected row is highlighted with the SEL style: a 256-colour background */
    CHECK(cell(0, 1).bg != cell(0, 2).bg);
    tui_close(&ui);
    feed();
}

TEST(selection_is_reverse_in_mono) {
    start(NONE, 0, MONO);
    app_t a = {0};
    app_frame(&a, 0);
    CHECK(cell(0, 1).attr & PAL_TERM_ATTR_REVERSE);
    CHECK(cell(0, 1).attr & PAL_TERM_ATTR_BOLD);
    CHECK(!(cell(0, 2).attr & PAL_TERM_ATTR_REVERSE));
    tui_close(&ui);
    feed();
}

TEST(identical_frame_emits_nothing) {
    start(NONE, 0, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    int before = F.outn;
    app_frame(&a, 0);
    CHECK_EQ(F.outn - before, 0);
    tui_close(&ui);
    feed();
}

TEST(moving_selection_one_row_is_a_small_diff) {
    static const step_t s[] = {{10, "\x1b[B", 0, 0}};
    start(s, 1, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    int before = F.outn;
    app_frame(&a, 100); /* the arrow arrives at 10 ms */
    int sent = F.outn - before;
    printf("    selection step emitted %d bytes\n", sent);
    CHECK(sent > 0);
    CHECK(sent < 120);
    CHECK_EQ(a.sel, 1);
    ROW(1, "  alpha");
    ROW(2, "> beta");
    CHECK(cell(0, 2).bg != cell(0, 1).bg);
    tui_close(&ui);
    feed();
}

TEST(tab_moves_focus_and_typing_goes_to_the_focused_field) {
    /* Tab to the name field, then "é" and "日" (UTF-8), then Tab to the secret and some keys */
    static const step_t s[] = {{10, "\t", 0, 0}, {20, "\xc3\xa9\xe6\x97\xa5", 0, 0}, {30, "\t", 0, 0}};
    start(s, 3, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    app_frame(&a, 15);
    CHECK_EQ(a.name[0], 0);
    for (int k = 0; k < 4; k++) app_frame(&a, 100); /* one key per frame: two characters, then the Tab */
    CHECK(strcmp(a.name, "\xc3\xa9\xe6\x97\xa5") == 0);
    ROW(5, "\xc3\xa9\xe6\x97\xa5");
    uint32_t cp;
    uint8_t st, at;
    CHECK(tui_shown(&ui, 1, 5, &cp, &st, &at) == 0);
    CHECK_EQ(cp, 0x65E5);                  /* 日 leads its cell */
    CHECK(at & TUI_A_WIDE);
    CHECK(tui_shown(&ui, 2, 5, &cp, &st, &at) == 0);
    CHECK(at & TUI_A_CONT);                /* and its second half follows */
    app_frame(&a, 100);
    tui_close(&ui);
    feed();
}

TEST(password_is_masked_on_the_wire_and_kept_in_the_buffer) {
    static const step_t s[] = {{10, "\t\t", 0, 0}, {20, "s3cret", 0, 0}};
    start(s, 2, COLOR256);
    app_t a = {0};
    app_frame(&a, 0); /* the widgets must exist before Tab can move the focus */
    for (int k = 0; k < 10; k++) app_frame(&a, 50); /* one key per frame: two Tabs, six characters */
    CHECK(strcmp(a.secret, "s3cret") == 0);
    CHECK(!has("s3cret"));
    ROW(6, "******");
    tui_close(&ui);
    feed();
}

TEST(lone_escape_comes_after_40_ms) {
    static const step_t s[] = {{100, "\x1b", 0, 0}, {5000, "x", 0, 0}};
    start(s, 2, COLOR256);
    int t0 = F.now;
    (void)t0;
    int got = 0;
    int at = 0;
    for (int i = 0; i < 4 && !got; i++) {
        tui_frame(&ui, 1000);
        const tui_key_t *k = tui_key(&ui);
        if (k && k->sym == TUI_K_ESC) { got = 1; at = F.now; }
    }
    CHECK(got);
    CHECK_EQ(at - 100, 40);
    tui_close(&ui);
    feed();
}

TEST(escape_sequences_decode_to_keys) {
    /* up, down, Ctrl+Right, F5, Alt+x, Ctrl+a, Delete, Backspace, Enter; Tab and Shift-Tab are the frame's */
    static const step_t s[] = {{0, "\x1b[A\x1bOB\x1b[1;5C\x1b[15~\x1bx\x01\x1b[3~\x7f\r\x1b[Z", 0, 0}};
    start(s, 1, COLOR256);
    uint16_t syms[12] = {0};
    uint32_t cps[12] = {0};
    uint8_t mods[12] = {0};
    int n = 0;
    for (int i = 0; i < 12; i++) {
        tui_frame(&ui, 0);
        const tui_key_t *k = tui_key(&ui);
        if (!k) continue;
        syms[n] = k->sym;
        cps[n] = k->cp;
        mods[n] = k->mods;
        n++;
    }
    CHECK_EQ(n, 9);
    CHECK_EQ(syms[0], TUI_K_UP);
    CHECK_EQ(syms[1], TUI_K_DOWN);
    CHECK_EQ(syms[2], TUI_K_RIGHT);
    CHECK_EQ(mods[2], TUI_MOD_CTRL);
    CHECK_EQ(syms[3], TUI_K_F1 + 4);
    CHECK_EQ(syms[4], 0);
    CHECK_EQ(cps[4], 'x');
    CHECK_EQ(mods[4], TUI_MOD_ALT);
    CHECK_EQ(cps[5], 'a');
    CHECK_EQ(mods[5], TUI_MOD_CTRL);
    CHECK_EQ(syms[6], TUI_K_DELETE);
    CHECK_EQ(syms[7], TUI_K_BACKSPACE);
    CHECK_EQ(syms[8], TUI_K_ENTER);
    tui_close(&ui);
}

TEST(bracketed_paste_goes_to_the_focused_field) {
    static const step_t s[] = {{10, "\t", 0, 0}, {20, "\x1b[200~hello world\x1b[201~", 0, 0}};
    start(s, 2, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    app_frame(&a, 50);
    app_frame(&a, 50);
    CHECK(strcmp(a.name, "hello world") == 0);
    ROW(5, "hello world");
    tui_close(&ui);
    feed();
}

TEST(alternate_screen_is_balanced_after_close) {
    start(NONE, 0, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    CHECK(pal_term_alt_screen(T));
    tui_close(&ui);
    feed();
    CHECK(!pal_term_alt_screen(T));
    CHECK_EQ(count("\x1b[?1049h"), 1);
    CHECK_EQ(count("\x1b[?1049l"), 1);
    CHECK_EQ(count("\x1b[?25h"), 1);
    CHECK_EQ(count("\x1b[?2004l"), 1);
}

TEST(stop_request_restores_the_terminal) {
    start(NONE, 0, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    /* the script is over: a wait without a timeout reads a stop */
    int more = tui_frame(&ui, -1);
    CHECK_EQ(more, TUI_QUIT);
    feed();
    CHECK(!pal_term_alt_screen(T));
    CHECK(has("\x1b[?1049l"));
}

TEST(suspend_hands_the_terminal_back_and_resume_repaints) {
    start(NONE, 0, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    CHECK_EQ(tui_suspend(&ui), 0);
    feed();
    CHECK(!pal_term_alt_screen(T));
    CHECK(has("\x1b[?25h"));
    int before = F.outn;
    CHECK_EQ(tui_resume(&ui), 0);
    app_frame(&a, 0);
    CHECK(F.outn - before > 1000); /* the whole screen again */
    ROW(0, "Hosts");
    ROW(1, "> alpha");
    CHECK(pal_term_alt_screen(T));
    tui_close(&ui);
    feed();
}

TEST(resize_repaints_at_the_new_size) {
    static const step_t s[] = {{10, NULL, 50, 20}};
    start(s, 1, COLOR256);
    app_t a = {0};
    app_frame(&a, 0);
    int before = F.outn;
    F.cols = 50;
    F.rows = 20;
    pal_term_resize(T, 50, 20);
    app_frame(&a, 100);
    int cols, rows;
    tui_size(&ui, &cols, &rows);
    CHECK_EQ(cols, 50);
    CHECK_EQ(rows, 20);
    CHECK(F.outn - before > 500);
    ROW(0, "Hosts");
    ROW(19, " status line");
    tui_close(&ui);
    feed();
}

TEST(modal_pick_returns_the_row_the_user_chose) {
    static const step_t s[] = {{0, "\x1b[B", 0, 0}, {10, "\x1b[B", 0, 0}, {20, "\r", 0, 0}};
    start(s, 3, COLOR256);
    CHECK_EQ(tui_pick(&ui, "Pick one", ITEMS, 4), 2);
    feed();
    tui_close(&ui);
}

TEST(modal_confirm_and_prompt) {
    static const step_t yes[] = {{0, "y", 0, 0}};
    start(yes, 1, COLOR256);
    CHECK_EQ(tui_confirm(&ui, "Really delete?"), 1);
    tui_close(&ui);

    static const step_t no[] = {{0, "\x1b", 0, 0}, {100, "\x1b", 0, 0}};
    start(no, 2, COLOR256);
    CHECK_EQ(tui_confirm(&ui, "Really delete?"), 0);
    tui_close(&ui);

    static const step_t typed[] = {{0, "ok\r", 0, 0}};
    start(typed, 1, COLOR256);
    char buf[16] = "";
    CHECK_EQ(tui_prompt(&ui, "Name", buf, sizeof(buf)), 1);
    CHECK(strcmp(buf, "ok") == 0);
    tui_close(&ui);
}

TEST(worst_case_frame_cost) {
    start(NONE, 0, COLOR256);
    /* every cell of the 64x30 screen written by widgets: 30 rows of text plus a status bar */
    static const char *line = "................................................................";
    for (int k = 0; k < 2; k++) {
        tui_frame(&ui, 0);
        tui_region(&ui, 0, 0, 64, 30);
        for (int r = 0; r < 30; r++) tui_label(&ui, line);
        tui_status(&ui, "status");
        tui_end(&ui);
    }
    uint32_t last = 0, max = 0;
    tui_stats(&ui, &last, &max);
    printf("    worst-case widget writes per frame (64x30): last %u, max %u\n", (unsigned)last, (unsigned)max);
    CHECK(last >= 64u * 30u);
    tui_close(&ui);
    feed();
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(first_frame_shows_title_menu_fields_and_status);
    RUN(selection_is_reverse_in_mono);
    RUN(identical_frame_emits_nothing);
    RUN(moving_selection_one_row_is_a_small_diff);
    RUN(tab_moves_focus_and_typing_goes_to_the_focused_field);
    RUN(password_is_masked_on_the_wire_and_kept_in_the_buffer);
    RUN(lone_escape_comes_after_40_ms);
    RUN(escape_sequences_decode_to_keys);
    RUN(bracketed_paste_goes_to_the_focused_field);
    RUN(alternate_screen_is_balanced_after_close);
    RUN(stop_request_restores_the_terminal);
    RUN(suspend_hands_the_terminal_back_and_resume_repaints);
    RUN(resize_repaints_at_the_new_size);
    RUN(modal_pick_returns_the_row_the_user_chose);
    RUN(modal_confirm_and_prompt);
    RUN(worst_case_frame_cost);
    pal_term_destroy(T);
    return TEST_DONE();
}
