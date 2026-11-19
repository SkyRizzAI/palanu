/* Annotations for the public API definitions in this directory. forge/packages/apigen reads them and generates the SDK
 * header, the WAMR glue, the symbol ledger, and docs/API.md. A C compiler sees empty macros, so every definition header
 * is valid C on its own.
 *
 *   PAL_SINCE(n)      API level that introduced the function (required)
 *   PAL_DOC("text")   one-line description, no "*\/" in it (required)
 *   PAL_CAP(name)     manifest capability the call needs: fs, net, proc.spawn (default: none); a dotted name is a string,
 *                     e.g. PAL_CAP("proc.spawn")
 *   PAL_WASM(name)    wasm import name, when it is not the C name (default: the C name)
 *   PAL_IN(len)       input pointer: const char * with its length parameter len, or const void * with len
 *   PAL_OUT(len)      output buffer: void * with its length parameter len
 *   PAL_INOUT(len)    void * buffer that is read and written (the app's records, e.g. pal_wait); len as for PAL_OUT
 *   PAL_ALIAS(name)   the SDK keeps the old C name too, as a deprecated wrapper with the same signature
 *   PAL_REMOVED(n)    removed in API level n: kept here for the ledger, absent from the SDK and the runtime
 *
 * Types: int32_t, uint32_t, int64_t, uint8_t, pal_handle_t (= int32_t), void, const char * (NUL-terminated unless
 * PAL_IN gives its length), void * and const void * (always with a length, see above). Nothing else: no structs, no
 * pointers to pointers, no callbacks. */
#ifndef PALANU_API_ANNOT_H
#define PALANU_API_ANNOT_H

#include <stdint.h>

#define PAL_API_LEVEL 7

#define PAL_SINCE(n)
#define PAL_DOC(text)
#define PAL_CAP(name)
#define PAL_WASM(name)
#define PAL_IN(len)
#define PAL_OUT(len)
#define PAL_INOUT(len)
#define PAL_ALIAS(name)
#define PAL_REMOVED(n)

typedef int32_t pal_handle_t;

#endif
