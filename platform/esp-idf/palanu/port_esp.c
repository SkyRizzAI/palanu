/* Nema port for ESP-IDF: FreeRTOS, heap_caps, esp_timer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nema/port.h"

#define TLS_INDEX 2 /* indexes 0 and 1 are used by IDF (pthread, newlib); see sdkconfig.defaults */

static uint32_t caps_of(nema_mem_class_t c) {
    switch (c) {
    case NEMA_MEM_DMA: return MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    case NEMA_MEM_EXTERNAL: return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    default: return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
}

void *nema_port_alloc(nema_mem_class_t cls, size_t size, size_t align) {
    if (align > 4) return heap_caps_aligned_alloc(align, size, caps_of(cls));
    return heap_caps_malloc(size, caps_of(cls));
}
void nema_port_free(nema_mem_class_t cls, void *p) {
    (void)cls;
    heap_caps_free(p);
}
size_t nema_port_mem_total(nema_mem_class_t cls) { return heap_caps_get_total_size(caps_of(cls)); }
size_t nema_port_mem_free(nema_mem_class_t cls) { return heap_caps_get_free_size(caps_of(cls)); }
size_t nema_port_mem_largest(nema_mem_class_t cls) { return heap_caps_get_largest_free_block(caps_of(cls)); }
size_t nema_port_mem_min_free(nema_mem_class_t cls) { return heap_caps_get_minimum_free_size(caps_of(cls)); }
void nema_port_mem_stats(int cls, size_t *free, size_t *min_free, size_t *largest) {
    uint32_t caps = caps_of((nema_mem_class_t)cls);
    if (free) *free = heap_caps_get_free_size(caps);
    if (min_free) *min_free = heap_caps_get_minimum_free_size(caps);
    if (largest) *largest = heap_caps_get_largest_free_block(caps);
}

uint64_t nema_port_time_us(void) { return (uint64_t)esp_timer_get_time(); }
/* The ESP32 TRNG. esp_fill_random is true random only while the RF subsystem runs (Wi-Fi or BLE); before that it is a
 * weaker source, so a key made at first boot should wait for the radio (noted in docs/API.md under keys). */
void nema_port_random(void *buf, size_t len) { esp_fill_random(buf, len); }

void nema_port_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }

struct nema_port_mutex {
    SemaphoreHandle_t h;
};
nema_port_mutex_t *nema_port_mutex_create(void) {
    nema_port_mutex_t *m = heap_caps_malloc(sizeof(*m), MALLOC_CAP_INTERNAL);
    if (!m) return NULL;
    m->h = xSemaphoreCreateMutex();
    if (!m->h) {
        heap_caps_free(m);
        return NULL;
    }
    return m;
}
void nema_port_mutex_lock(nema_port_mutex_t *m) { xSemaphoreTake(m->h, portMAX_DELAY); }
void nema_port_mutex_unlock(nema_port_mutex_t *m) { xSemaphoreGive(m->h); }
void nema_port_mutex_destroy(nema_port_mutex_t *m) {
    if (!m) return;
    vSemaphoreDelete(m->h);
    heap_caps_free(m);
}

struct nema_port_sem {
    SemaphoreHandle_t h;
};
nema_port_sem_t *nema_port_sem_create(uint32_t initial, uint32_t max) {
    nema_port_sem_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_INTERNAL);
    if (!s) return NULL;
    s->h = xSemaphoreCreateCounting(max, initial);
    if (!s->h) {
        heap_caps_free(s);
        return NULL;
    }
    return s;
}
nema_err_t nema_port_sem_take(nema_port_sem_t *s, int32_t timeout_ms) {
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && t == 0) t = 1;
    return xSemaphoreTake(s->h, t) == pdTRUE ? NEMA_OK : NEMA_ERR_TIMEOUT;
}
void nema_port_sem_give(nema_port_sem_t *s) { xSemaphoreGive(s->h); }
void nema_port_sem_destroy(nema_port_sem_t *s) {
    if (!s) return;
    vSemaphoreDelete(s->h);
    heap_caps_free(s);
}

nema_err_t nema_port_task_create(nema_port_task_t **out, const char *name, nema_port_task_fn fn, void *arg,
                                 size_t stack_bytes, int prio, int core) {
    TaskHandle_t h = NULL;
    BaseType_t r = xTaskCreatePinnedToCore(fn, name, (uint32_t)stack_bytes, arg, (UBaseType_t)(prio + 1), &h,
                                           core < 0 ? tskNO_AFFINITY : core);
    if (r != pdPASS) return NEMA_ERR_NOMEM;
    if (out) *out = (nema_port_task_t *)h;
    return NEMA_OK;
}
void nema_port_task_exit(void) {
    vTaskDelete(NULL);
    for (;;) {
    }
}
size_t nema_port_task_stack_free(void) { return uxTaskGetStackHighWaterMark(NULL); }

void *nema_port_tls_get(void) { return pvTaskGetThreadLocalStoragePointer(NULL, TLS_INDEX); }
void nema_port_tls_set(void *p) { vTaskSetThreadLocalStoragePointer(NULL, TLS_INDEX, p); }

/* CPU load per core = 100% minus the share of time the idle task ran since the last query. */
int nema_port_cpu_count(void) { return 2; }
bool nema_port_heap_check(void) { return heap_caps_check_integrity_all(true); }

int nema_port_cpu_percent(nema_cpu_sample_t *s, int core) {
    static TaskStatus_t tasks[48];
    if (!s || core < 0 || core > 1) return 0;
    UBaseType_t n = uxTaskGetSystemState(tasks, 48, NULL);
    const char *want = core == 0 ? "IDLE0" : "IDLE1";
    int64_t now = esp_timer_get_time();
    int pct = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strcmp(tasks[i].pcTaskName, want) != 0) continue;
        uint32_t idle = (uint32_t)tasks[i].ulRunTimeCounter;
        if (s->t[core]) {
            int64_t dt = now - s->t[core];
            pct = 100 - (int)((uint64_t)(uint32_t)(idle - s->idle[core]) * 100u / (uint64_t)(dt > 0 ? dt : 1));
        }
        s->idle[core] = idle;
        s->t[core] = now;
    }
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void nema_port_console_write(const char *buf, size_t n) {
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
}
void nema_port_abort(void) { abort(); }
