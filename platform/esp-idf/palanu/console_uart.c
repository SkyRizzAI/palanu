/* A shell session on the debug UART: a TTY whose terminal side is the serial port.
 * The process side is the same shell as on the LCD, which is the point of the TTY design.
 *
 * Nothing here polls. Three tasks, all blocked when idle:
 *   uart_evt_task  drains the UART driver's event queue and notifies uart_obj (a nema object that is readable while
 *                  the UART RX buffer holds bytes);
 *   console_task   waits with nema_wait_on() for uart_obj or the TTY's terminal side, then moves bytes both ways;
 *   shell_task     waits for the shell to exit (nema_proc_wait) and starts another. */
#include <stdio.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nema/nema.h"
#include "pal/link.h"
#include "pal_console.h"

#define UART_EVT_QUEUE_LEN 16

static nema_tty_t *s_tty;
static nema_pid_t s_pid;
static nema_owner_t s_owner;
static pal_link_t *s_link;
static QueueHandle_t s_uart_q;

/* ---- the UART as a waitable object: readable while the RX buffer holds bytes ---- */
static int uart_obj_read(nema_obj_t *o, void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_NOSYS; /* only polled; reads go through uart_read_bytes() */
}
static int uart_obj_write(nema_obj_t *o, const void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_NOSYS;
}
static uint32_t uart_obj_poll(nema_obj_t *o, uint32_t ev) {
    size_t len = 0;
    (void)o, (void)ev;
    if (uart_get_buffered_data_len(UART_NUM_0, &len) == ESP_OK && len > 0) return NEMA_POLL_IN;
    return 0;
}
static nema_err_t uart_obj_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    (void)o, (void)req, (void)arg;
    return NEMA_ERR_NOSYS;
}
static const nema_obj_ops_t UART_OBJ_OPS = {"uart", uart_obj_read, uart_obj_write, uart_obj_poll, uart_obj_ioctl, NULL};
static nema_obj_t s_uart_obj;

static int link_tx(void *ctx, const uint8_t *d, size_t n) {
    return uart_write_bytes(UART_NUM_0, d, n) == (int)n ? 0 : -1;
}
static void link_pass(void *ctx, const uint8_t *d, size_t n) { nema_tty_input(s_tty, d, n); }

static void start_shell(void) {
    nema_obj_t *io = nema_tty_proc_obj(s_tty);
    nema_spawn_t sp = {.name = "sh", .stdio = {io, io, io}, .cwd = "/home"};
    if (nema_proc_spawn(&sp, &s_pid) == NEMA_OK) nema_tty_set_foreground(s_tty, s_pid);
}

static bool console_ready(void *ctx) {
    (void)ctx;
    return (nema_obj_poll(nema_tty_term_obj(s_tty), NEMA_POLL_IN) & NEMA_POLL_IN) || uart_obj_poll(&s_uart_obj, 0);
}

static void uart_evt_task(void *arg) {
    (void)arg;
    uart_event_t ev;
    for (;;) {
        if (xQueueReceive(s_uart_q, &ev, portMAX_DELAY) == pdTRUE) nema_notify(&s_uart_obj);
    }
}

static void console_task(void *arg) {
    char buf[128];
    nema_obj_t *objs[2] = {&s_uart_obj, nema_tty_term_obj(s_tty)};
    for (;;) {
        if (nema_wait_on(objs, 2, console_ready, NULL, -1) != NEMA_OK) {
            nema_sleep_ms(10); /* not expected for a system task; never spin on it */
            continue;
        }
        int n = nema_tty_output_read(s_tty, buf, sizeof(buf), 0);
        if (n > 0) {
            fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
        }
        int r = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), 0);
        if (r > 0) pal_link_feed(s_link, (const uint8_t *)buf, (size_t)r);
    }
}

static void shell_task(void *arg) {
    (void)arg;
    for (;;) {
        if (s_pid == 0) { /* the last spawn failed: try again later */
            nema_sleep_ms(1000);
            start_shell();
            continue;
        }
        int code;
        nema_proc_wait(s_pid, &code, -1); /* blocks until the shell exits, then reaps it */
        s_pid = 0;
        start_shell();
    }
}

void pal_console_start(void) {
    s_owner = nema_owner_register("console");
    uart_driver_install(UART_NUM_0, 2048, 0, UART_EVT_QUEUE_LEN, &s_uart_q, 0);
    nema_obj_init(&s_uart_obj, &UART_OBJ_OPS);
    pal_link_io_t io = {.tx = link_tx, .passthrough = link_pass, .transport = PAL_LINK_UART};
    s_link = pal_link_create(&io);
    if (!s_link) return;
    if (nema_tty_create(&s_tty, 80, 24, s_owner) != NEMA_OK) return;
    nema_log_set_console(false); /* the shell owns the console now; `log` shows the ring */
    start_shell();
    if (s_uart_q) {
        nema_task_desc_t ev = {.name = "uart_evt", .fn = uart_evt_task, .stack = 2048, .prio = 5, .core = -1,
                               .role = NEMA_TASK_SYSTEM, .owner = s_owner};
        nema_task_create(&ev);
    }
    nema_task_desc_t td = {.name = "console", .fn = console_task, .stack = 8192, .prio = 4, .core = -1,
                           .role = NEMA_TASK_SYSTEM, .owner = s_owner};
    nema_task_create(&td);
    nema_task_desc_t sh = {.name = "shell", .fn = shell_task, .stack = 3072, .prio = 3, .core = -1,
                           .role = NEMA_TASK_SYSTEM, .owner = s_owner};
    nema_task_create(&sh);
}
