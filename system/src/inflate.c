/* pal_inflate (pal/inflate.h): tinfl from third_party/miniz, its state on the heap. */
#include "pal/inflate.h"

#include "../../third_party/miniz/miniz.h"
#include "nema/nema.h"

nema_err_t pal_inflate(const void *z, size_t zlen, void *out, size_t len) {
    tinfl_decompressor *t = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*t));
    if (!t) return NEMA_ERR_NOMEM;
    tinfl_init(t);
    size_t in = zlen, n = len;
    tinfl_status st = tinfl_decompress(t, (const uint8_t *)z, &in, (uint8_t *)out, (uint8_t *)out, &n, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    nema_free(t);
    return st == TINFL_STATUS_DONE && n == len ? NEMA_OK : NEMA_ERR_PROTO;
}
