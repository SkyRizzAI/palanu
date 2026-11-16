/* Installed packages: verify, install, scan, run, remove. A package is one file, /apps/<id>.papp (written by Link or
 * cp) or /apps/<id>/app.papp (written by the installer). Every path in here checks the same things, in the same order:
 * hash and structure (papp.c), then the signature and the permissions against the policy (trust.c). A package that
 * passes is a command and a registry entry (registry.c); a package that fails is never run. */
#include <stdio.h>
#include <string.h>

#include "pal/papp.h"
#include "pal/registry.h"
#include "pal/trust.h"

#define STAGING_DIR "/apps/.staging"
#define TRUST_MAX 8
#define APP_STACK (12u * 1024u)
#define EX_TEMPFAIL 75 /* refused for now (admission limit), not broken */

static int papp_main(int argc, char **argv);

static void say(int fd, const char *fmt, const char *a, const char *b) {
    char m[160];
    int n = snprintf(m, sizeof(m), fmt, a, b);
    if (n > 0) nema_write(fd, m, (size_t)(n < (int)sizeof(m) ? n : (int)sizeof(m) - 1));
}

static void set_msg(char *msg, size_t cap, const char *fmt, const char *a, const char *b) {
    if (msg && cap) snprintf(msg, cap, fmt, a ? a : "", b ? b : "");
}

/* The checks every package passes before it is trusted to run: a WebAssembly module, and a signature the policy accepts.
 * The hash and the structure were checked by pal_papp_parse. note gets the warning (accepted) or the refusal reason. */
static nema_err_t check_package(const pal_papp_t *app, char *note, size_t cap) {
    static const uint8_t magic[8] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    if (app->payload_len < 8 || memcmp(app->payload, magic, 8) != 0) {
        set_msg(note, cap, "not a WebAssembly module (version 1)", NULL, NULL);
        return NEMA_ERR_PROTO;
    }
    uint8_t keys[TRUST_MAX][32];
    size_t nkeys = psys_trust_load(keys, TRUST_MAX);
    pal_sig_t sig = psys_trust_verify(app->digest, app->sig, (const uint8_t (*)[32])keys, nkeys);
    return psys_policy(sig, app->perms, psys_dev_mode(), note, cap);
}

static nema_err_t register_command(const pal_papp_t *app) {
    nema_owner_t owner = nema_owner_register("papp");
    nema_app_t *a = nema_calloc(NEMA_MEM_ANY, owner, sizeof(*a) + sizeof(app->id) + sizeof(app->name) + 16);
    if (!a) return NEMA_ERR_NOMEM;
    char *id = (char *)(a + 1);
    char *help = id + sizeof(app->id);
    memcpy(id, app->id, sizeof(app->id));
    snprintf(help, sizeof(app->name) + 16, "%s (installed)", app->name[0] ? app->name : app->id);
    a->name = id;
    a->help = help;
    a->main = papp_main;
    a->stack = APP_STACK;
    nema_err_t r = nema_app_register(a);
    if (r != NEMA_OK) nema_free(a);
    return r;
}

/* Registry entry and command, together: both or neither. */
static nema_err_t register_installed(const pal_papp_t *app, const char *path) {
    nema_err_t r = psys_reg_add_installed(app, path);
    if (r != NEMA_OK) return r;
    r = register_command(app);
    if (r != NEMA_OK) psys_reg_remove(app->id);
    return r;
}

/* Runs a verified package: admission first, then the module in this process. Takes the buffer. */
static int run_loaded(pal_reg_app_t *e, uint8_t *buf, pal_papp_t *app, int argc, char **argv) {
    nema_proc_t *self = nema_self();
    char msg[120];
    nema_err_t r = psys_reg_admit(e, self ? (int32_t)self->pid : 0, msg, sizeof(msg));
    if (r != NEMA_OK) {
        say(2, "%s\n", msg, NULL);
        nema_free(buf);
        return EX_TEMPFAIL;
    }
    return pal_wasm_run(app, buf, argc, argv); /* takes the buffer */
}

