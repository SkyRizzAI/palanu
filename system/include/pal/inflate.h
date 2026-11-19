/* Raw DEFLATE (RFC 1951) into a buffer of the exact size, for data the image keeps compressed (the offline manual). The inflater's
 * state (about 11 KB) is taken from the heap, so a caller's small stack is fine. */
#ifndef PAL_INFLATE_H
#define PAL_INFLATE_H

#include <stddef.h>

#include "nema/types.h"

/* NEMA_OK when z inflates to exactly len bytes in out; NEMA_ERR_PROTO for a corrupt stream; NEMA_ERR_NOMEM. */
nema_err_t pal_inflate(const void *z, size_t zlen, void *out, size_t len);

#endif
