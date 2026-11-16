/* Nema: common types and error codes. */
#ifndef NEMA_TYPES_H
#define NEMA_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int nema_err_t;

enum {
    NEMA_OK = 0,
    NEMA_ERR_NOMEM = -1,   /* allocation or admission refused */
    NEMA_ERR_INVAL = -2,   /* bad argument */
    NEMA_ERR_NOENT = -3,   /* not found */
    NEMA_ERR_BUSY = -4,
    NEMA_ERR_TIMEOUT = -5,
    NEMA_ERR_INTR = -6,    /* interrupted by a signal */
    NEMA_ERR_AGAIN = -7,   /* would block */
    NEMA_ERR_PIPE = -8,    /* other end closed */
    NEMA_ERR_PERM = -9,
    NEMA_ERR_NOSYS = -10,
    NEMA_ERR_EXIST = -11,
    NEMA_ERR_IO = -12,
    NEMA_ERR_RANGE = -13,
    NEMA_ERR_BADF = -14,   /* bad handle */
    NEMA_ERR_FULL = -15,   /* fixed table is full */
    NEMA_ERR_STATE = -16,
    NEMA_ERR_NOSPC = -17,
    NEMA_ERR_ISDIR = -18,
    NEMA_ERR_NOTDIR = -19,
    NEMA_ERR_PROTO = -20,
    NEMA_ERR_UNSUPPORTED = -21,
    NEMA_ERR_CANCELED = -22, /* the caller was killed: blocking calls return this at once (docs/adr/0003) */
    NEMA_ERR_AUTH = -23,     /* authentication failed: a MAC, signature or tag did not verify */
    NEMA_ERR_REFUSED = -26,  /* the peer refused the connection (nothing listens there) */
    NEMA_ERR_RESET = -24,    /* the peer reset the connection */
    NEMA_ERR_UNREACH = -25,  /* no route to the host or the network */
};

const char *nema_err_str(int err);

/* Kernel invariant failure. Logs, then aborts through the port. Never returns. */
void nema_panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#define NEMA_CHECK(cond)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) nema_panic("check failed: %s (%s:%d)", #cond, __FILE__, __LINE__);                      \
    } while (0)

/* Bounded copy that always terminates (strncpy does not, and newer compilers warn about it). */
static inline void nema_strlcpy(char *dst, const char *src, size_t size) {
    if (!size) return;
    size_t i = 0;
    for (; i + 1 < size && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

#define NEMA_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* NEMA_EXT_BSS, written after the declarator (`static T buf[N] NEMA_EXT_BSS;`): zero-initialised static data that is big
 * and not on a hot path (UI canvases, screen tables, list stores) goes to external RAM when the board has it, so internal
 * RAM stays free for the radios, DMA and task stacks. The build defines NEMA_HAVE_EXT_BSS where the linker maps
 * .ext_ram.bss to PSRAM (ESP-IDF with CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY); elsewhere it is ordinary .bss.
 * Never for data with an initialiser, DMA buffers, data an ISR or a flash-cache-off path touches, or the kernel's tables. */
#ifdef NEMA_HAVE_EXT_BSS
#define NEMA_EXT_BSS __attribute__((section(".ext_ram.bss")))
#else
#define NEMA_EXT_BSS
#endif

#endif
