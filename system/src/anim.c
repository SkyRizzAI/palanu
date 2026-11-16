/* The .panim reader (docs/ANIM.md): the header, the palette, the playback order and the bitmaps, checked against the size before
 * anything is used. Versions 1 and 2 are read, byte for byte as the reference codec writes them. The built-in animations are read in
 * place from flash; a file is copied into one PSRAM block, so a bad file never leaves a half-built animation behind. */
#include <stdio.h>
#include <string.h>

#include "anim_int.h"
#include "nema/mem.h"
#include "nema/vfs.h"
#include "pal/anims_data.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

nema_err_t anim_parse(const uint8_t *d, size_t len, anim_parsed_t *p) {
    memset(p, 0, sizeof(*p));
    if (!d || len < 5) return NEMA_ERR_INVAL;
    if (d[0] != 'P' || d[1] != 'A' || d[2] != 'N' || d[3] != 'M') return NEMA_ERR_INVAL;
    int version = d[4];
    if (version != 1 && version != 2) return NEMA_ERR_INVAL;
    size_t off = version == 1 ? 14 : 16;
    if (len < off) return NEMA_ERR_INVAL;
    int w = rd16(d + 5), h = rd16(d + 7);
    int fps = d[9], passive = d[10], active = d[11], uniq = d[12], order_len = d[13];
    if (w <= 0 || h <= 0 || w > PAL_ANIM_MAX_W || h > PAL_ANIM_MAX_H || uniq == 0 || fps == 0) return NEMA_ERR_INVAL;
    bool indexed = false;
    if (version == 2) {
        int pf = d[14], ext = d[15];
        if (pf > 1) return NEMA_ERR_INVAL;
        if (off + (size_t)ext > len) return NEMA_ERR_INVAL;
        off += (size_t)ext; /* TLV extensions are skipped: readers ignore what they do not know */
        indexed = pf == 1;
        if (indexed) {
            if (off + 1 > len) return NEMA_ERR_INVAL;
            int count = d[off++];
            if (count < 1 || count > ANIM_MAX_PAL) return NEMA_ERR_INVAL;
            if (off + (size_t)count * 3 > len) return NEMA_ERR_INVAL;
            p->npal = (uint8_t)count;
            for (int i = 0; i < count; i++) {
                p->pal_rgb[i] = rd16(d + off);
                p->pal_mo[i] = d[off + 2];
                if (p->pal_mo[i] > 2) return NEMA_ERR_INVAL;
                off += 3;
            }
        }
    }
    if (off + (size_t)order_len > len) return NEMA_ERR_INVAL;
    p->has_order = order_len > 0;
    p->order_off = off;
    for (int i = 0; i < order_len; i++)
        if (d[off + (size_t)i] >= uniq) return NEMA_ERR_INVAL;
    off += (size_t)order_len;
    size_t bpf = indexed ? (size_t)((w + 1) / 2) * (size_t)h : (size_t)((w + 7) / 8) * (size_t)h;
    if (off + (size_t)uniq * bpf > len) return NEMA_ERR_INVAL;
    int positions = p->has_order ? order_len : uniq;
    if (passive > positions || active > positions) return NEMA_ERR_INVAL;
    p->frame0 = off;
    p->bpf = bpf;
    p->info.version = version;
    p->info.width = w;
    p->info.height = h;
    p->info.fps = fps;
    p->info.frames = uniq;
    p->info.positions = positions;
    p->info.passive = passive;
    p->info.active = active;
    p->info.indexed = indexed;
    p->info.bytes = len;
    return NEMA_OK;
}

int anim_bitmap_at(const pal_anim_t *a, int ph) {
    if (ph < 0) ph = 0;
    if (ph >= a->p.info.positions) ph = a->p.info.positions - 1;
    if (!a->p.has_order) return ph;
    return a->data[a->p.order_off + (size_t)ph];
}

nema_err_t pal_anim_parse(const uint8_t *data, size_t len, pal_anim_info_t *out) {
    anim_parsed_t p;
    nema_err_t e = anim_parse(data, len, &p);
    if (e == NEMA_OK && out) *out = p.info;
    return e;
}

