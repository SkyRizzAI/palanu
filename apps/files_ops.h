/* File operations for the Files app, run a slice at a time: copy, move and recursive delete advance by a time budget per call,
 * so a big copy never stalls the desktop (the view calls files_job_step from its pump while a busy overlay is shown). One job at
 * a time. Tests: tests/unit/test_files.c, on a RAM filesystem.
 *
 * Rules: copying a folder merges it into an existing folder and overwrites files with the same name (the view asks first). A move
 * is a rename when the target does not exist and the same filesystem holds both; otherwise it is a copy followed by a delete of
 * the source. Nothing refuses a path here; the view applies the policy (installed apps, mount points). */
#ifndef FILES_OPS_H
#define FILES_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/vfs.h"

typedef enum { FILES_OP_NONE = 0, FILES_OP_COPY, FILES_OP_MOVE, FILES_OP_DELETE } files_op_t;

typedef struct files_job files_job_t;

/* The job, allocated once (NULL when memory is short). Keep it for the life of the app. */
files_job_t *files_job_new(void);
void files_job_free(files_job_t *j);
bool files_job_busy(const files_job_t *j);

/* Start an operation. dst is the target for copy and move. Errors that show before any work (no source, a copy into itself)
 * come back here; the rest arrive through files_job_step. */
nema_err_t files_job_copy(files_job_t *j, const char *src, const char *dst, bool move);
nema_err_t files_job_delete(files_job_t *j, const char *path);

/* Advances the job for about budget_us microseconds (at least one unit of work). Returns true when the job has finished. */
bool files_job_step(files_job_t *j, uint32_t budget_us);
/* After finishing: the result (NEMA_OK when the whole job worked) and what it did. */
nema_err_t files_job_result(const files_job_t *j);
const char *files_job_error_path(const files_job_t *j); /* the path where it failed */
/* One line for the busy overlay: "Copying 12 files, 340K". */
void files_job_status(const files_job_t *j, char *out, size_t cap);
files_op_t files_job_kind(const files_job_t *j);

/* Counts the entries under path (path itself included), stopping at max. Returns the count; *more is true when max was reached. */
int files_count_tree(const char *path, int max, bool *more);

#endif
