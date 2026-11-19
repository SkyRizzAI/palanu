/* RTTTL tunes (pal/music.h): the header's defaults, lengths and tempo in milliseconds, dotted notes, sharps, octaves, rests,
 * the built-in tunes, and text that is not RTTTL. */
#include <string.h>

#include "pal/music.h"
#include "test.h"

TEST(notes_lengths_and_octaves) {
    pal_note_t n[16];
    char name[16];
    int c = pal_rtttl_parse("test:d=4,o=5,b=120:c,8e.,g#6,p,2a4,h", n, 16, name, sizeof(name));
    CHECK_EQ(c, 6);
    CHECK(!strcmp(name, "test"));
    CHECK(n[0].freq == 524 && n[0].ms == 500); /* a quarter at 120 bpm: 500 ms; c5 (c4 doubled) */
    CHECK(n[1].freq == 660 && n[1].ms == 375); /* a dotted eighth: 250 + 125 */
    CHECK(n[2].freq == 1660 && n[2].ms == 500); /* g#6 */
    CHECK(n[3].freq == 0);                      /* a rest */
    CHECK(n[4].freq == 440 && n[4].ms == 1000); /* a half note of a4 */
    CHECK(n[5].freq == 988);                    /* h is B */
}

TEST(defaults_builtins_and_errors) {
    pal_note_t n[64];
    CHECK_EQ(pal_rtttl_parse("x::c", n, 64, NULL, 0), 1); /* d=4, o=6, b=63 */
    CHECK(n[0].freq == 1048 && n[0].ms == 952);
    for (size_t i = 0; pal_music_builtin_at(i); i++)
        CHECK(pal_rtttl_parse(pal_music_builtin(pal_music_builtin_at(i)), n, 64, NULL, 0) > 0);
    CHECK(pal_music_builtin("nope") == NULL);
    CHECK_EQ(pal_rtttl_parse("no colon here", n, 64, NULL, 0), -1);
    CHECK_EQ(pal_rtttl_parse("x:d=4:z", n, 64, NULL, 0), -1);
    CHECK_EQ(pal_rtttl_parse("x:d=4,o=5,b=120:c,c,c,c", n, 2, NULL, 0), 2); /* no more than max */
}

int main(void) {
    RUN(notes_lengths_and_octaves);
    RUN(defaults_builtins_and_errors);
    return TEST_DONE();
}
