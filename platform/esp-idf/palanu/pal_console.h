#ifndef PAL_CONSOLE_H
#define PAL_CONSOLE_H

/* Attach a shell to the debug UART. Call after pal_boot(). */
void pal_console_start(void);

void pal_ota_esp_start(void); /* firmware update ops, and the self-check that confirms a new image */

#endif
