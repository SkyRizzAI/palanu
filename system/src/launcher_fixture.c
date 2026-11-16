/* The launcher as the reference screens show it: the seven entries Apps, Files, Wallets,
 * BadUSB, Settings, Logs and Games, and the title PALANU. uishow launcher SKIN ref selects it (launcher.c); the golden tool
 * compares those screens with the reference pixels. It is not a product list.
 *
 * Icons: Files, Wallets, Logs and Games use the reference's own 8x8 icons. Apps, BadUSB and Settings use PalanuOS's own icons, drawn
 * for this screen, in place of the reference art. */
#include <stdio.h>
#include <string.h>

#include "pal/icons.h"
#include "pal/papp.h"
#include "pal/registry.h"
#include "ui_launcher.h"

static const uint8_t K_FOLDER[8] = {0x00, 0x70, 0xFF, 0x81, 0x81, 0x81, 0xFF, 0x00};
static const uint8_t K_FILE[8] = {0x3E, 0x2A, 0x3E, 0x22, 0x22, 0x22, 0x3E, 0x00};
static const uint8_t K_WALLET[8] = {0x00, 0x7E, 0x42, 0x42, 0x46, 0x46, 0x7E, 0x00};
static const uint8_t K_INFO[8] = {0x18, 0x18, 0x00, 0x38, 0x18, 0x18, 0x3C, 0x00};
static const uint8_t K_APPS[8] = {0xDB, 0xDB, 0x00, 0xDB, 0xDB, 0x00, 0xDB, 0xDB}; /* feature.apps */

static const pal_icon_t REF_ICONS[] = {
    {"ref.folder", 8, 8, 8, K_FOLDER, NULL},
    {"ref.file", 8, 8, 8, K_FILE, NULL},
    {"ref.wallet", 8, 8, 8, K_WALLET, NULL},
    {"ref.info", 8, 8, 8, K_INFO, NULL},
    {"ref.apps", 8, 8, 8, K_APPS, NULL},
};

/* The reference app list: Terminal alone (apps-list.png), with the feature.apps icon the reference gives an app without one. */
static const struct {
    const char *id, *name, *icon;
} APPS[] = {
    {"terminal", "Terminal", "ref.apps"},
};

int ui_fixture_apps(pal_reg_app_t *out, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof(APPS) / sizeof(APPS[0]) && n < max; i++, n++) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].id, sizeof(out[n].id), "%s", APPS[i].id);
        snprintf(out[n].name, sizeof(out[n].name), "%s", APPS[i].name);
        snprintf(out[n].icon, sizeof(out[n].icon), "%s", APPS[i].icon);
        out[n].kind = PAL_KIND_TTY;
        out[n].source = PAL_SRC_BUILTIN;
    }
    return n;
}

static const struct {
    const char *id, *name, *icon;
} ENTRIES[] = {
    {"apps", "Apps", "apps"},
    {"files", "Files", "ref.folder"},
    {"wallets", "Wallets", "ref.wallet"},
    {"badusb", "BadUSB", "default"},
    {"settings", "Settings", "settings"},
    {"logs", "Logs", "ref.file"},
    {"games", "Games", "ref.info"},
};

const pal_icon_t *ui_fixture_icon(const char *name) {
    if (!name || strncmp(name, "ref.", 6) != 0) return NULL;
    for (size_t i = 0; i < sizeof(REF_ICONS) / sizeof(REF_ICONS[0]); i++)
        if (strcmp(name, REF_ICONS[i].name) == 0) return &REF_ICONS[i];
    return NULL;
}

int ui_fixture_entries(pal_reg_app_t *out, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof(ENTRIES) / sizeof(ENTRIES[0]) && n < max; i++, n++) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].id, sizeof(out[n].id), "%s", ENTRIES[i].id);
        snprintf(out[n].name, sizeof(out[n].name), "%s", ENTRIES[i].name);
        snprintf(out[n].icon, sizeof(out[n].icon), "%s", ENTRIES[i].icon);
        out[n].kind = PAL_KIND_GUI;
        out[n].source = PAL_SRC_BUILTIN;
    }
    return n;
}
