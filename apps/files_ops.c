/* The incremental file operations of the Files app (see files_ops.h). The state machine keeps an explicit stack of directories
 * (FILES_OPS_DEPTH deep), so no recursion runs on the desktop's stack. */
#include <stdio.h>
#include <string.h>

#include "files_model.h"
#include "files_ops.h"
#include "nema/nema.h"
#include "pal/sysinfo.h"

#define DEPTH 12
#define CHUNK 4096

typedef struct {
    char src[NEMA_PATH_MAX], dst[NEMA_PATH_MAX];
    size_t idx; /* readdir index for a copy (a delete always reads entry 0) */
} frame_t;

enum { ST_IDLE, ST_WALK, ST_FILE, ST_DONE };

struct files_job {
    files_op_t kind;     /* the operation the user asked for (a move may copy and then delete) */
    int state;
    bool moving;         /* a copy belonging to a move: the source is deleted at the end */
    bool deleting;       /* the delete phase of a move */
    frame_t *stack;
    int depth;
    uint8_t *buf;        /* CHUNK bytes */
    char cur_src[NEMA_PATH_MAX], cur_dst[NEMA_PATH_MAX]; /* the file being copied */
    nema_obj_t *in, *out;
    nema_err_t err;
    char err_path[NEMA_PATH_MAX];
    uint64_t bytes;
    int files, dirs;
    char top_src[NEMA_PATH_MAX], top_dst[NEMA_PATH_MAX];
};

files_job_t *files_job_new(void) {
    files_job_t *j = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*j));
    if (!j) return NULL;
    memset(j, 0, sizeof(*j));
    j->stack = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, DEPTH * sizeof(frame_t));
    j->buf = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, CHUNK);
    if (!j->stack || !j->buf) {
        files_job_free(j);
        return NULL;
    }
    return j;
}

void files_job_free(files_job_t *j) {
    if (!j) return;
    if (j->in) nema_obj_unref(j->in);
    if (j->out) nema_obj_unref(j->out);
    nema_free(j->stack);
    nema_free(j->buf);
    nema_free(j);
}

bool files_job_busy(const files_job_t *j) { return j && j->state != ST_IDLE && j->state != ST_DONE; }
files_op_t files_job_kind(const files_job_t *j) { return j ? j->kind : FILES_OP_NONE; }
nema_err_t files_job_result(const files_job_t *j) { return j ? j->err : NEMA_ERR_INVAL; }
const char *files_job_error_path(const files_job_t *j) { return j ? j->err_path : ""; }

static void close_files(files_job_t *j) {
    if (j->in) nema_obj_unref(j->in);
    if (j->out) nema_obj_unref(j->out);
    j->in = j->out = NULL;
}

static void fail(files_job_t *j, nema_err_t e, const char *path) {
    close_files(j);
    j->err = e;
    snprintf(j->err_path, sizeof(j->err_path), "%s", path ? path : "");
    j->state = ST_DONE;
}

static bool push(files_job_t *j, const char *src, const char *dst) {
    if (j->depth >= DEPTH) return false;
    frame_t *f = &j->stack[j->depth++];
    snprintf(f->src, sizeof(f->src), "%s", src);
    snprintf(f->dst, sizeof(f->dst), "%s", dst);
    f->idx = 0;
    return true;
}

static void reset(files_job_t *j) {
    close_files(j);
    j->depth = 0;
    j->bytes = 0;
    j->files = j->dirs = 0;
    j->err = NEMA_OK;
    j->err_path[0] = 0;
    j->moving = j->deleting = false;
}

/* A copy's source may not contain its target, and the two may not be the same path. */
static bool into_itself(const char *src, const char *dst) {
    return strcmp(src, dst) == 0 || (files_under(dst, src) && strcmp(src, "/") != 0);
}

