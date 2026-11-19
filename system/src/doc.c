/* Document screens bound to views (pal/doc.h): a table a session reads to draw a view's lines in its own look. */
#include "pal/doc.h"

#include <string.h>

#include "nema/nema.h"
#include "nema/types.h"

static struct {
    char id[24];
    pal_doc_t *d;
} s_bound[4] NEMA_EXT_BSS;

void pal_doc_bind(const char *view_id, pal_doc_t *doc) {
    if (!view_id) return;
    int free_slot = -1;
    for (int i = 0; i < 4; i++) {
        if (s_bound[i].d && !strcmp(s_bound[i].id, view_id)) {
            s_bound[i].d = doc;
            return;
        }
        if (!s_bound[i].d && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || !doc) return;
    nema_strlcpy(s_bound[free_slot].id, view_id, sizeof(s_bound[free_slot].id));
    s_bound[free_slot].d = doc;
}

pal_doc_t *pal_doc_bound(const char *view_id) {
    if (!view_id) return NULL;
    for (int i = 0; i < 4; i++)
        if (s_bound[i].d && !strcmp(s_bound[i].id, view_id)) return s_bound[i].d;
    return NULL;
}