static int papp_main(int argc, char **argv) {
    nema_proc_t *self = nema_self();
    pal_reg_app_t e;
    if (!self || !psys_reg_find(self->app->name, &e) || e.source != PAL_SRC_INSTALLED) {
        say(2, "%s: not an installed app\n", self && self->app ? self->app->name : "app", NULL);
        return 126;
    }
    uint8_t *buf;
    pal_papp_t app;
    nema_err_t r = pal_papp_load(e.path, &buf, &app);
    if (r != NEMA_OK) {
        say(2, "%s: %s\n", e.id, pal_papp_strerror(r));
        return 126;
    }
    char note[120];
    r = check_package(&app, note, sizeof(note));
    if (r != NEMA_OK) {
        say(2, "%s: %s\n", e.id, note);
        nema_free(buf);
        pal_papp_release(&app);
        return 126;
    }
    return run_loaded(&e, buf, &app, argc, argv);
}

int pal_papp_run_file(const char *path, int argc, char **argv) {
    uint8_t *buf;
    pal_papp_t app;
    nema_err_t r = pal_papp_load(path, &buf, &app);
    if (r != NEMA_OK) {
        say(2, "app: %s: %s\n", path, pal_papp_strerror(r));
        return 1;
    }
    char note[120];
    r = check_package(&app, note, sizeof(note));
    if (r != NEMA_OK) {
        say(2, "app: %s: %s\n", path, note);
        nema_free(buf);
        return 126;
    }
    pal_reg_app_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.id, sizeof(e.id), "%s", app.id);
    e.kind = app.kind;
    e.source = PAL_SRC_INSTALLED;
    return run_loaded(&e, buf, &app, argc, argv);
}

/* Registers one file of /apps. in_dir: the package lives in /apps/<id>/app.papp, so its folder names it. */
static int scan_one(const char *path, const char *name, size_t idlen, bool in_dir) {
    uint8_t *buf;
    pal_papp_t app;
    nema_err_t r = pal_papp_load(path, &buf, &app);
    if (r != NEMA_OK) {
        NEMA_LOGW("papp", "%s: %s", path, pal_papp_strerror(r));
        return 0;
    }
    char note[120] = "";
    int count = 0;
    r = check_package(&app, note, sizeof(note));
    if (r != NEMA_OK) {
        NEMA_LOGW("papp", "%s: %s", path, note);
    } else {
        if (note[0]) NEMA_LOGW("papp", "%s: %s", path, note);
        if (strlen(app.id) != idlen || strncmp(name, app.id, idlen) != 0) {
            NEMA_LOGW("papp", "%s: %s does not match id '%s'", path, in_dir ? "folder name" : "file name", app.id);
        } else {
            pal_reg_app_t existing;
            if (psys_reg_find(app.id, &existing)) {
                if (existing.source == PAL_SRC_BUILTIN) NEMA_LOGW("papp", "%s: name is taken by a built-in command", app.id);
            } else if (register_installed(&app, path) == NEMA_OK) {
                count = 1;
            } else {
                NEMA_LOGW("papp", "%s: cannot register", app.id);
            }
        }
    }
    nema_free(buf);
    return count;
}

int pal_papp_scan(void) {
    int count = 0;
    nema_dirent_t e;
    for (size_t i = 0; nema_fs_readdir("/apps", i, &e) == NEMA_OK; i++) {
        if (e.name[0] == '.') continue; /* .staging and other hidden entries are never packages */
        size_t n = strlen(e.name);
        char path[72];
        if (e.type == NEMA_T_DIR) {
            snprintf(path, sizeof(path), "/apps/%s/app.papp", e.name);
            count += scan_one(path, e.name, n, true);
        } else if (e.type == NEMA_T_FILE && n > 5 && strcmp(e.name + n - 5, ".papp") == 0) {
            snprintf(path, sizeof(path), "/apps/%s", e.name);
            count += scan_one(path, e.name, n - 5, false);
        }
    }
    return count;
}

