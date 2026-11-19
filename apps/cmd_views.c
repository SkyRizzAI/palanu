/* The terminal commands that open the file and log views (files, logs), and the hidden fixture (viewfix) the golden scenes use:
 * it writes a few known files and log lines, so the screens are the same on every run. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "nema/log.h"
#include "nema/nema.h"
#include "pal/console.h"
#include "pal/desktops.h"
#include "views.h"

static int cmd_files(int argc, char **argv) {
    if (argc > 2) {
        pal_eprintf("usage: files [PATH]\n");
        return 1;
    }
    pal_files_open(argc > 1 ? argv[1] : "/home");
    return 0;
}

static int cmd_logs(int argc, char **argv) {
    if (argc > 2) {
        pal_eprintf("usage: logs [COMPONENT]\n");
        return 1;
    }
    pal_logs_open(argc > 1 ? argv[1] : NULL);
    return 0;
}

static void put(const char *path, const void *data, size_t n) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) return;
    nema_obj_write(f, data, n, -1);
    nema_obj_unref(f);
}

static void make_dir(const char *path) { (void)nema_fs_mkdir(path); }

static const char NOTES[] =
    "Shopping list\n"
    "  - tea, oat milk, two lemons\n"
    "  - batteries (AA) for the remote\n\n"
    "The Palanu notes are short on purpose: one idea per line, and no line\n"
    "longer than the screen unless it is a long sentence like this one, which\n"
    "wraps onto the next row of the viewer without losing a single character.\n"
    "\tTabs expand to the next multiple of four columns.\n\n"
    "Search sample: the lorem line below appears twice.\n"
    "lorem ipsum dolor sit amet, consectetur adipiscing elit.\n"
    "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.\n"
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.\n"
    "Duis aute irure dolor in reprehenderit in voluptate velit esse.\n"
    "lorem again, at the end of the sample.\n";

/* The same 127 bytes as the reference readme.txt (firmware/platforms/wasm: the seeded root file), so the viewer shows the same lines. */
static const char README[] = "Palanu virtual filesystem (in-RAM, volatile).\n"
                             "See examples/ folder for sample apps.\n"
                             "Copy .papp folders to /apps/ or /sd/apps/.\n";

static void write_fixture_files(void) {
    make_dir("/home/fixture");
    make_dir("/home/fixture/archive");
    make_dir("/home/fixture/sd");
    put("/home/fixture/readme.txt", README, sizeof(README) - 1);
    put("/home/fixture/notes.txt", NOTES, sizeof(NOTES) - 1);
    put("/home/fixture/todo.md", "# Todo\n- [ ] flash the board\n- [x] write the files app\n", 58);
    put("/home/fixture/data.csv", "name,value\nalpha,1\nbeta,2\n", 26);
    put("/home/fixture/archive/readme.txt", "inside a folder\n", 16);
    put("/home/fixture/a-file-with-a-rather-long-name-to-test-the-marquee.txt", "long name\n", 10);
    put("/home/fixture/tool.papp", "PLNU-fixture-not-a-real-package", 31);
    put("/home/fixture/image.jpg", "\xff\xd8\xff\xe0 fixture", 13);
    uint8_t bin[512];
    for (size_t i = 0; i < sizeof(bin); i++) bin[i] = (uint8_t)(i * 37 + (i >> 3));
    bin[0] = 0x89;
    bin[1] = 'P';
    bin[2] = 'N';
    bin[3] = 'G';
    put("/home/fixture/photo.png", bin, sizeof(bin));
}