nema_err_t files_job_copy(files_job_t *j, const char *src, const char *dst, bool move) {
    if (!j || files_job_busy(j)) return NEMA_ERR_BUSY;
    reset(j);
    j->kind = move ? FILES_OP_MOVE : FILES_OP_COPY;
    snprintf(j->top_src, sizeof(j->top_src), "%s", src);
    snprintf(j->top_dst, sizeof(j->top_dst), "%s", dst);
    if (into_itself(src, dst)) return NEMA_ERR_INVAL;
    nema_stat_t st;
    nema_err_t r = nema_fs_stat(src, &st);
    if (r != NEMA_OK) return r;
    if (move) {
        /* A rename is the cheap way when the target is free and both ends are on one filesystem. */
        nema_stat_t dt;
        if (nema_fs_stat(dst, &dt) != NEMA_OK) {
            r = nema_fs_rename(src, dst);
            if (r == NEMA_OK) {
                j->state = ST_DONE;
                return NEMA_OK;
            }
        }
        j->moving = true;
    }
    if (st.type == NEMA_T_DIR) {
        nema_stat_t dt;
        if (nema_fs_stat(dst, &dt) == NEMA_OK) {
            if (dt.type != NEMA_T_DIR) return NEMA_ERR_NOTDIR;
        } else {
            r = nema_fs_mkdir(dst);
            if (r != NEMA_OK) return r;
        }
        push(j, src, dst);
        j->dirs++;
        j->state = ST_WALK;
    } else {
        snprintf(j->cur_src, sizeof(j->cur_src), "%s", src);
        snprintf(j->cur_dst, sizeof(j->cur_dst), "%s", dst);
        j->state = ST_FILE;
    }
    return NEMA_OK;
}

nema_err_t files_job_delete(files_job_t *j, const char *path) {
    if (!j || files_job_busy(j)) return NEMA_ERR_BUSY;
    reset(j);
    j->kind = FILES_OP_DELETE;
    j->deleting = true;
    snprintf(j->top_src, sizeof(j->top_src), "%s", path);
    nema_stat_t st;
    nema_err_t r = nema_fs_stat(path, &st);
    if (r != NEMA_OK) return r;
    if (st.type == NEMA_T_DIR) {
        push(j, path, path);
    } else {
        /* a single file: files < 0 marks it, and step_delete unlinks it */
        snprintf(j->cur_src, sizeof(j->cur_src), "%s", path);
        j->files = -1;
    }
    j->state = ST_WALK;
    return NEMA_OK;
}

/* One copy step: a chunk of the open file, or the opening of the next file in the walk. */
static void step_file(files_job_t *j) {
    if (!j->in) {
        nema_err_t r = nema_fs_open(j->cur_src, NEMA_O_READ, &j->in);
        if (r != NEMA_OK) return fail(j, r, j->cur_src);
        r = nema_fs_open(j->cur_dst, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &j->out);
        if (r != NEMA_OK) return fail(j, r, j->cur_dst);
    }
    int n = nema_obj_read(j->in, j->buf, CHUNK, -1);
    if (n < 0) return fail(j, (nema_err_t)n, j->cur_src);
    if (n == 0) {
        close_files(j);
        j->files++;
        j->state = j->depth > 0 ? ST_WALK : ST_DONE; /* a single file has no directory to walk */
        return;
    }
    int w = nema_obj_write(j->out, j->buf, (size_t)n, -1);
    if (w != n) return fail(j, w < 0 ? (nema_err_t)w : NEMA_ERR_NOSPC, j->cur_dst);
    j->bytes += (uint64_t)n;
}

/* One walk step: the next entry of the top directory of a copy, or the pop of a finished one. */
static void step_copy_walk(files_job_t *j) {
    frame_t *f = &j->stack[j->depth - 1];
    nema_dirent_t d;
    nema_err_t r = nema_fs_readdir(f->src, f->idx, &d);
    if (r == NEMA_ERR_NOENT) {
        j->depth--;
        if (j->depth == 0) j->state = ST_DONE;
        return;
    }
    if (r != NEMA_OK) return fail(j, r, f->src);
    f->idx++;
    char cs[NEMA_PATH_MAX], cd[NEMA_PATH_MAX];
    files_join(cs, sizeof(cs), f->src, d.name);
    files_join(cd, sizeof(cd), f->dst, d.name);
    if (d.type == NEMA_T_DIR) {
        nema_stat_t st;
        if (nema_fs_stat(cd, &st) == NEMA_OK) {
            if (st.type != NEMA_T_DIR) return fail(j, NEMA_ERR_NOTDIR, cd);
        } else {
            r = nema_fs_mkdir(cd);
            if (r != NEMA_OK) return fail(j, r, cd);
        }
        j->dirs++;
        if (!push(j, cs, cd)) return fail(j, NEMA_ERR_RANGE, cs);
    } else {
        nema_stat_t st;
        if (nema_fs_stat(cd, &st) == NEMA_OK && st.type == NEMA_T_DIR) return fail(j, NEMA_ERR_ISDIR, cd);
        snprintf(j->cur_src, sizeof(j->cur_src), "%s", cs);
        snprintf(j->cur_dst, sizeof(j->cur_dst), "%s", cd);
        j->state = ST_FILE;
    }
}

/* One delete step: the first entry of the top directory is removed (a folder is pushed and handled the same way), and an empty
 * directory goes when nothing is left in it. Always reading entry 0 keeps the index valid while entries disappear. */