/* A name: 1 to 23 characters of a-z, 0-9 and '-'. Anything else is never a file or a built-in. */
static bool name_ok(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n >= PAL_ANIM_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static const pal_anim_blob_t *find_builtin(const char *name) {
    for (size_t i = 0; i < PAL_ANIM_BLOBS_COUNT; i++)
        if (strcmp(PAL_ANIM_BLOBS[i].name, name) == 0) return &PAL_ANIM_BLOBS[i];
    return NULL;
}

static pal_anim_t *new_anim(const char *name, void *block, size_t block_bytes) {
    pal_anim_t *a = block;
    memset(a, 0, sizeof(*a));
    a->owned = block;
    a->block_bytes = block_bytes;
    nema_strlcpy(a->name, name, sizeof(a->name));
    return a;
}

pal_anim_t *pal_anim_open_blob(const char *name, const uint8_t *data, size_t len) {
    if (!name_ok(name) || !data) return NULL;
    /* a borrowed animation still needs its struct: a small block, not PSRAM */
    void *block = nema_alloc(NEMA_MEM_ANY, nema_owner_register("anim"), sizeof(pal_anim_t));
    if (!block) return NULL;
    pal_anim_t *a = new_anim(name, block, sizeof(pal_anim_t));
    if (anim_parse(data, len, &a->p) != NEMA_OK) {
        nema_free(a->owned);
        return NULL;
    }
    a->data = data;
    return a;
}

pal_anim_t *pal_anim_open_builtin(const char *name) {
    const pal_anim_blob_t *b = name_ok(name) ? find_builtin(name) : NULL;
    if (!b) return NULL;
    return pal_anim_open_blob(name, b->data, b->len);
}

/* Reads a whole file into one PSRAM block after the struct. A file over the limit or a short read is refused before parsing. */
pal_anim_t *pal_anim_open_file(const char *name) {
    if (!name_ok(name)) return NULL;
    char path[sizeof(PAL_ANIM_DIR) + PAL_ANIM_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.panim", PAL_ANIM_DIR, name);
    nema_stat_t st;
    if (nema_fs_stat(path, &st) != NEMA_OK || st.size < 5 || st.size > PAL_ANIM_MAX_BYTES) return NULL;
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_READ, &f) != NEMA_OK) return NULL;
    nema_owner_t owner = nema_owner_register("anim");
    pal_anim_t *a = nema_alloc(NEMA_MEM_EXTERNAL, owner, sizeof(pal_anim_t) + st.size);
    if (!a) {
        nema_obj_unref(f);
        return NULL;
    }
    a = new_anim(name, a, sizeof(pal_anim_t) + st.size);
    uint8_t *bytes = (uint8_t *)(a + 1);
    size_t got = 0;
    while (got < st.size) {
        int n = nema_obj_read(f, bytes + got, st.size - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    nema_obj_unref(f);
    if (got != st.size || anim_parse(bytes, got, &a->p) != NEMA_OK) {
        nema_free(a->owned);
        return NULL;
    }
    a->data = bytes;
    return a;
}

size_t pal_anim_memory(const pal_anim_t *a) { return a ? a->block_bytes : 0; }

void pal_anim_close(pal_anim_t *a) {
    if (a) nema_free(a->owned);
}

const pal_anim_info_t *pal_anim_info(const pal_anim_t *a) { return a ? &a->p.info : NULL; }
const char *pal_anim_name(const pal_anim_t *a) { return a ? a->name : ""; }

static bool list_has(char names[][PAL_ANIM_NAME_MAX], int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) return true;
    return false;
}

int pal_anim_list(char names[][PAL_ANIM_NAME_MAX], int max) {
    int n = 0;
    for (size_t i = 0; i < PAL_ANIM_BLOBS_COUNT && n < max; i++) {
        if (!name_ok(PAL_ANIM_BLOBS[i].name) || list_has(names, n, PAL_ANIM_BLOBS[i].name)) continue;
        nema_strlcpy(names[n++], PAL_ANIM_BLOBS[i].name, PAL_ANIM_NAME_MAX);
    }
    nema_dirent_t ent;
    for (size_t i = 0; n < max && nema_fs_readdir(PAL_ANIM_DIR, i, &ent) == NEMA_OK; i++) {
        size_t len = strlen(ent.name);
        if (len <= 6 || strcmp(ent.name + len - 6, ".panim") != 0) continue;
        char base[PAL_ANIM_NAME_MAX];
        size_t bl = len - 6;
        if (bl >= sizeof(base)) continue;
        memcpy(base, ent.name, bl);
        base[bl] = 0;
        if (!name_ok(base) || list_has(names, n, base)) continue;
        nema_strlcpy(names[n++], base, PAL_ANIM_NAME_MAX);
    }
    return n;
}

bool pal_anim_exists(const char *name) {
    if (!name_ok(name)) return false;
    if (find_builtin(name)) return true;
    char path[sizeof(PAL_ANIM_DIR) + PAL_ANIM_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.panim", PAL_ANIM_DIR, name);
    nema_stat_t st;
    return nema_fs_stat(path, &st) == NEMA_OK;
}
