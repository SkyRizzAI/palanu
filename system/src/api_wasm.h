/* The WAMR side of the `palanu` import module. api_wasm_glue.c (generated) calls the hooks below; wasm_rt.c
 * implements them and registers the table. Nothing else includes WAMR for the API. */
#ifndef SYSTEM_API_WASM_H
#define SYSTEM_API_WASM_H

#include <stdbool.h>
#include <stdint.h>

#include "api_ctx.h"
#include "wasm_export.h"

extern NativeSymbol api_palanu_natives[];
extern uint32_t api_palanu_natives_count;

api_ctx_t *api_ctx_of(wasm_exec_env_t env);
bool api_cap_ok(wasm_exec_env_t env, const char *cap); /* the manifest grants the capability */
void api_after(wasm_exec_env_t env, api_ctx_t *c);     /* ends the app if the implementation asked to stop */

#endif
