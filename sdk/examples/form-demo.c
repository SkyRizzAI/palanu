/* A form app (API level 6, api/palanu/ui.h): the fields are data, and the running desktop session lays them out in its own look.
 * Build: bun forge/tools/palanu.ts build sdk/examples/form-demo.c --id form-demo --name "Form demo" --kind gui --api 6 -o form-demo.papp */
#include "../palanu.h"

enum { F_LAMP = 1, F_MODE, F_LEVEL, F_NAME, F_COUNT, F_ADD, F_QUIT };

static void show_count(int n) {
    char t[16];
    int i = 15;
    t[i] = 0;
    do t[--i] = (char)('0' + n % 10), n /= 10; while (n && i > 0);
    pal_form_set(F_COUNT, t + i);
}

void _start(void) {
    if (pal_form_open("Form demo") != 0) pal_exit(1);
    pal_form_add(100, PAL_FORM_SECTION, "Lamp", "");
    pal_form_add(F_LAMP, PAL_FORM_TOGGLE, "Lamp on", "1");
    pal_form_add(F_MODE, PAL_FORM_CHOICE, "Mode", "Warm|Cool|Daylight");
    pal_form_add(F_LEVEL, PAL_FORM_SLIDER, "Level", "0|100|10|60");
    pal_form_add(F_NAME, PAL_FORM_TEXT, "Name", "Desk lamp");
    pal_form_add(101, PAL_FORM_SECTION, "Counter", "");
    pal_form_add(F_COUNT, PAL_FORM_INFO, "Presses", "0");
    pal_form_add(F_ADD, PAL_FORM_ACTION, "Press me", "");
    pal_form_add(F_QUIT, PAL_FORM_ACTION, "Quit", "");
    int presses = 0;
    for (;;) {
        int32_t ev[3];
        int r = pal_form_next(ev, sizeof(ev), -1);
        if (r != 1) continue;
        if (ev[1] == PAL_FORM_CLOSED) break; /* the person left: this demo ends */
        if (ev[1] == PAL_FORM_ACTIVATED && ev[0] == F_ADD) show_count(++presses);
        if (ev[1] == PAL_FORM_ACTIVATED && ev[0] == F_QUIT) break;
    }
    pal_form_close();
}
