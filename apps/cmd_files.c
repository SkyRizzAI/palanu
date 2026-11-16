#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libapp.h"

/* File commands. Each is a small independent app: nothing here is special to the shell. */

static int cmd_echo(int argc, char **argv) {
    bool nl = true;
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        nl = false;
        i = 2;
    }
    for (; i < argc; i++) {
        pal_puts(argv[i]);
        if (i + 1 < argc) pal_puts(" ");
    }
    if (nl) pal_puts("\n");
    return 0;
}

static int copy_fd(int in, int out) {
    char buf[256];
    int n;
    while ((n = nema_read(in, buf, sizeof(buf))) > 0)
        if (nema_write(out, buf, (size_t)n) < 0) return 1;
    return n < 0 ? 1 : 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc == 1) return copy_fd(0, 1);
    int st = 0;
    for (int i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") == 0 ? 0 : nema_open(argv[i], NEMA_O_READ);
        if (fd < 0) {
            pal_eprintf("cat: %s: %s\n", argv[i], pal_strerror(fd));
            st = 1;
            continue;
        }
        if (copy_fd(fd, 1)) st = 1;
        if (fd != 0) nema_close(fd);
    }
    return st;
}

typedef struct {
    char name[NEMA_NAME_MAX];
    uint8_t type;
    uint32_t size;
} ent_t;

static int cmp_ent(const void *a, const void *b) { return strcmp(((const ent_t *)a)->name, ((const ent_t *)b)->name); }

static int ls_one(const char *path, bool all, bool lng, bool color, int cols) {
    nema_stat_t st;
    nema_err_t r = nema_fs_stat(path, &st);
    if (r != NEMA_OK) {
        pal_eprintf("ls: %s: %s\n", path, pal_strerror(r));
        return 1;
    }
    if (st.type == NEMA_T_FILE) {
        pal_printf("%s\n", path);
        return 0;
    }
    nema_proc_t *self = nema_self();
    size_t cap = 32, n = 0;
    ent_t *e = (ent_t *)nema_alloc(NEMA_MEM_ANY, self->owner, cap * sizeof(ent_t));
    if (!e) return 1;
    nema_dirent_t d;
    for (size_t i = 0; nema_fs_readdir(path, i, &d) == NEMA_OK; i++) {
        if (!all && d.name[0] == '.') continue;
        if (n == cap) {
            ent_t *ne = (ent_t *)nema_alloc(NEMA_MEM_ANY, self->owner, cap * 2 * sizeof(ent_t));
            if (!ne) break;
            memcpy(ne, e, n * sizeof(ent_t));
            nema_free(e);
            e = ne;
            cap *= 2;
        }
        memcpy(e[n].name, d.name, sizeof(d.name));
        e[n].type = d.type;
        e[n].size = d.size;
        n++;
    }
    qsort(e, n, sizeof(ent_t), cmp_ent);
    int width = 0;
    for (size_t i = 0; i < n; i++) {
        int l = (int)strlen(e[i].name) + 1;
        if (l > width) width = l;
    }
    width += 2;
    int per = lng ? 1 : (cols / width > 0 ? cols / width : 1);
    for (size_t i = 0; i < n; i++) {
        const char *c1 = "", *c2 = "";
        if (color && e[i].type == NEMA_T_DIR) { c1 = "\x1b[1;34m"; c2 = "\x1b[0m"; }
        char nm[NEMA_NAME_MAX + 2];
        snprintf(nm, sizeof(nm), "%s%s", e[i].name, e[i].type == NEMA_T_DIR ? "/" : "");
        if (lng) pal_printf("%c %8lu  %s%s%s\n", e[i].type == NEMA_T_DIR ? 'd' : '-', (unsigned long)e[i].size, c1, nm, c2);
        else {
            int pad = width - (int)strlen(nm);
            pal_printf("%s%s%s%*s", c1, nm, c2, ((i + 1) % (size_t)per == 0 || i + 1 == n) ? 0 : pad, "");
            if ((i + 1) % (size_t)per == 0 || i + 1 == n) pal_puts("\n");
        }
    }
    nema_free(e);
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    bool all = false, lng = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *f = argv[first] + 1; *f; f++) {
            if (*f == 'a') all = true;
            else if (*f == 'l') lng = true;
        }
    }
    int cols, rows;
    pal_term_size(&cols, &rows);
    bool color = pal_isatty(1);
    if (first >= argc) return ls_one(".", all, lng, color, cols);
    int st = 0;
    for (int i = first; i < argc; i++) {
        if (argc - first > 1) pal_printf("%s:\n", argv[i]);
        st |= ls_one(argv[i], all, lng, color, cols);
    }
    return st;
}

static int cmd_mkdir(int argc, char **argv) {
    int st = 0;
    for (int i = 1; i < argc; i++) {
        nema_err_t r = nema_fs_mkdir(argv[i]);
        if (r != NEMA_OK) {
            pal_eprintf("mkdir: %s: %s\n", argv[i], pal_strerror(r));
            st = 1;
        }
    }
    return st;
}

