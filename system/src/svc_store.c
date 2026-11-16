/* The choices of the services: /data/services ("id=enabled" or "id=disabled") and the rule that a radio service
 * keeps its choice in the radio settings (psys_svc_builtin_t.on_enabled). The file is read once, at the first use; a
 * choice made before that is never written over the lines not read yet. */

#include <stdio.h>
#include <string.h>

#include "svc_int.h"

/* Sets the choice in the table only. The loader uses it, so it never rewrites the file it is reading. */
static void put(const char *id, uint8_t ovr) {
    saved_t *slot = NULL;
    for (size_t i = 0; i < PSYS_SVC_MAX && !slot; i++)
        if (svc_g->saved[i].used && strcmp(svc_g->saved[i].id, id) == 0) slot = &svc_g->saved[i];
    for (size_t i = 0; i < PSYS_SVC_MAX && !slot; i++)
        if (!svc_g->saved[i].used) slot = &svc_g->saved[i];
    if (!slot) return;
    slot->used = true;
    svc_setstr(slot->id, sizeof(slot->id), id);
    slot->ovr = ovr;
}

void svc_store_load(void) {
    if (svc_g->store_loaded) return;
    svc_g->store_loaded = true;
    nema_obj_t *f;
    if (nema_fs_open(STORE_PATH, NEMA_O_READ, &f) != NEMA_OK) return;
    char buf[STORE_MAX];
    int n = nema_obj_read(f, buf, sizeof(buf) - 1, 0);
    nema_obj_unref(f);
    if (n <= 0) return;
    buf[n] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        if (!svc_valid_id(line)) continue;
        if (strcmp(eq + 1, "enabled") == 0) put(line, OVR_ON);
        else if (strcmp(eq + 1, "disabled") == 0) put(line, OVR_OFF);
    }
}

uint8_t svc_saved_ovr(const char *id) {
    svc_store_load();
    for (size_t i = 0; i < PSYS_SVC_MAX; i++)
        if (svc_g->saved[i].used && strcmp(svc_g->saved[i].id, id) == 0) return svc_g->saved[i].ovr;
    return OVR_NONE;
}

void svc_store_save(void) {
    char text[STORE_MAX];
    size_t n = 0;
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        if (!svc_g->saved[i].used) continue;
        int w = snprintf(text + n, sizeof(text) - n, "%s=%s\n", svc_g->saved[i].id, svc_g->saved[i].ovr == OVR_ON ? "enabled" : "disabled");
        if (w < 0 || (size_t)w >= sizeof(text) - n) break;
        n += (size_t)w;
    }
    nema_obj_t *f;
    if (nema_fs_open(STORE_PATH, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) {
        NEMA_LOGW("svc", "cannot write %s", STORE_PATH);
        return;
    }
    nema_obj_write(f, text, n, 0);
    nema_obj_unref(f);
}

/* Records a choice made by a command or by start/stop, and writes the file. The file is read first, so the write keeps
 * every line that was not read yet. */
static void saved_set(const char *id, uint8_t ovr) {
    svc_store_load();
    put(id, ovr);
    svc_store_save();
}

/* Records the choice: a radio service keeps it in the radio settings (on_enabled), every other service in the file. */
void svc_record_choice(svc_t *e, bool on) {
    e->enabled = on;
    if (!e->installed && e->bi.on_enabled) {
        e->bi.on_enabled(on);
        return;
    }
    saved_set(e->id, on ? OVR_ON : OVR_OFF);
}

void svc_apply_saved(svc_t *e) {
    if (!e->installed && e->bi.on_enabled) return; /* the owner decided at registration */
    uint8_t o = svc_saved_ovr(e->id);
    if (o != OVR_NONE) e->enabled = o == OVR_ON;
    else if (!e->installed) e->enabled = e->bi.autostart;
    else e->enabled = false;
}
