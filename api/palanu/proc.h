/* The app itself and its child processes: how it ends, what it was started with, the environment, and the commands it
 * starts. A command line is argv[0] (the command name), then the arguments the user typed. Children are processes like
 * any other: they share the app's stdin, stdout and stderr, and join its process group, so Ctrl-C reaches them too.
 * An app may only wait for or signal the children it started. A child it neither waited for nor detached is reaped by
 * the system when the app ends. */
#ifndef PALANU_API_PROC_H
#define PALANU_API_PROC_H

#include "annot.h"

/* Signals for pal_signal. The values are the system's: INT is Ctrl-C, TERM asks the child to return (it is killed if it
 * is still running after 3 seconds), KILL ends it at once, HUP is a hangup. */
#define PAL_SIG_INT 0x01
#define PAL_SIG_TERM 0x02
#define PAL_SIG_KILL 0x04
#define PAL_SIG_HUP 0x08

/* pal_spawn flags: the child is not waited for by anyone; it is reaped when it exits. */
#define PAL_SPAWN_DETACH 0x01

PAL_SINCE(1) PAL_WASM(exit)
PAL_DOC("Ends the app with the given status. The status is what the shell sees.")
void pal_exit(int32_t code);

PAL_SINCE(1) PAL_WASM(argc)
PAL_DOC("Number of command line words, including the command name.")
int32_t pal_argc(void);

PAL_SINCE(1) PAL_WASM(arg) PAL_ALIAS(pal_arg)
PAL_DOC("Copies word i (0 is the command name) into buf, NUL-terminated. Returns its length, or a negative error if it does not fit.")
int32_t pal_argv(int32_t i, PAL_OUT(cap) void *buf, uint32_t cap);

PAL_SINCE(2) PAL_CAP("proc.spawn") PAL_WASM(spawn)
PAL_DOC("Starts the registered command app as a child. args holds the argument words after the command name, each ended by NUL (args_len bytes, at most 255; 0 for none). flags is 0 or PAL_SPAWN_DETACH. Returns a child handle (usable only by this app), or a negative error.")
int32_t pal_spawn(const char *app, PAL_IN(args_len) const char *args, uint32_t args_len, int32_t flags);

PAL_SINCE(2) PAL_WASM(child_wait)
PAL_DOC("Waits for a child this app started (timeout_ms as in pal_wait). When it has ended, writes its status into code_out (4 bytes, little-endian int32) and returns 1. Returns 0 on timeout, or a negative error (not one of this app's children, or the app was stopped).")
int32_t pal_child_wait(int32_t child, PAL_OUT(len) void *code_out, uint32_t len, int32_t timeout_ms);

PAL_SINCE(2) PAL_WASM(signal)
PAL_DOC("Sends a PAL_SIG_* signal to a child this app started. Returns 0, or a negative error (not one of this app's children, or unknown signal).")
int32_t pal_signal(int32_t child, int32_t sig);

PAL_SINCE(2) PAL_WASM(getenv)
PAL_DOC("Copies the value of the environment variable key into buf, NUL-terminated. Returns its length, or a negative error if the variable is not set or the value does not fit. Set: HOME (/home), USER (palanu), TERM (xterm-256color), PWD (the app's working directory).")
int32_t pal_getenv(const char *key, PAL_OUT(cap) void *buf, uint32_t cap);

#endif