static void step_delete(files_job_t *j) {
    if (j->files < 0) { /* a single file */
        nema_err_t r = nema_fs_unlink(j->cur_src);
        if (r != NEMA_OK) return fail(j, r, j->cur_src);
        j->files = 1;
        j->state = ST_DONE;
        return;
    }
    frame_t *f = &j->stack[j->depth - 1];
    nema_dirent_t d;
    nema_err_t r = nema_fs_readdir(f->src, 0, &d);
    if (r == NEMA_ERR_NOENT) {
        r = nema_fs_unlink(f->src);
        if (r != NEMA_OK) return fail(j, r, f->src);
        j->dirs++;
        j->depth--;
        if (j->depth == 0) j->state = ST_DONE;
        return;
    }
    if (r != NEMA_OK) return fail(j, r, f->src);
    char child[NEMA_PATH_MAX];
    files_join(child, sizeof(child), f->src, d.name);
    if (d.type == NEMA_T_DIR) {
        if (!push(j, child, child)) return fail(j, NEMA_ERR_RANGE, child);
    } else {
        r = nema_fs_unlink(child);
        if (r != NEMA_OK) return fail(j, r, child);
        j->files++;
    }
}

bool files_job_step(files_job_t *j, uint32_t budget_us) {
    if (!files_job_busy(j)) return true;
    uint64_t end = pal_sys_uptime_us() + budget_us;
    do {
        if (j->state == ST_FILE) step_file(j);
        else if (j->state == ST_WALK) {
            if (j->deleting || j->kind == FILES_OP_DELETE) step_delete(j);
            else step_copy_walk(j);
        }
        if (j->state == ST_DONE) break;
    } while (pal_sys_uptime_us() < end);
    if (j->state == ST_DONE && j->moving && !j->deleting && j->err == NEMA_OK) {
        /* the copy of a move is complete: the source goes now */
        j->deleting = true;
        j->moving = false;
        nema_stat_t st;
        if (nema_fs_stat(j->top_src, &st) == NEMA_OK) {
            j->depth = 0;
            if (st.type == NEMA_T_DIR) {
                push(j, j->top_src, j->top_src);
            } else {
                nema_err_t r = nema_fs_unlink(j->top_src);
                if (r != NEMA_OK) fail(j, r, j->top_src);
                j->state = ST_DONE;
                return true;
            }
            j->state = ST_WALK;
            return false;
        }
    }
    return j->state == ST_DONE;
}

void files_job_status(const files_job_t *j, char *out, size_t cap) {
    char size[16];
    files_size_str(size, sizeof(size), j ? j->bytes : 0);
    int files = j && j->files > 0 ? j->files : 0;
    if (!j || j->kind == FILES_OP_NONE) snprintf(out, cap, "Working");
    else if (j->kind == FILES_OP_DELETE) snprintf(out, cap, "Deleting %d items", files + (j->dirs > 0 ? j->dirs : 0));
    else if (j->deleting) snprintf(out, cap, "Removing %d items", files + j->dirs);
    else snprintf(out, cap, "%s %d files, %s", j->kind == FILES_OP_MOVE ? "Moving" : "Copying", files, size);
}

/* Counts the entries under path with an explicit stack; stops at max. */
static void count_rec(const char *path, int max, int *count) {
    /* on the heap: the walk's stack of paths is ~1.7 KB, and this runs on the desktop task (6 KB) under the file system */
    struct frame {
        char p[NEMA_PATH_MAX];
        size_t idx;
    } *st;
    int depth = 0;
    nema_stat_t s;
    if (nema_fs_stat(path, &s) != NEMA_OK) return;
    *count = 1;
    if (s.type != NEMA_T_DIR) return;
    st = (struct frame *)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*st) * DEPTH);
    if (!st) return;
    snprintf(st[0].p, sizeof(st[0].p), "%s", path);
    st[0].idx = 0;
    depth = 1;
    while (depth > 0 && *count < max) {
        nema_dirent_t d;
        if (nema_fs_readdir(st[depth - 1].p, st[depth - 1].idx, &d) != NEMA_OK) {
            depth--;
            continue;
        }
        st[depth - 1].idx++;
        (*count)++;
        if (d.type == NEMA_T_DIR && depth < DEPTH) {
            files_join(st[depth].p, sizeof(st[depth].p), st[depth - 1].p, d.name);
            st[depth].idx = 0;
            depth++;
        }
    }
    nema_free(st);
}

int files_count_tree(const char *path, int max, bool *more) {
    int count = 0;
    count_rec(path, max, &count);
    if (more) *more = count >= max;
    return count;
}
