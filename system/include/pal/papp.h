/* The .papp container (docs/PAPP.md). Format 1 is one header, a CBOR manifest, a WebAssembly payload and a SHA-256.
 * Format 2 is a header with a section table (manifest, code, icon, resources), per-section raw DEFLATE, the same
 * SHA-256, and an optional Ed25519 signature over that hash. Parsing takes bytes and returns what the file claims, or an
 * error. Nothing is trusted until the hash matches; a signature is trusted only when trust.c says so (papp_pkg.c). */
#ifndef PAL_PAPP_H
#define PAL_PAPP_H

#include "nema/nema.h"

#define PAL_PAPP_MAGIC "PLNU"
#define PAL_PAPP_FORMAT 2     /* the format this firmware writes */
#define PAL_PAPP_FORMAT_V1 1  /* still read: the single-payload layout */
#define PAL_PAPP_API 5        /* the `palanu` import module this firmware provides (api level 5: bus transfers) */
#define PAL_PAPP_MAX_BYTES (1024u * 1024u)       /* the raw code section: larger modules are refused */
#define PAL_PAPP_MAX_FILE (PAL_PAPP_MAX_BYTES + 512u * 1024u) /* the whole file as stored */

/* manifest "perm" entries (docs/API.md, fs and net). "fs" opens /data and /tmp; "fs.home" adds /home and "fs.media" /sd
 * (each implies fs). "net" is TCP and DNS as a client; "net.listen" adds listen and accept. A manifest built for API
 * level 1 that says "fs" predates the namespace, so its "fs" also grants fs.home: its /home keeps working. */
#define PAL_PERM_FS 0x1u          /* fs: /data and /tmp */
#define PAL_PERM_NET 0x2u         /* net: client sockets and DNS */
#define PAL_PERM_FS_HOME 0x4u     /* fs.home: /home */
#define PAL_PERM_FS_MEDIA 0x8u    /* fs.media: /sd */
#define PAL_PERM_NET_LISTEN 0x10u /* net.listen: listen and accept */
#define PAL_PERM_PROC 0x20u       /* proc.spawn: start child processes and signal them (api level 2, proc module) */
#define PAL_PERM_CRYPTO_KEYS 0x40u /* crypto.keys: the keystore (api level 1, keys module) */
#define PAL_PERM_SETTINGS_READ 0x80u   /* settings.read: read the system settings (api level 2, settings module) */
#define PAL_PERM_SETTINGS_WRITE 0x100u /* settings.write: change them (api level 2, settings module) */
#define PAL_PERM_NOTIFY 0x200u         /* notify: post notifications and toasts (api level 2, notify module) */
#define PAL_PERM_SVC_CONTROL 0x400u    /* svc.control: start and stop services (service supervisor, T3) */
#define PAL_PERM_LINK 0x800u           /* link: a byte channel to Forge (Palanu Link, T3) */
#define PAL_PERM_DEV 0x1000u           /* dev: the board's LEDs and buzzers (api level 3, dev module) */
/* Permissions that can reach the network or the keys: they need a trusted signature, or developer mode (trust.c). */
#define PAL_PERM_SENSITIVE (PAL_PERM_NET | PAL_PERM_NET_LISTEN | PAL_PERM_CRYPTO_KEYS)

/* What an app is (the app kind): a service runs in the background, a tty app runs in a terminal, a gui app runs in
 * the desktop. The launcher lists tty and gui apps; services are started by the supervisor. */
typedef enum { PAL_KIND_SERVICE = 0, PAL_KIND_TTY = 1, PAL_KIND_GUI = 2 } pal_kind_t;

/* Where the signature stood when the package was parsed: none, a valid signature by a trusted key, or a signature that
 * no trusted key verifies (or a bad one). Set by the verifier in papp_pkg.c, not by the parser. */
typedef enum { PAL_SIG_NONE = 0, PAL_SIG_TRUSTED = 1, PAL_SIG_UNTRUSTED = 2 } pal_sig_t;

typedef struct {
    char id[24]; /* also the command name; [a-z0-9_-] */
    char name[32];
    char version[16];
    uint16_t api; /* minimum API version the app needs */
    uint16_t format; /* PAL_PAPP_FORMAT_V1 or PAL_PAPP_FORMAT */
    uint32_t perms;
    uint32_t mem_kb;   /* limit on linear memory at start; 0 = 256 */
    uint32_t stack_kb; /* wasm operand/call stack; 0 = 16 */
    uint32_t fuel_m;   /* stop after this many million instructions (0 = unlimited) */
    pal_kind_t kind;
    bool background; /* kind == PAL_KIND_SERVICE; kept for format 1 callers */
    bool hidden;     /* manifest [launcher] hidden: not listed in the launcher */
    char icon[24];     /* icon id for the launcher; empty = the id's default */
    char category[16]; /* launcher category */
    const uint8_t *payload; /* the module: points into the file buffer, or into `owned` for a compressed code section */
    size_t payload_len;
    uint8_t *owned;         /* the inflated module when the code section was compressed (pal_papp_release frees it) */
    uint8_t digest[32];     /* SHA-256 over everything before the hash */
    const uint8_t *sig;     /* the 64-byte Ed25519 signature (points at sigbuf), or NULL when unsigned; do not copy this struct by value */
    uint8_t sigbuf[64];     /* a copy of the signature: the file buffer is freed once the module is inflated */
} pal_papp_t;

/* Validate and parse. Compressed sections are inflated into process-owned memory: release the result with
 * pal_papp_release. Uncompressed payloads point into `file`, which must outlive the result. */
nema_err_t pal_papp_parse(const uint8_t *file, size_t len, pal_papp_t *out);
void pal_papp_release(pal_papp_t *app); /* frees app->owned; safe on a parse that failed or used no compression */
const char *pal_papp_strerror(nema_err_t e);

/* Read a file and parse it; *buf receives the module bytes (nema_free it, or pass it to pal_wasm_run). `out->payload`
 * points into *buf. The bytes are charged to the calling process when there is one, otherwise to the papp subsystem. */
nema_err_t pal_papp_load(const char *path, uint8_t **buf, pal_papp_t *out);

/* Install (docs/PAPP.md, "Installer"): verify, stage to /apps/.staging, move to /apps/<id>/app.papp, register. On
 * failure nothing is left behind, and msg (when given) says why in one line. */
nema_err_t pal_papp_install(const char *path, char *id_out, size_t cap);
nema_err_t pal_papp_install_msg(const char *path, char *id_out, size_t cap, char *msg, size_t msgcap);
/* Removes the command, its registry entry and its files. NEMA_ERR_BUSY while it runs; NEMA_ERR_NOENT if not installed. */
nema_err_t pal_papp_remove(const char *id);

/* Register every package in /apps (boot, and after something wrote one). Returns how many are newly registered. */
int pal_papp_scan(void);

/* Runs a package file once, as `app run FILE` does: the same checks as an installed app, then admission. Returns the
 * exit status (1 when it cannot be read, 126 when refused by the policy, 75 when admission refuses). */
int pal_papp_run_file(const char *path, int argc, char **argv);

/* Runs the module of `app` in the calling process. `image` is the buffer that app->payload points into; ownership
 * passes to this call, which moves the module to its start and frees the buffer as soon as the loader has copied
 * what it keeps. Returns the exit status. */
int pal_wasm_run(const pal_papp_t *app, uint8_t *image, int argc, char **argv);

#endif
