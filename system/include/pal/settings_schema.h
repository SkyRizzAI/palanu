/* The settings schema (ADR-0010 §7): every settings key, once, as data. A descriptor says what a key is (its kind, range or choices),
 * how a person reads it (label, group, help), and whose it is: the system's, shown by every session, or one session's (its look),
 * shown only while that session runs. Values are read and written through psys_settings_get/set as before; the schema adds no
 * storage. A Settings screen lays the descriptors out with its own components (pal_settings_schema_rows builds menu rows of them),
 * and tests/unit/test_settings_reach.c checks that every system key is reachable in the Settings of every session. */
#ifndef PAL_SETTINGS_SCHEMA_H
#define PAL_SETTINGS_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PAL_SETTING_BOOL,
    PAL_SETTING_INT,    /* min..max in step */
    PAL_SETTING_CHOICE, /* one of names (stored as the name) */
    PAL_SETTING_TEXT,   /* free text up to max characters */
    PAL_SETTING_PIN,    /* write only: 4 to 8 digits, or empty to clear */
} pal_setting_kind_t;

typedef struct {
    const char *key;
    pal_setting_kind_t kind;
    const char *label;
    const char *group;  /* the Settings group it belongs to */
    const char *help;   /* one line, or NULL */
    const char *owner;  /* NULL: the system's; else the id of the session it belongs to */
    int32_t min, max, step;
    const char *const *names;  /* CHOICE: stored names, NULL-terminated; NULL with dynamic */
    const char *const *labels; /* CHOICE: what a person reads, same order; NULL = the names */
    bool dynamic;       /* CHOICE whose names come from the system at run time (desktop.env: the registered sessions) */
    bool internal;      /* set by commands or not effective yet: no Settings screen has to show it */
    uint32_t caps;      /* PAL_CAP_* the board needs for it to mean something (0: none); without them no screen has to show it */
} pal_setting_desc_t;

size_t pal_settings_schema_count(void);
const pal_setting_desc_t *pal_settings_schema_get(size_t i);
const pal_setting_desc_t *pal_settings_schema_find(const char *key);
/* The key belongs to no session, or to the one running now. Unknown keys are visible. */
bool pal_setting_visible(const char *key);

/* Menu rows for the keys of one owner (NULL: the system's; a session id: that session's look), optionally of one group (NULL: all),
 * appended to a page under construction (its build). Each row reads and writes through psys_settings_get/set; a CHOICE row of
 * desktop.env lists the registered sessions and switches to the one picked. Rows a menu cannot show (dynamic choices other than
 * desktop.env, internal keys) are left out. */
struct pal_menu_page;
void pal_settings_schema_rows(struct pal_menu_page *p, const char *owner, const char *group);

#endif
