/* The views of the file and log apps (files.c, textview.c, logs.c) and their entry points for other code: the terminal commands
 * (cmd_views.c), the Settings app (Logs filtered by a service), and the Files app (which opens the text viewer). Boot registers
 * the three views (apps/boot.c). */
#ifndef PAL_APPS_VIEWS_H
#define PAL_APPS_VIEWS_H

#include <stddef.h>

#include "pal/desktop.h"

extern pal_view_t pal_files_view;   /* id "files": the file browser, launcher entry */
extern pal_view_t pal_textview_view; /* id "textview": opened as an overlay by Files (Esc closes it) */
extern pal_view_t pal_logs_view;    /* id "logs": the log viewer, launcher entry */

/* Opens Files at path (default /home), from any task. */
void pal_files_open(const char *path);
/* Opens the text viewer over the front view (desktop task only). Binary files show a hex dump. */
void pal_textview_open(const char *path);
/* Opens Logs. component: NULL shows every source; a name (a service id or a log tag) shows only that source. From any task. */
void pal_logs_open(const char *component);

/* The log view hides the lines it holds now (the ring has no clear call); the fixture command uses it for a fixed screen. */
void pal_logs_mark_clear(void);

/* Hidden fixture command and file/log helpers used by the golden scenes (cmd_views.c). */
void pal_apps_register_views(void);

#endif
