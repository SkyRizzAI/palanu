/* Helpers shared by the built-in apps: formatted output on stdout/stderr and a few terminal queries. */
#ifndef PAL_LIBAPP_H
#define PAL_LIBAPP_H

#include <stdarg.h>

#include "nema/nema.h"

int pal_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int pal_eprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int pal_puts(const char *s); /* no newline added */
bool pal_isatty(int fd);
void pal_term_size(int *cols, int *rows); /* of stdout; 80x24 if it is not a terminal */
const char *pal_strerror(int err);

/* Read a whole stream into a growing buffer (owner: the calling process). Caller frees with nema_free. */
char *pal_read_all(int fd, size_t *len);

/* A line without echo (passwords): stdin in line mode, echo off; the newline is not kept. Returns the length, -1 at end. */
int pal_read_password(char *out, size_t cap);

/* Register every built-in app with the kernel. */
void pal_apps_register_all(void);

#endif
