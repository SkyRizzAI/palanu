/* The standard streams. fd 0 is stdin, 1 is stdout, 2 is stderr; each may be a terminal, a pipe or a file. An app's
 * stdio is its process handles, so pipes, redirects, Ctrl-C and ps work without code in the app. */
#ifndef PALANU_API_IO_H
#define PALANU_API_IO_H

#include "annot.h"

/* Error codes: negative values are errors (same numbers as the system). */
#define PAL_ERR_AGAIN (-7)
#define PAL_ERR_INTR (-6)
#define PAL_ERR_PERM (-9)

PAL_SINCE(1) PAL_WASM(write)
PAL_DOC("Writes len bytes from buf to fd. Returns the bytes written, or a negative error.")
int32_t pal_write(int32_t fd, PAL_IN(len) const void *buf, uint32_t len);

PAL_SINCE(1) PAL_WASM(read)
PAL_DOC("Reads up to len bytes from fd into buf. Returns the bytes read, 0 at end of input, or a negative error. Blocks; Ctrl-C stops the app.")
int32_t pal_read(int32_t fd, PAL_OUT(len) void *buf, uint32_t len);

#endif
