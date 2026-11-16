#include "pal/ota.h"

static const pal_ota_ops_t *s_ops;
void pal_ota_set(const pal_ota_ops_t *ops) { s_ops = ops; }
const pal_ota_ops_t *pal_ota(void) { return s_ops; }
