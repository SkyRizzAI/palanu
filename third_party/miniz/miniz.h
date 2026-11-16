/* Entry header of the vendored tinfl (third_party/miniz). Derived from miniz 3.1.0 miniz.h: it keeps only what the
 * inflater needs. No heap (MINIZ_NO_MALLOC: system/ allocates through nema_alloc), no stdio, no time, no zip and no
 * deflate APIs. Unaligned loads are off, so the code reads bytes and runs the same on the host and the chip. */
#ifndef MINIZ_MINIMAL_H
#define MINIZ_MINIMAL_H

#define MINIZ_NO_MALLOC
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_HAS_64BIT_REGISTERS 0

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define MINIZ_LITTLE_ENDIAN 1
#else
#define MINIZ_LITTLE_ENDIAN 0
#endif

#include "miniz_common.h"
#include "miniz_tinfl.h"

#endif