static int cmd_touch(int argc, char **argv) {
    int st = 0;
    for (int i = 1; i < argc; i++) {
        nema_obj_t *o;
        nema_err_t r = nema_fs_open(argv[i], NEMA_O_WRITE | NEMA_O_CREATE, &o);
        if (r != NEMA_OK) {
            pal_eprintf("touch: %s: %s\n", argv[i], pal_strerror(r));
            st = 1;
        } else {
            nema_obj_unref(o);
        }
    }
    return st;
}

static int rm_path(const char *path, bool recursive, int depth) {
    nema_stat_t st;
    nema_err_t r = nema_fs_stat(path, &st);
    if (r != NEMA_OK) {
        pal_eprintf("rm: %s: %s\n", path, pal_strerror(r));
        return 1;
    }
    if (st.type == NEMA_T_DIR) {
        if (!recursive) {
            pal_eprintf("rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        if (depth > 6) return 1;
        nema_dirent_t d;
        int rc = 0;
        while (nema_fs_readdir(path, 0, &d) == NEMA_OK) {
            char child[NEMA_PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, d.name);
            if (rm_path(child, true, depth + 1)) { rc = 1; break; }
        }
        if (rc) return rc;
    }
    r = nema_fs_unlink(path);
    if (r != NEMA_OK) {
        pal_eprintf("rm: %s: %s\n", path, pal_strerror(r));
        return 1;
    }
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    bool rec = false;
    int i = 1, st = 0;
    for (; i < argc && argv[i][0] == '-'; i++)
        if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) rec = true;
    if (i >= argc) {
        pal_eprintf("usage: rm [-r] file...\n");
        return 1;
    }
    for (; i < argc; i++) st |= rm_path(argv[i], rec, 0);
    return st;
}

static int copy_file(const char *from, const char *to) {
    nema_obj_t *in, *out;
    nema_err_t r = nema_fs_open(from, NEMA_O_READ, &in);
    if (r != NEMA_OK) {
        pal_eprintf("cp: %s: %s\n", from, pal_strerror(r));
        return 1;
    }
    r = nema_fs_open(to, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &out);
    if (r != NEMA_OK) {
        pal_eprintf("cp: %s: %s\n", to, pal_strerror(r));
        nema_obj_unref(in);
        return 1;
    }
    char buf[256];
    int n, rc = 0;
    while ((n = nema_obj_read(in, buf, sizeof(buf), 0)) > 0) {
        if (nema_obj_write(out, buf, (size_t)n, 0) < 0) {
            pal_eprintf("cp: %s: write failed\n", to);
            rc = 1;
            break;
        }
    }
    nema_obj_unref(in);
    nema_obj_unref(out);
    return rc;
}

static void dest_path(const char *to, const char *from, char *out, size_t cap) {
    nema_stat_t st;
    if (nema_fs_stat(to, &st) == NEMA_OK && st.type == NEMA_T_DIR) {
        const char *base = strrchr(from, '/');
        snprintf(out, cap, "%s/%s", to, base ? base + 1 : from);
    } else {
        nema_strlcpy(out, to, cap);
    }
}

static int cmd_cp(int argc, char **argv) {
    if (argc < 3) {
        pal_eprintf("usage: cp from... to\n");
        return 1;
    }
    int st = 0;
    for (int i = 1; i < argc - 1; i++) {
        char dst[NEMA_PATH_MAX];
        dest_path(argv[argc - 1], argv[i], dst, sizeof(dst));
        st |= copy_file(argv[i], dst);
    }
    return st;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 3) {
        pal_eprintf("usage: mv from... to\n");
        return 1;
    }
    int st = 0;
    for (int i = 1; i < argc - 1; i++) {
        char dst[NEMA_PATH_MAX];
        dest_path(argv[argc - 1], argv[i], dst, sizeof(dst));
        nema_err_t r = nema_fs_rename(argv[i], dst);
        if (r == NEMA_ERR_UNSUPPORTED) { /* across filesystems: copy, then remove */
            if (copy_file(argv[i], dst) == 0) r = nema_fs_unlink(argv[i]);
            else r = NEMA_ERR_IO;
        }
        if (r != NEMA_OK) {
            pal_eprintf("mv: %s: %s\n", argv[i], pal_strerror(r));
            st = 1;
        }
    }
    return st;
}

static int cmd_head_tail(int argc, char **argv, bool tail) {
    int n = 10, i = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        n = atoi(argv[2]);
        i = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') { /* head -5 */
        n = atoi(argv[1] + 1);
        i = 2;
    }
    int fd = i < argc ? nema_open(argv[i], NEMA_O_READ) : 0;
    if (fd < 0) {
        pal_eprintf("%s: %s\n", argv[i], pal_strerror(fd));
        return 1;
    }
    size_t len;
    char *t = pal_read_all(fd, &len);
    if (fd) nema_close(fd);
    if (!t) return 1;
    if (!tail) {
        size_t p = 0;
        int lines = 0;
        while (p < len && lines < n) {
            if (t[p++] == '\n') lines++;
        }
        nema_write(1, t, p);
    } else {
        size_t p = len;
        int lines = 0;
        if (p && t[p - 1] == '\n') p--;
        while (p > 0 && lines < n) {
            if (t[--p] == '\n') lines++;
        }
        if (lines >= n) p++;
        nema_write(1, t + p, len - p);
    }
    nema_free(t);
    return 0;
}
static int cmd_head(int argc, char **argv) { return cmd_head_tail(argc, argv, false); }
static int cmd_tail(int argc, char **argv) { return cmd_head_tail(argc, argv, true); }

