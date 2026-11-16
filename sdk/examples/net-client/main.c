/* net-client HOST PORT [TEXT]: connects over TCP, sends TEXT and a newline (default "hello"), and prints the first
 * line the server sends back. Needs the "net" permission (app.toml). Example server on a Mac:
 *   nc -l 9000        (then: net-client 192.168.1.10 9000 "ping")
 * Build: bun forge/tools/palanu.ts build   (in this directory; writes net-client.papp) */
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"

/* Copies argument i into buf. Returns false when it is missing or too long. */
static int arg_text(int32_t i, char *buf, uint32_t cap) {
    int32_t n = pal_argv(i, buf, cap);
    return n >= 0 && (uint32_t)n < cap;
}

void _start(void) {
    char host[64], port_text[16], text[128], line[256], reply[256];
    if (pal_argc() < 3 || !arg_text(1, host, sizeof host) || !arg_text(2, port_text, sizeof port_text)) {
        printf("usage: net-client HOST PORT [TEXT]\n");
        pal_exit(2);
    }
    if (!arg_text(3, text, sizeof text)) strcpy(text, "hello");
    long port = strtol(port_text, NULL, 10);
    if (port <= 0 || port > 65535) {
        printf("net-client: bad port %s\n", port_text);
        pal_exit(2);
    }
    int32_t fd = pal_tcp_connect(host, (int32_t)port, 5000);
    if (fd < 0) {
        printf("net-client: cannot connect to %s:%ld (error %ld)\n", host, port, (long)fd);
        pal_exit(1);
    }
    int n = snprintf(line, sizeof line, "%s\n", text);
    pal_write(fd, line, (uint32_t)n);
    /* read until a newline arrives, the peer closes, or 5 s pass without data */
    size_t got = 0;
    reply[0] = 0;
    while (got < sizeof reply - 1 && !strchr(reply, '\n')) {
        struct { int32_t h; uint16_t want, got; } rec = {fd, PAL_POLL_IN, 0};
        if (pal_wait(&rec, sizeof rec, 5000) <= 0) break;
        int32_t k = pal_read(fd, reply + got, (uint32_t)(sizeof reply - 1 - got));
        if (k <= 0) break;
        got += (size_t)k;
        reply[got] = 0;
    }
    pal_close(fd);
    char *nl = strchr(reply, '\n');
    if (nl) *nl = 0;
    printf("reply from %s:%ld: %s\n", host, port, got ? reply : "(nothing)");
}
