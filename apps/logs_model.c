/* The Logs app's model: see logs_model.h. */
#include <stdio.h>
#include <string.h>

#include "logs_model.h"

static const char *const FILTER_NAMES[LOGS_F_COUNT] = {"All", "Info+", "Warn+", "Error"};

const char *logs_filter_name(int f) { return f >= 0 && f < LOGS_F_COUNT ? FILTER_NAMES[f] : "All"; }

bool logs_parse(const char *line, size_t len, logs_line_t *out) {
    if (len < 6) return false;
    uint8_t level;
    switch (line[0]) {
    case 'E': level = 0; break;
    case 'W': level = 1; break;
    case 'I': level = 2; break;
    case 'D': level = 3; break;
    default: return false;
    }
    if (line[1] != ' ' || line[2] != '(') return false;
    size_t i = 3;
    uint32_t ms = 0;
    while (i < len && line[i] >= '0' && line[i] <= '9') ms = ms * 10 + (uint32_t)(line[i++] - '0');
    if (i == 3 || i + 1 >= len || line[i] != ')' || line[i + 1] != ' ') return false;
    i += 2;
    size_t tag = i;
    while (i + 1 < len && !(line[i] == ':' && line[i + 1] == ' ')) i++;
    if (i + 1 >= len) return false;
    size_t tl = i - tag;
    out->level = level;
    out->ms = ms;
    size_t keep = tl < LOGS_COMP - 1 ? tl : LOGS_COMP - 1;
    memcpy(out->comp, line + tag, keep);
    out->comp[keep] = 0;
    out->msg = line + i + 2;
    out->msglen = len - (i + 2);
    return true;
}

void logs_hms(uint32_t ms, char *out, size_t cap) {
    uint32_t s = ms / 1000u;
    snprintf(out, cap, "%02u:%02u:%02u", (unsigned)((s / 3600u) % 100u), (unsigned)((s / 60u) % 60u), (unsigned)(s % 60u));
}

size_t logs_after_marker(const char *buf, size_t len, const char *marker) {
    size_t m = marker ? strlen(marker) : 0;
    if (!m || m > len) return 0;
    for (size_t end = len - m + 1; end-- > 0;) {
        if (memcmp(buf + end, marker, m) != 0) continue;
        if (end > 0 && buf[end - 1] != '\n') continue;
        size_t after = end + m;
        if (after < len && buf[after] == '\n') after++;
        return after;
    }
    return 0;
}

int logs_add_lines(logs_ref_t *refs, int n, int max, const char *buf, size_t len, size_t from, uint8_t src) {
    size_t p = from;
    while (p < len) {
        size_t e = p;
        while (e < len && buf[e] != '\n') e++;
        logs_line_t ln;
        if (e > p && logs_parse(buf + p, e - p, &ln)) {
            if (n >= max) {
                memmove(refs, refs + 1, (size_t)(max - 1) * sizeof(*refs));
                n = max - 1;
            }
            logs_ref_t *r = &refs[n++];
            r->off = (uint32_t)p;
            r->len = (uint16_t)(e - p > 0xFFFF ? 0xFFFF : e - p);
            r->src = src;
            r->level = ln.level;
            r->ms = ln.ms;
            memcpy(r->comp, ln.comp, LOGS_COMP);
        }
        p = e + 1;
    }
    return n;
}

void logs_sort(logs_ref_t *refs, int n) {
    for (int i = 1; i < n; i++) {
        logs_ref_t x = refs[i];
        int j = i - 1;
        while (j >= 0 && refs[j].ms > x.ms) {
            refs[j + 1] = refs[j];
            j--;
        }
        refs[j + 1] = x;
    }
}

int logs_visible(const logs_ref_t *refs, int n, int level_filter, const char *comp, int *vis, int max) {
    int k = 0;
    int worst = level_filter == LOGS_F_ERROR ? 0 : level_filter == LOGS_F_WARN ? 1 : level_filter == LOGS_F_INFO ? 2 : 3;
    for (int i = 0; i < n && k < max; i++) {
        if (refs[i].level > worst) continue;
        if (comp && comp[0] && strcmp(refs[i].comp, comp) != 0) continue;
        vis[k++] = i;
    }
    return k;
}

int logs_components(const logs_ref_t *refs, int n, char names[][LOGS_COMP], int max) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        bool seen = false;
        for (int j = 0; j < k && !seen; j++) seen = strcmp(names[j], refs[i].comp) == 0;
        if (!seen && k < max) {
            snprintf(names[k], LOGS_COMP, "%s", refs[i].comp);
            k++;
        }
    }
    return k;
}

int logs_follow_top(int nvis, int rows) {
    int t = nvis - rows;
    return t < 0 ? 0 : t;
}