static int cmd_wc(int argc, char **argv) {
    int fd = argc > 1 ? nema_open(argv[1], NEMA_O_READ) : 0;
    if (fd < 0) {
        pal_eprintf("wc: %s: %s\n", argv[1], pal_strerror(fd));
        return 1;
    }
    size_t len;
    char *t = pal_read_all(fd, &len);
    if (fd) nema_close(fd);
    if (!t) return 1;
    size_t lines = 0, words = 0;
    bool in = false;
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '\n') lines++;
        bool sp = t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r';
        if (!sp && !in) words++;
        in = !sp;
    }
    pal_printf("%7lu %7lu %7lu %s\n", (unsigned long)lines, (unsigned long)words, (unsigned long)len, argc > 1 ? argv[1] : "");
    nema_free(t);
    return 0;
}

static bool contains_ci(const char *h, const char *n, bool ci) {
    size_t nl = strlen(n);
    if (!nl) return true;
    for (; *h; h++) {
        size_t i = 0;
        for (; i < nl && h[i]; i++) {
            char a = h[i], b = n[i];
            if (ci) {
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            }
            if (a != b) break;
        }
        if (i == nl) return true;
    }
    return false;
}

static int cmd_grep(int argc, char **argv) {
    bool ci = false, num = false, inv = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'i') ci = true;
            else if (*f == 'n') num = true;
            else if (*f == 'v') inv = true;
        }
    }
    if (i >= argc) {
        pal_eprintf("usage: grep [-inv] pattern [file]\n");
        return 2;
    }
    const char *pat = argv[i++];
    int fd = i < argc ? nema_open(argv[i], NEMA_O_READ) : 0;
    if (fd < 0) {
        pal_eprintf("grep: %s: %s\n", argv[i], pal_strerror(fd));
        return 2;
    }
    size_t len;
    char *t = pal_read_all(fd, &len);
    if (fd) nema_close(fd);
    if (!t) return 2;
    int found = 0, ln = 0;
    for (size_t p = 0; p < len;) {
        char *nl = memchr(t + p, '\n', len - p);
        size_t l = nl ? (size_t)(nl - (t + p)) : len - p;
        char line[256];
        size_t cl = l < sizeof(line) - 1 ? l : sizeof(line) - 1;
        memcpy(line, t + p, cl);
        line[cl] = 0;
        ln++;
        if (contains_ci(line, pat, ci) != inv) {
            found++;
            if (num) pal_printf("%d:", ln);
            pal_printf("%s\n", line);
        }
        p += l + 1;
    }
    nema_free(t);
    return found ? 0 : 1;
}

static int cmd_hexdump(int argc, char **argv) {
    int fd = argc > 1 ? nema_open(argv[1], NEMA_O_READ) : 0;
    if (fd < 0) {
        pal_eprintf("hexdump: %s: %s\n", argv[1], pal_strerror(fd));
        return 1;
    }
    unsigned char b[8];
    int n;
    unsigned off = 0;
    while ((n = nema_read(fd, b, sizeof(b))) > 0) {
        pal_printf("%04x ", off);
        for (int i = 0; i < 8; i++) {
            if (i < n) pal_printf("%02x ", b[i]);
            else pal_puts("   ");
        }
        for (int i = 0; i < n; i++) pal_printf("%c", b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        pal_puts("\n");
        off += (unsigned)n;
    }
    if (fd) nema_close(fd);
    return 0;
}

#define APP(n, h, f) {n, h, f, 0, 0}
static const nema_app_t FILE_APPS[] = {
    APP("echo", "print arguments", cmd_echo),     APP("cat", "show files", cmd_cat),
    APP("ls", "list directory (-a -l)", cmd_ls),  APP("mkdir", "make directories", cmd_mkdir),
    APP("touch", "create empty files", cmd_touch), APP("rm", "remove files (-r)", cmd_rm),
    APP("cp", "copy files", cmd_cp),              APP("mv", "move or rename", cmd_mv),
    APP("head", "first lines (-n N)", cmd_head),  APP("tail", "last lines (-n N)", cmd_tail),
    APP("wc", "count lines, words, bytes", cmd_wc), APP("grep", "find lines (-i -n -v)", cmd_grep),
    APP("hexdump", "hex view of a file", cmd_hexdump),
};

void pal_apps_register_files(void);
void pal_apps_register_files(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(FILE_APPS); i++) nema_app_register(&FILE_APPS[i]);
}
