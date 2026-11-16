/* Memory. A WebAssembly app allocates from its own linear memory, with the allocator its SDK or libc brings; there is no
 * pal_alloc in this API. The system charges the linear memory to the app and caps it at the manifest's "mem" (KB, 256
 * when not given): memory.grow past the cap fails, so an app never holds more than its manifest says. Native code
 * (the system and the built-in apps) uses the kernel allocator, which has classes and owners. */
#ifndef PALANU_API_MEM_H
#define PALANU_API_MEM_H

#include "annot.h"

#endif