static int cmd_viewfix(int argc, char **argv) {
    if (argc < 2) {
        pal_eprintf("usage: viewfix files | text | hex | readme | logs\n");
        return 1;
    }
    if (!strcmp(argv[1], "files")) {
        write_fixture_files();
        pal_files_open("/home/fixture");
    } else if (!strcmp(argv[1], "text")) {
        write_fixture_files();
        pal_textview_open("/home/fixture/notes.txt");
    } else if (!strcmp(argv[1], "hex")) {
        write_fixture_files();
        pal_textview_open("/home/fixture/photo.png");
    } else if (!strcmp(argv[1], "readme")) {
        write_fixture_files();
        pal_textview_open("/home/fixture/readme.txt");
    } else if (!strcmp(argv[1], "logs")) {
        pal_logs_mark_clear();
        NEMA_LOGD("fixture", "debug line: cache warm");
        NEMA_LOGI("wifi", "scan finished: 4 networks");
        NEMA_LOGI("radio-wifi", "associated with Home-5G");
        NEMA_LOGW("monitor", "heap below 40 KB free");
        NEMA_LOGE("ssh", "connect to 10.0.0.9 failed: timeout");
        NEMA_LOGI("settings", "theme set to dark");
        NEMA_LOGW("notify", "queue is 80%% full");
        NEMA_LOGE("fs", "write to /data/log.txt: no space");
        pal_logs_open(NULL);
    } else {
        pal_eprintf("viewfix: unknown fixture %s\n", argv[1]);
        return 1;
    }
    return 0;
}

/* desktop: the sessions, as `startx` and stopping the display manager are on Linux. Under every session is the system console
 * (Ctrl+Alt+Esc); `use` from the console starts the session and gives it the screen. */
static int cmd_desktop(int argc, char **argv) {
    if (argc == 1 || !strcmp(argv[1], "list")) {
        for (size_t i = 0; i < pal_desktops_count(); i++) {
            const pal_desktop_env_t *e = pal_desktops_get(i);
            pal_printf("%c %-10s %s\n", !strcmp(e->id, pal_desktops_active()) ? '*' : ' ', e->id, e->about ? e->about : e->title);
        }
        if (pal_desktops_count() == 0) pal_printf("this firmware ships no desktop session: the console is the user interface\n");
        else if (!pal_desktops_active()[0]) pal_printf("no session is running (`desktop use ID` starts one)\n");
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "use") || !strcmp(argv[1], "start"))) {
        nema_err_t r = pal_desktops_use(argv[2]);
        if (r == NEMA_ERR_NOENT) pal_eprintf("desktop: no desktop called %s (`desktop list`)\n", argv[2]);
        else if (r == NEMA_ERR_PERM) pal_eprintf("desktop: the device is locked; unlock it first\n");
        else if (r != NEMA_OK) pal_eprintf("desktop: %s did not start; no session is running\n", argv[2]);
        else {
            pal_printf("desktop: %s\n", argv[2]);
            pal_console_hide(); /* started from the console: the session takes the screen */
        }
        return r == NEMA_OK ? 0 : 1;
    }
    if (argc == 2 && !strcmp(argv[1], "stop")) {
        if (!pal_desktops_active()[0]) {
            pal_eprintf("desktop: no session is running\n");
            return 1;
        }
        if (pal_desktops_stop() == NEMA_ERR_PERM) {
            pal_eprintf("desktop: the device is locked; unlock it first\n");
            return 1;
        }
        pal_printf("desktop: stopped; the console has the screen\n");
        return 0;
    }
    pal_eprintf("usage: desktop [list | use ID | start ID | stop]\n");
    return 1;
}

/* edit [FILE]: the text editor on FILE (relative to the shell's directory), or on a new text. */
static int cmd_edit(int argc, char **argv) {
    char path[NEMA_PATH_MAX] = "";
    if (argc > 1 && nema_path_resolve(argv[1], path, sizeof(path)) != NEMA_OK) { /* relative to the shell's directory */
        pal_eprintf("edit: %s: not a path\n", argv[1]);
        return 1;
    }
    pal_editor_open(path);
    return 0;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t VIEW_APPS[] = {
    APP("edit", "edit a text file (Ctrl+S saves, Esc closes)", cmd_edit, 8192),
    APP("files", "open the file browser at PATH (default /home)", cmd_files, 8192),
    APP("desktop", "list the desktop sessions, start one (desktop use ID) or stop it (desktop stop)", cmd_desktop, 8192),
    APP("logs", "open the log viewer, optionally only one source (COMPONENT)", cmd_logs, 8192),
    APP("viewfix", "fixed files, text and log lines for the golden images (files, text, hex, readme, logs)", cmd_viewfix, 8192),
};

void pal_apps_register_views(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(VIEW_APPS); i++) nema_app_register(&VIEW_APPS[i]);
}
