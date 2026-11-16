/* hello-service: a service loop. Logs every second with pal_log and prints a line; a stop request (Ctrl-C, or
 * kill from the shell) arrives as a QUIT event, and the loop ends. Build: bun forge/tools/palanu.ts build */
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"

void _start(void) {
    int32_t events = pal_events();
    int ticks = 0;
    pal_log(PAL_LOG_INFO, "hello-service started");
    for (;;) {
        if (events < 0) { /* no event handle: plain sleeps, and the service ends after ten seconds */
            pal_sleep_ms(1000);
            if (++ticks >= 10) break;
            continue;
        }
        struct { int32_t h; uint16_t want, got; } rec = {events, PAL_POLL_IN, 0};
        int32_t n = pal_wait(&rec, sizeof rec, 1000);
        if (n < 0) break;
        if (n > 0) {
            uint32_t ev[4];
            while (pal_event_next(ev, sizeof ev, 0) == 16)
                if (ev[0] == PAL_EV_QUIT) {
                    printf("hello-service: stop asked after %d ticks, ending\n", ticks);
                    pal_log(PAL_LOG_INFO, "hello-service stopped");
                    return;
                }
            continue;
        }
        ticks++;
        printf("hello-service: tick %d\n", ticks);
        pal_log(PAL_LOG_INFO, "hello-service tick");
    }
}
