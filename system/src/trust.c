/* Trust for installed packages (docs/PAPP.md, "Confiança"). The trust store is the files in /etc/trust: each holds one
 * Ed25519 public key, as 32 raw bytes or as 64 hex digits (whitespace allowed). The release keys in release_keys.h are
 * compiled in (empty until the owner makes one). Developer mode is the setting dev.mode=1 in /data/settings. The policy is one pure function, so the
 * host tests can run the whole matrix (tests/unit/test_papp.c). */
#include <stdio.h>
#include <string.h>

#include "crypto_prim.h"
#include "pal/papp.h"
#include "pal/trust.h"
#include "release_keys.h"

#define TRUST_DIR "/etc/trust"
#define SETTINGS_FILE "/data/settings"
#define TRUST_FILE_MAX 160 /* a raw key is 32 bytes; a hex key is 64 digits plus a newline; the rest is ignored */

/* Release keys compiled into the firmware (release_keys.h). Empty until a release key exists; the list is read by psys_trust_load.
 * A zero placeholder keeps the array legal when the list is empty; it is never used as a key. */
static const uint8_t RELEASE_KEYS[][32] = {PAL_RELEASE_KEYS{0}};
#define RELEASE_KEY_SLOTS (sizeof(RELEASE_KEYS) / sizeof(RELEASE_KEYS[0])) /* the keys and the placeholder */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One key from a trust file: 32 raw bytes, or 64 hex digits with optional whitespace around them. */
static bool key_from_file(const uint8_t *b, size_t n, uint8_t out[32]) {
    if (n == 32) {
        memcpy(out, b, 32);
        return true;
    }
    int hi = -1, count = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == ' ' || b[i] == '\n' || b[i] == '\r' || b[i] == '\t') continue;
        int v = hex_nibble((char)b[i]);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
        } else {
            if (count >= 32) return false;
            out[count++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return count == 32 && hi < 0;
}

size_t psys_trust_load(uint8_t (*keys)[32], size_t max) {
    size_t n = 0;
    for (size_t i = 0; i + 1 < RELEASE_KEY_SLOTS && n < max; i++) memcpy(keys[n++], RELEASE_KEYS[i], 32);
    nema_dirent_t e;
    for (size_t i = 0; nema_fs_readdir(TRUST_DIR, i, &e) == NEMA_OK; i++) {
        if (e.type != NEMA_T_FILE || e.name[0] == '.' || n >= max) continue;
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", TRUST_DIR, e.name);
        nema_obj_t *f;
        if (nema_fs_open(path, NEMA_O_READ, &f) != NEMA_OK) continue;
        uint8_t buf[TRUST_FILE_MAX];
        int got = nema_obj_read(f, buf, sizeof(buf), 0);
        nema_obj_unref(f);
        if (got <= 0) continue;
        if (key_from_file(buf, (size_t)got, keys[n])) n++;
        else NEMA_LOGW("trust", "%s: not a 32-byte key or 64 hex digits; ignored", path);
    }
    return n;
}

pal_sig_t psys_trust_verify(const uint8_t hash[32], const uint8_t *sig, const uint8_t (*keys)[32], size_t nkeys) {
    if (!sig) return PAL_SIG_NONE;
    for (size_t i = 0; i < nkeys; i++)
        if (prim_ed25519_verify(keys[i], hash, 32, sig) == NEMA_OK) return PAL_SIG_TRUSTED;
    return PAL_SIG_UNTRUSTED;
}

/* The factory default is PSYS_DEV_MODE_DEFAULT (trust.h); an explicit dev.mode=0 or =1 in the settings file wins. The whole
 * file is read (it is at most 1536 bytes, apps/settings_store.c): a 512 byte read lost the line once the file grew, and an
 * owner's "off" came back as the default. A file that cannot be read leaves the default, not "on". */
#define SETTINGS_READ_MAX 2048

bool psys_dev_mode(void) {
    const bool dflt = PSYS_DEV_MODE_DEFAULT != 0;
    nema_obj_t *f;
    if (nema_fs_open(SETTINGS_FILE, NEMA_O_READ, &f) != NEMA_OK) return dflt;
    char *buf = (char *)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, SETTINGS_READ_MAX); /* not on the caller's stack */
    if (!buf) {
        nema_obj_unref(f);
        return dflt;
    }
    int n = nema_obj_read(f, buf, SETTINGS_READ_MAX - 1, 0);
    nema_obj_unref(f);
    bool result = dflt;
    if (n > 0) {
        buf[n] = 0;
        for (char *line = buf; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (strcmp(line, "dev.mode=1") == 0) { result = true; break; }
            if (strcmp(line, "dev.mode=0") == 0) { result = false; break; }
            line = nl ? nl + 1 : NULL;
        }
    }
    nema_free(buf);
    return result;
}

/* The policy (docs/PAPP.md, "Confiança"):
 *   trusted signature                      accepted
 *   untrusted or no signature, dev mode on  accepted, with a warning
 *   untrusted or no signature, dev mode off refused
 * A package that asks for net, net.listen or crypto.keys is in the same position: it needs a trusted signature, or dev
 * mode, and the refusal says so. note receives the reason (refusal) or the warning (accepted in dev mode). */
nema_err_t psys_policy(pal_sig_t sig, uint32_t caps, bool dev, char *note, size_t cap) {
    if (note && cap) note[0] = 0;
    if (sig == PAL_SIG_TRUSTED) return NEMA_OK;
    bool sensitive = (caps & PAL_PERM_SENSITIVE) != 0;
    const char *why = sig == PAL_SIG_UNTRUSTED ? "its signature is not made by a key in /etc/trust" : "it is not signed";
    if (dev) {
        if (note && cap) snprintf(note, cap, "developer mode: accepting a package that %s", why);
        return NEMA_OK;
    }
    if (note && cap) {
        if (sensitive) snprintf(note, cap, "refused: %s and asks for net or crypto.keys; it needs a trusted signature (or dev.mode=1)", why);
        else snprintf(note, cap, "refused: %s; install it signed with a key in /etc/trust, or set dev.mode=1", why);
    }
    return NEMA_ERR_PERM;
}
