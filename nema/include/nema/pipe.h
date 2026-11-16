/* Nema pipe: a one-way byte stream between two objects (read end, write end). */
#ifndef NEMA_PIPE_H
#define NEMA_PIPE_H

#include "nema/obj.h"

/* Both ends start with one reference each. Memory comes from `cls` and is charged to `owner`. */
nema_err_t nema_pipe_create(size_t capacity, nema_mem_class_t cls, nema_owner_t owner, nema_obj_t **rd,
                            nema_obj_t **wr);

#endif
