#include <string.h>

#include "internal.h"

#define REG_MAX 24
#define REG_NAME 24

static struct {
    nema_port_mutex_t *lock;
    struct {
        char name[REG_NAME];
        void *ptr;
    } e[REG_MAX];
} R;

static nema_obj_t g_reg_obj;
static int reg_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static int reg_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static uint32_t reg_poll(nema_obj_t *o, uint32_t e) { (void)o, (void)e; return 0; }
static nema_err_t reg_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t REG_OPS = {"registry", reg_read, reg_write, reg_poll, reg_ioctl, NULL};

void nema_registry_init(void) {
    memset(&R, 0, sizeof(R));
    R.lock = nema_port_mutex_create();
    nema_obj_init(&g_reg_obj, &REG_OPS);
}

static int find_locked(const char *name) {
    for (int i = 0; i < REG_MAX; i++)
        if (R.e[i].ptr && strncmp(R.e[i].name, name, REG_NAME) == 0) return i;
    return -1;
}

nema_err_t nema_registry_publish(const char *name, void *ptr) {
    if (!name || !ptr || strlen(name) >= REG_NAME) return NEMA_ERR_INVAL;
    nema_err_t r = NEMA_ERR_FULL;
    nema_port_mutex_lock(R.lock);
    if (find_locked(name) >= 0) {
        r = NEMA_ERR_EXIST;
    } else {
        for (int i = 0; i < REG_MAX; i++) {
            if (!R.e[i].ptr) {
                nema_strlcpy(R.e[i].name, name, REG_NAME);
                R.e[i].ptr = ptr;
                r = NEMA_OK;
                break;
            }
        }
    }
    nema_port_mutex_unlock(R.lock);
    if (r == NEMA_OK) nema_notify(&g_reg_obj);
    return r;
}

nema_err_t nema_registry_unpublish(const char *name) {
    nema_err_t r = NEMA_ERR_NOENT;
    nema_port_mutex_lock(R.lock);
    int i = find_locked(name);
    if (i >= 0) {
        R.e[i].ptr = NULL;
        R.e[i].name[0] = 0;
        r = NEMA_OK;
    }
    nema_port_mutex_unlock(R.lock);
    return r;
}

typedef struct {
    const char *name;
    void *found;
} get_ctx_t;

static bool pred_get(void *p) {
    get_ctx_t *c = (get_ctx_t *)p;
    nema_port_mutex_lock(R.lock);
    int i = find_locked(c->name);
    c->found = i >= 0 ? R.e[i].ptr : NULL;
    nema_port_mutex_unlock(R.lock);
    return c->found != NULL;
}

nema_err_t nema_registry_get(const char *name, void **out, int32_t timeout_ms) {
    get_ctx_t c = {name, NULL};
    nema_obj_t *o = &g_reg_obj;
    int w = nema_wait_on(&o, 1, pred_get, &c, timeout_ms);
    if (w == NEMA_OK) {
        *out = c.found;
        return NEMA_OK;
    }
    return w == NEMA_ERR_TIMEOUT ? NEMA_ERR_NOENT : w;
}
