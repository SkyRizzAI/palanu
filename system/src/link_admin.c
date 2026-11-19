/* Palanu Link administration: the live links (so a revoke or a trust change reaches them), the connected status, the
 * pairing window, the paired clients, the device identifier, and the cable trust setting. The shell commands are in
 * apps/cmd_link.c. */
#include <string.h>

#include "link_auth.h"
#include "link_clients.h"
#include "link_int.h"
#include "nema/port.h"
#include "nema/types.h"

static nema_port_mutex_t *s_reg_mu;
static pal_link_t *s_live[LINK_MAX_LIVE];

static void reg_lock(void) {
    if (!s_reg_mu) s_reg_mu = nema_port_mutex_create();
    nema_port_mutex_lock(s_reg_mu);
}
static void reg_unlock(void) { nema_port_mutex_unlock(s_reg_mu); }

nema_err_t link_reg_add(pal_link_t *l) {
    nema_err_t r = NEMA_ERR_NOMEM;
    reg_lock();
    for (int i = 0; i < LINK_MAX_LIVE; i++) {
        if (!s_live[i]) {
            s_live[i] = l;
            r = NEMA_OK;
            break;
        }
    }
    reg_unlock();
    return r;
}

void link_reg_del(pal_link_t *l) {
    reg_lock();
    for (int i = 0; i < LINK_MAX_LIVE; i++)
        if (s_live[i] == l) s_live[i] = NULL;
    reg_unlock();
}

bool pal_link_connected(void) {
    bool c = false;
    reg_lock();
    for (int i = 0; i < LINK_MAX_LIVE; i++) {
        pal_link_t *l = s_live[i];
        if (l && l->hello && l->trusted && !l->kill) c = true;
    }
    reg_unlock();
    return c;
}

bool pal_link_uart_trusted(void) {
    char v[8] = "";
    return !(lc_config_get("link.uart.trust", v, sizeof(v)) >= 0 && strcmp(v, "off") == 0);
}

bool link_default_trust(pal_link_transport_t tr) {
    if (tr == PAL_LINK_UART) return pal_link_uart_trusted();
    if (tr == PAL_LINK_TCP || tr == PAL_LINK_WS) return link_tcp_open();
    return false; /* wireless links start untrusted: they authenticate first */
}

nema_err_t pal_link_uart_trust(bool on) {
    nema_err_t r = lc_config_set("link.uart.trust", on ? "on" : "off");
    if (r != NEMA_OK) return r;
    reg_lock();
    for (int i = 0; i < LINK_MAX_LIVE; i++) {
        pal_link_t *l = s_live[i];
        if (!l || l->tr != PAL_LINK_UART) continue;
        l->kill = !on;
        l->trusted = on;
    }
    reg_unlock();
    return NEMA_OK;
}

nema_err_t pal_link_revoke(const char *id) {
    uint8_t ids[LC_MAX][8];
    int n = 0, removed[LC_MAX] = {0};
    if (strcmp(id, "all") == 0) {
        n = lc_ids(ids, LC_MAX);
    } else {
        if (!lc_hex_dec(id, ids[0], 8)) return NEMA_ERR_INVAL;
        n = 1;
    }
    int gone = 0;
    for (int i = 0; i < n; i++)
        if (lc_remove(ids[i]) == NEMA_OK) {
            removed[i] = 1;
            gone++;
        }
    if (strcmp(id, "all") != 0 && !gone) return NEMA_ERR_NOENT;
    reg_lock();
    for (int i = 0; i < LINK_MAX_LIVE; i++) {
        pal_link_t *l = s_live[i];
        if (!l || !l->auth.authed) continue;
        for (int k = 0; k < n; k++) {
            if (removed[k] && memcmp(l->auth.client_id, ids[k], 8) == 0) {
                l->trusted = false; /* the next frame ends the session; a TCP connection is dropped */
                l->kill = true;
            }
        }
    }
    reg_unlock();
    return NEMA_OK;
}

nema_err_t pal_link_pair_open(char code[PAL_LINK_CODE_LEN + 1]) { return la_pair_open(code); }
void pal_link_pair_close(void) { la_pair_close(); }
size_t pal_link_pair_status(char *out, size_t cap) { return la_pair_status(out, cap); }
size_t pal_link_clients_text(char *out, size_t cap) { return lc_list_text(out, cap); }
int pal_link_clients_count(void) { return lc_count(); }

void pal_link_device_id(char out[17]) {
    if (lc_device_id(out) != NEMA_OK) nema_strlcpy(out, "0000000000000000", 17);
}
