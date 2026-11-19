/* RTTTL tunes on the buzzer (pal/music.h). Frequencies come from the fourth octave doubled or halved: no floating point. */
#include "pal/music.h"

#include <stdbool.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/device.h"

/* c c# d d# e f f# g g# a a# b in the fourth octave (A4 = 440 Hz) */
static const uint16_t OCT4[12] = {262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494};

static int note_freq(int semitone, int octave) {
    uint32_t f = OCT4[semitone];
    for (int o = octave; o > 4; o--) f *= 2;
    for (int o = octave; o < 4; o++) f /= 2;
    return (int)f;
}

static int num(const char **p) {
    int n = 0;
    while (**p >= '0' && **p <= '9') n = n * 10 + (*(*p)++ - '0');
    return n;
}

static void skip_space(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') (*p)++;
}

int pal_rtttl_parse(const char *text, pal_note_t *out, size_t max, char *name, size_t namecap) {
    const char *p = text, *colon = text ? strchr(text, ':') : NULL;
    if (!colon) return -1;
    if (name && namecap) {
        size_t n = (size_t)(colon - p) < namecap - 1 ? (size_t)(colon - p) : namecap - 1;
        memcpy(name, p, n);
        name[n] = 0;
    }
    p = colon + 1;
    int d = 4, o = 6, b = 63; /* the format's own defaults */
    while (*p && *p != ':') { /* d=4,o=5,b=120 */
        skip_space(&p);
        char k = *p;
        if (!k || k == ':') break;
        p++;
        if (*p != '=') return -1;
        p++;
        int v = num(&p);
        if (k == 'd' || k == 'D') d = v;
        else if (k == 'o' || k == 'O') o = v;
        else if (k == 'b' || k == 'B') b = v;
        skip_space(&p);
        if (*p == ',') p++;
    }
    if (*p != ':' || d <= 0 || b <= 0 || o < 1 || o > 8) return -1;
    p++;
    uint32_t whole = 240000u / (uint32_t)b; /* ms of a whole note: four beats */
    size_t n = 0;
    while (*p && n < max) {
        skip_space(&p);
        if (!*p) break;
        int len = num(&p);
        if (len <= 0) len = d;
        char c = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
        static const char NAMES[] = "c?d?ef?g?a?b";
        int semi = -1;
        if (c == 'p') semi = -2;
        else if (c == 'h') semi = 11; /* German B */
        else
            for (int i = 0; i < 12; i++)
                if (NAMES[i] == c) semi = i;
        if (semi == -1) return -1;
        p++;
        if (*p == '#' && semi >= 0) semi++, p++;
        bool dotted = false;
        if (*p == '.') dotted = true, p++;
        int oct = o;
        if (*p >= '0' && *p <= '9') oct = *p++ - '0';
        if (*p == '.') dotted = true, p++;
        uint32_t ms = whole / (uint32_t)len;
        if (dotted) ms += ms / 2;
        out[n].freq = semi < 0 ? 0 : (uint16_t)note_freq(semi % 12, oct + semi / 12);
        out[n].ms = (uint16_t)(ms > 65535u ? 65535u : ms);
        n++;
        skip_space(&p);
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return (int)n;
}

void pal_tone(int freq_hz, int ms) {
    const pal_device_t *bz = pal_dev_get(PAL_DEV_BUZZER, 0);
    if (bz && freq_hz > 0 && ms > 0) pal_dev_beep(bz, freq_hz, ms);
    if (ms > 0) nema_sleep_ms((uint32_t)ms);
}

nema_err_t pal_music_play(const char *rtttl) {
    if (!pal_dev_get(PAL_DEV_BUZZER, 0)) return NEMA_ERR_NOENT;
    enum { MAXN = 160 };
    pal_note_t *notes = nema_alloc(NEMA_MEM_ANY, nema_self() ? nema_self()->owner : NEMA_OWNER_KERNEL, MAXN * sizeof(pal_note_t));
    if (!notes) return NEMA_ERR_NOMEM;
    int n = pal_rtttl_parse(rtttl, notes, MAXN, NULL, 0);
    nema_err_t r = n < 0 ? NEMA_ERR_INVAL : NEMA_OK;
    for (int i = 0; i < n && r == NEMA_OK; i++) {
        if (nema_interrupted()) r = NEMA_ERR_INTR;
        /* the note a little shorter than its time, so two equal notes are heard as two */
        int on = notes[i].ms > 40 ? notes[i].ms - 15 : notes[i].ms;
        pal_tone(notes[i].freq, on);
        nema_sleep_ms((uint32_t)(notes[i].ms - on));
    }
    nema_free(notes);
    return r;
}

/* Public-domain melodies, written down here: a scale, Twinkle Twinkle Little Star, Beethoven's Ode to Joy, and an alert. */
static const char *const TUNES[][2] = {
    {"scale", "scale:d=8,o=5,b=160:c,d,e,f,g,a,b,c6"},
    {"twinkle", "twinkle:d=4,o=5,b=140:c,c,g,g,a,a,2g,f,f,e,e,d,d,2c"},
    {"ode", "ode:d=4,o=5,b=140:e,e,f,g,g,f,e,d,c,c,d,e,e.,8d,2d"},
    {"alert", "alert:d=16,o=6,b=200:e,g,c7,p,e,g,c7"},
};

const char *pal_music_builtin(const char *name) {
    for (size_t i = 0; i < sizeof(TUNES) / sizeof(TUNES[0]); i++)
        if (name && !strcmp(TUNES[i][0], name)) return TUNES[i][1];
    return NULL;
}

const char *pal_music_builtin_at(size_t i) { return i < sizeof(TUNES) / sizeof(TUNES[0]) ? TUNES[i][0] : NULL; }