static void unlink_quiet(const char *p) { (void)nema_fs_unlink(p); }

/* Deletes a path and, for a folder, everything under it (the app's data/, kv.dat, kv.tmp, and anything else it wrote). */
static void remove_tree(const char *path, int depth) {
    nema_stat_t st;
    if (depth > 8 || nema_fs_stat(path, &st) != NEMA_OK) return;
    if (st.type == NEMA_T_DIR) {
        nema_dirent_t e;
        char child[120];
        /* list by index while deleting: restart each time, since removing an entry shifts the rest */
        for (bool again = true; again;) {
            again = false;
            for (size_t i = 0; nema_fs_readdir(path, i, &e) == NEMA_OK; i++) {
                snprintf(child, sizeof(child), "%s/%s", path, e.name);
                remove_tree(child, depth + 1);
                again = true;
                break;
            }
        }
    }
    (void)nema_fs_unlink(path);
}

/* Install (docs/PAPP.md, "Instalasi"): read, verify, stage, verify the staged copy, move into /apps/<id>/, register. Every
 * failure removes what it made; a refused package leaves /apps as it was. */
nema_err_t pal_papp_install_msg(const char *src, char *id_out, size_t cap, char *msg, size_t mcap) {
    nema_stat_t st;
    nema_err_t r = nema_fs_stat(src, &st);
    if (r != NEMA_OK || st.type != NEMA_T_FILE) {
        set_msg(msg, mcap, "cannot read %s", src, NULL);
        return r != NEMA_OK ? r : NEMA_ERR_ISDIR;
    }
    if (st.size > PAL_PAPP_MAX_FILE) {
        set_msg(msg, mcap, "package is larger than %s", "1.5 MB", NULL);
        return NEMA_ERR_RANGE;
    }
    nema_obj_t *f;
    if (nema_fs_open(src, NEMA_O_READ, &f) != NEMA_OK) {
        set_msg(msg, mcap, "cannot open %s", src, NULL);
        return NEMA_ERR_IO;
    }
    uint8_t *bytes = nema_alloc(NEMA_MEM_EXTERNAL, nema_owner_register("papp"), st.size ? st.size : 1);
    if (!bytes) bytes = nema_alloc(NEMA_MEM_ANY, nema_owner_register("papp"), st.size ? st.size : 1);
    size_t got = 0;
    while (bytes && got < st.size) {
        int n = nema_obj_read(f, bytes + got, st.size - got, 2000);
        if (n <= 0) break;
        got += (size_t)n;
    }
    nema_obj_unref(f);
    if (!bytes || got != st.size) {
        if (bytes) nema_free(bytes);
        set_msg(msg, mcap, "cannot read %s", src, NULL);
        return bytes ? NEMA_ERR_IO : NEMA_ERR_NOMEM;
    }
    pal_papp_t app;
    r = pal_papp_parse(bytes, got, &app);
    if (r != NEMA_OK) {
        set_msg(msg, mcap, "not a valid package: %s", pal_papp_strerror(r), NULL);
        nema_free(bytes);
        return r;
    }
    char note[120];
    r = check_package(&app, note, sizeof(note));
    if (r != NEMA_OK) {
        set_msg(msg, mcap, "%s", note, NULL);
        pal_papp_release(&app);
        nema_free(bytes);
        return r;
    }
    if (note[0]) NEMA_LOGW("papp", "%s: %s", app.id, note);
    pal_reg_app_t existing;
    if (psys_reg_find(app.id, &existing)) {
        bool builtin = existing.source == PAL_SRC_BUILTIN;
        set_msg(msg, mcap, builtin ? "%s is the name of a built-in command" : "%s is already installed: run `app remove %s` first",
                app.id, builtin ? NULL : app.id);
        pal_papp_release(&app);
        nema_free(bytes);
        return NEMA_ERR_EXIST;
    }
    pal_papp_release(&app);

    /* stage: the bytes go to /apps/.staging, and that copy is read back and verified before it is moved */
    char staging[72], dir[64], dst[80];
    snprintf(staging, sizeof(staging), "%s/%s.papp", STAGING_DIR, app.id);
    snprintf(dir, sizeof(dir), "/apps/%s", app.id);
    snprintf(dst, sizeof(dst), "%s/app.papp", dir);
    (void)nema_fs_mkdir("/apps");
    (void)nema_fs_mkdir(STAGING_DIR);
    nema_obj_t *w;
    r = nema_fs_open(staging, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &w);
    if (r == NEMA_OK) {
        int wrote = nema_obj_write(w, bytes, got, 5000);
        nema_obj_unref(w);
        if (wrote != (int)got) r = NEMA_ERR_NOSPC;
    }
    nema_free(bytes);
    bytes = NULL;
    if (r != NEMA_OK) {
        set_msg(msg, mcap, "cannot stage the package: %s", nema_err_str(r), NULL);
        unlink_quiet(staging);
        return r;
    }
    uint8_t *again;
    pal_papp_t check;
    r = pal_papp_load(staging, &again, &check);
    if (r == NEMA_OK) {
        char note2[120];
        r = check_package(&check, note2, sizeof(note2));
        if (r != NEMA_OK) set_msg(msg, mcap, "the staged copy was refused: %s", note2, NULL);
        pal_papp_release(&check);
        nema_free(again);
    } else {
        set_msg(msg, mcap, "the staged copy is damaged: %s", pal_papp_strerror(r), NULL);
    }
    if (r != NEMA_OK) {
        unlink_quiet(staging);
        return r;
    }

    (void)nema_fs_mkdir(dir);
    unlink_quiet(dst);
    r = nema_fs_rename(staging, dst);
    if (r != NEMA_OK) {
        set_msg(msg, mcap, "cannot move the package into %s", dst, NULL);
        unlink_quiet(staging);
        return r;
    }
    pal_papp_t reg;
    uint8_t *img;
    r = pal_papp_load(dst, &img, &reg);
    if (r == NEMA_OK) {
        r = register_installed(&reg, dst);
        nema_free(img);
    }
    if (r != NEMA_OK) {
        set_msg(msg, mcap, "cannot register %s: %s", app.id, nema_err_str(r));
        unlink_quiet(dst);
        (void)nema_fs_unlink(dir);
        return r;
    }
    if (id_out) nema_strlcpy(id_out, reg.id, cap);
    pal_papp_release(&reg);
    return NEMA_OK;
}

nema_err_t pal_papp_install(const char *path, char *id_out, size_t cap) {
    return pal_papp_install_msg(path, id_out, cap, NULL, 0);
}

nema_err_t pal_papp_remove(const char *id) {
    pal_reg_app_t e;
    if (!psys_reg_find(id, &e) || e.source != PAL_SRC_INSTALLED) return NEMA_ERR_NOENT;
    /* The command goes first: refused (NEMA_ERR_BUSY) while it runs, and then nothing has changed. */
    const nema_app_t *a = nema_app_find(id);
    if (a && a->main == papp_main) {
        nema_err_t u = nema_app_unregister(id);
        if (u != NEMA_OK) return u;
        nema_free((void *)a); /* the registration and its id/help text share one block (register_command) */
    }
    psys_reg_remove(id);
    char path[72], dir[64];
    snprintf(dir, sizeof(dir), "/apps/%s", id);
    snprintf(path, sizeof(path), "/apps/%s.papp", id);
    unlink_quiet(path); /* the flat layout, if that is how it was installed */
    remove_tree(dir, 0); /* the folder and everything the app wrote there: data/, kv.dat, kv.tmp */
    return NEMA_OK;
}
