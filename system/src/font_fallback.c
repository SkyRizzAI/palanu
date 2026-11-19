/* The fallback font: glyphs for the code points the Spleen fonts do not carry (Armenian, Hebrew, Arabic, Thai, Devanagari, the
 * rest of Latin and symbols, emoji outlines, Nerd Font icons ...), 8x16 (one cell) or 16x16 (two cells), from GNU Unifont and a
 * rasterised subset of Symbols Nerd Font (assets/fonts/unifont/README). Two tiers share one format (forge/tools/fontpack.ts):
 *   - the embedded tier, compressed in the firmware image (raw deflate, about 250 KB) and inflated once into PSRAM (about 700 KB)
 *     at the first glyph it is asked for, so the image keeps room under the OTA slot's limit;
 *   - the file tier /data/unifont-cjk.pfnt (CJK ideographs, Hangul), read glyph by glyph: only the run table is kept in RAM,
 *     and a few recent glyphs. A missing file only means those characters show as an empty box.
 * The desktop task draws the terminal, so the single result buffers are not shared across tasks. */
#include <string.h>

#include "nema/mem.h"
#include "nema/nema.h"
#include "nema/port.h"
#include "nema/vfs.h"
#include "pal/font.h"
#include "../../third_party/miniz/miniz.h" /* tinfl (third_party/miniz) */

extern const uint8_t PAL_FALLBACK_Z[];
extern const size_t PAL_FALLBACK_Z_SIZE, PAL_FALLBACK_BLOB_SIZE;

#define FILE_TIER_PATH "/data/unifont-cjk.pfnt"
#define RUN_SZ 12
#define CACHE_N 16
#define RETRY_MS 10000

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* The run holding cp in a blob's run table (n runs at runs), or NULL. */
static const uint8_t *find_run(const uint8_t *runs, uint32_t n, uint32_t cp) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const uint8_t *r = runs + (size_t)mid * RUN_SZ;
        uint32_t first = rd32(r);
        if (cp < first) hi = mid;
        else if (cp - first >= rd16(r + 4)) lo = mid + 1;
        else return r;
    }
    return NULL;
}

/* ---- the embedded tier ---- */

/* The inflated blob, made at the first call; NULL when that failed (no memory: tried again later, a corrupt stream: never). */
static const uint8_t *blob(void) {
    static uint8_t *b;
    static bool broken;
    static uint32_t next_try;
    if (b || broken) return b;
    uint32_t now = (uint32_t)(nema_port_time_us() / 1000);
    if (next_try && (int32_t)(now - next_try) < 0) return NULL;
    uint8_t *d = nema_alloc(NEMA_MEM_EXTERNAL, NEMA_OWNER_KERNEL, PAL_FALLBACK_BLOB_SIZE);
    tinfl_decompressor *t = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*t)); /* about 11 KB: not on a task's stack */
    if (!d || !t) {
        nema_free(d), nema_free(t);
        next_try = now + RETRY_MS;
        return NULL;
    }
    tinfl_init(t);
    size_t in = PAL_FALLBACK_Z_SIZE, out = PAL_FALLBACK_BLOB_SIZE;
    tinfl_status st = tinfl_decompress(t, PAL_FALLBACK_Z, &in, d, d, &out, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    nema_free(t);
    if (st != TINFL_STATUS_DONE || out != PAL_FALLBACK_BLOB_SIZE || memcmp(d, "PFN1", 4) != 0) {
        nema_free(d);
        broken = true;
        return NULL;
    }
    b = d;
    return b;
}

static const uint8_t *embedded(uint32_t cp, int *cells) {
    const uint8_t *B = blob();
    if (!B) return NULL;
    const uint8_t *r = find_run(B + 12, rd32(B + 4), cp);
    if (!r) return NULL;
    *cells = r[6];
    return B + rd32(B + 8) + rd32(r + 8) + (size_t)(cp - rd32(r)) * (size_t)r[6] * 16;
}

/* ---- the file tier ---- */

static struct {
    nema_obj_t *f;
    uint8_t *runs; /* the run table, malloc'd */
    uint32_t nruns, data_off;
    uint32_t next_try; /* uptime in ms before which a missing file is not looked for again */
    bool tried;
    struct {
        uint32_t cp;
        uint8_t cells, bits[32];
    } cache[CACHE_N];
    int victim;
} T NEMA_EXT_BSS; /* PSRAM: the internal RAM budget of the system is spent (ramcheck) */

static bool read_exact(nema_obj_t *f, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        int r = nema_obj_read(f, p, n, 200);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool seek_to(nema_obj_t *f, uint32_t off) {
    nema_seek_t sk = {.off = (int32_t)off, .whence = 0};
    return nema_obj_ioctl(f, NEMA_FIOC_SEEK, &sk) == NEMA_OK;
}

static void file_close(void) {
    nema_obj_unref(T.f);
    T.f = NULL;
    nema_free(T.runs);
    T.runs = NULL;
    T.nruns = 0;
}

static bool file_open(void) {
    if (T.f) return true;
    uint32_t now = (uint32_t)(nema_port_time_us() / 1000);
    if (T.tried && (int32_t)(now - T.next_try) < 0) return false;
    T.tried = true;
    T.next_try = now + RETRY_MS;
    nema_obj_t *f;
    uint8_t hdr[12];
    if (nema_fs_open(FILE_TIER_PATH, NEMA_O_READ, &f) != NEMA_OK) return false;
    if (!read_exact(f, hdr, sizeof hdr) || memcmp(hdr, "PFN1", 4) != 0) goto bad;
    uint32_t n = rd32(hdr + 4);
    if (n == 0 || n > 4096) goto bad;
    uint8_t *runs = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, (size_t)n * RUN_SZ);
    if (!runs) goto bad;
    if (!read_exact(f, runs, (size_t)n * RUN_SZ)) {
        nema_free(runs);
        goto bad;
    }
    T.f = f;
    T.runs = runs;
    T.nruns = n;
    T.data_off = rd32(hdr + 8);
    memset(T.cache, 0, sizeof T.cache);
    return true;
bad:
    nema_obj_unref(f);
    return false;
}

static const uint8_t *from_file(uint32_t cp, int *cells) {
    if (!file_open()) return NULL;
    for (int i = 0; i < CACHE_N; i++)
        if (T.cache[i].cp == cp && T.cache[i].cells) {
            *cells = T.cache[i].cells;
            return T.cache[i].bits;
        }
    const uint8_t *r = find_run(T.runs, T.nruns, cp);
    if (!r) return NULL;
    int n = r[6];
    uint32_t off = T.data_off + rd32(r + 8) + (cp - rd32(r)) * (uint32_t)n * 16;
    int slot = T.victim;
    T.victim = (T.victim + 1) % CACHE_N;
    if (!seek_to(T.f, off) || !read_exact(T.f, T.cache[slot].bits, (size_t)n * 16)) {
        file_close(); /* the card or the file went away: look again later */
        T.next_try = (uint32_t)(nema_port_time_us() / 1000) + RETRY_MS;
        return NULL;
    }
    T.cache[slot].cp = cp;
    T.cache[slot].cells = (uint8_t)n;
    *cells = n;
    return T.cache[slot].bits;
}

const uint8_t *pal_fallback_glyph(uint32_t cp, int *cells) {
    int c = 0;
    const uint8_t *g = embedded(cp, &c);
    if (!g) g = from_file(cp, &c);
    if (g && cells) *cells = c;
    return g;
}
