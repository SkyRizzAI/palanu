/* The POSIX filesystem (platform/posix_fs) through the VFS: a directory on the host, mounted at "/".
 * The checks go through nema_fs_* and also look at the host directory, so a passing run proves the files are
 * really there. The root is a fresh directory under $TMPDIR (or /tmp), removed at the end. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nema/nema.h"
#include "test.h"

static char ROOT[256];

static bool host_has(const char *rel) {
    char p[320];
    snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
    struct stat st;
    return stat(p, &st) == 0;
}

static nema_err_t write_file(const char *path, const char *text) {
    nema_obj_t *f;
    nema_err_t r = nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f);
    if (r != NEMA_OK) return r;
    int n = nema_obj_write(f, text, strlen(text), 0);
    nema_obj_unref(f);
    return n == (int)strlen(text) ? NEMA_OK : NEMA_ERR_IO;
}

static bool read_file(const char *path, char *out, size_t cap) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_READ, &f) != NEMA_OK) return false;
    int n = nema_obj_read(f, out, cap - 1, 0);
    nema_obj_unref(f);
    if (n < 0) return false;
    out[n] = 0;
    return true;
}

TEST(create_write_read_on_the_host) {
    CHECK(write_file("/hello.txt", "hello posix") == NEMA_OK);
    char b[64];
    CHECK(read_file("/hello.txt", b, sizeof(b)));
    CHECK(strcmp(b, "hello posix") == 0);
    CHECK(host_has("hello.txt"));
    nema_stat_t st;
    CHECK(nema_fs_stat("/hello.txt", &st) == NEMA_OK);
    CHECK_EQ(st.type, NEMA_T_FILE);
    CHECK_EQ(st.size, 11);
    CHECK(nema_fs_stat("/missing.txt", &st) == NEMA_ERR_NOENT);
    nema_obj_t *f;
    CHECK(nema_fs_open("/missing.txt", NEMA_O_READ, &f) == NEMA_ERR_NOENT);
}

TEST(append_and_seek) {
    nema_obj_t *f;
    CHECK(nema_fs_open("/hello.txt", NEMA_O_WRITE | NEMA_O_APPEND, &f) == NEMA_OK);
    nema_obj_write(f, "!", 1, 0);
    nema_obj_unref(f);
    char b[64];
    CHECK(read_file("/hello.txt", b, sizeof(b)));
    CHECK(strcmp(b, "hello posix!") == 0);
    CHECK(nema_fs_open("/hello.txt", NEMA_O_READ, &f) == NEMA_OK);
    nema_seek_t sk = {6, 0};
    CHECK(f->ops->ioctl(f, NEMA_FIOC_SEEK, &sk) == NEMA_OK);
    char w[8] = {0};
    CHECK_EQ(nema_obj_read(f, w, 5, 0), 5);
    CHECK(memcmp(w, "posix", 5) == 0);
    uint32_t size = 0;
    f->ops->ioctl(f, NEMA_FIOC_SIZE, &size);
    CHECK_EQ(size, 12);
    nema_obj_unref(f);
}

TEST(mkdir_and_stat) {
    CHECK(nema_fs_mkdir("/dir") == NEMA_OK);
    CHECK(nema_fs_mkdir("/dir") == NEMA_ERR_EXIST);
    CHECK(host_has("dir"));
    nema_stat_t st;
    CHECK(nema_fs_stat("/dir", &st) == NEMA_OK);
    CHECK_EQ(st.type, NEMA_T_DIR);
    CHECK(nema_fs_mkdir("/nope/inner") == NEMA_ERR_NOENT);
}

TEST(readdir_lists_children) {
    CHECK(write_file("/dir/one.txt", "1") == NEMA_OK);
    CHECK(write_file("/dir/two.txt", "22") == NEMA_OK);
    CHECK(nema_fs_mkdir("/dir/sub") == NEMA_OK);
    int files = 0, dirs = 0, total = 0;
    bool saw_one = false, saw_sub = false;
    nema_dirent_t e;
    for (size_t i = 0; nema_fs_readdir("/dir", i, &e) == NEMA_OK; i++) {
        total++;
        if (e.type == NEMA_T_DIR) dirs++;
        else files++;
        if (strcmp(e.name, "one.txt") == 0) {
            saw_one = true;
            CHECK_EQ(e.size, 1);
        }
        if (strcmp(e.name, "sub") == 0) saw_sub = true;
    }
    CHECK_EQ(total, 3);
    CHECK_EQ(files, 2);
    CHECK_EQ(dirs, 1);
    CHECK(saw_one && saw_sub);
    CHECK(nema_fs_readdir("/dir", 3, &e) == NEMA_ERR_NOENT); /* past the end */
    CHECK(nema_fs_readdir("/dir/one.txt", 0, &e) == NEMA_ERR_NOTDIR);
}

TEST(rename_moves_files_across_directories) {
    CHECK(nema_fs_rename("/hello.txt", "/dir/moved.txt") == NEMA_OK);
    nema_stat_t st;
    CHECK(nema_fs_stat("/hello.txt", &st) == NEMA_ERR_NOENT);
    CHECK(nema_fs_stat("/dir/moved.txt", &st) == NEMA_OK);
    CHECK_EQ(st.size, 12);
    CHECK(host_has("dir/moved.txt"));
    CHECK(nema_fs_rename("/nope.txt", "/x.txt") == NEMA_ERR_NOENT);
}

TEST(open_directory_is_refused) {
    nema_obj_t *f;
    CHECK(nema_fs_open("/dir", NEMA_O_READ, &f) == NEMA_ERR_ISDIR);
}

TEST(unlink_removes_files_and_empty_dirs_only) {
    CHECK(nema_fs_unlink("/dir") == NEMA_ERR_STATE); /* not empty: ENOTEMPTY */
    CHECK(nema_fs_unlink("/dir/sub") == NEMA_OK);
    CHECK(nema_fs_unlink("/dir/one.txt") == NEMA_OK);
    CHECK(nema_fs_unlink("/dir/one.txt") == NEMA_ERR_NOENT);
    CHECK(nema_fs_unlink("/dir/moved.txt") == NEMA_OK);
    CHECK(nema_fs_unlink("/dir/two.txt") == NEMA_OK);
    CHECK(nema_fs_unlink("/dir") == NEMA_OK);
    CHECK(!host_has("dir"));
}

TEST(overlong_root_is_refused_not_truncated) {
    char long_root[400];
    memset(long_root, 'r', sizeof(long_root) - 1);
    long_root[0] = '/';
    long_root[sizeof(long_root) - 1] = 0;
    CHECK(nema_posixfs_create(long_root) == NULL);
}

static void remove_tree_on_host(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[320];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree_on_host(child);
            else unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}

/* The open listing (posix_fs.c keeps the folder open while the next index is asked for): it must give the same answers as a
 * fresh open in every order, and see a change made in the middle. */
TEST(a_listing_in_any_order_and_after_a_change) {
    CHECK(nema_fs_mkdir("/many") == NEMA_OK);
    char path[32];
    for (int i = 0; i < 12; i++) {
        snprintf(path, sizeof(path), "/many/f%02d", i);
        CHECK(write_file(path, "x") == NEMA_OK);
    }
    nema_dirent_t e, again;
    size_t n = 0;
    while (nema_fs_readdir("/many", n, &e) == NEMA_OK) n++;
    CHECK_EQ((int)n, 12);
    /* a jump back, and two folders read in turn, answer as a fresh open does */
    CHECK(nema_fs_readdir("/many", 5, &e) == NEMA_OK);
    CHECK(nema_fs_readdir("/dir", 0, &again) == NEMA_OK);
    CHECK(nema_fs_readdir("/many", 5, &again) == NEMA_OK && !strcmp(e.name, again.name));
    CHECK(nema_fs_readdir("/many", 2, &again) == NEMA_OK && strcmp(e.name, again.name));
    /* removing an entry in the middle of a listing: the rest of it sees the folder as it is now */
    CHECK(nema_fs_readdir("/many", 0, &e) == NEMA_OK);
    CHECK(nema_fs_unlink("/many/f00") == NEMA_OK && nema_fs_unlink("/many/f01") == NEMA_OK);
    n = 0;
    while (nema_fs_readdir("/many", n, &e) == NEMA_OK) n++;
    CHECK_EQ((int)n, 10);
    CHECK(write_file("/many/new", "y") == NEMA_OK);
    n = 0;
    bool saw = false;
    while (nema_fs_readdir("/many", n, &e) == NEMA_OK) saw |= !strcmp(e.name, "new"), n++;
    CHECK(n == 11 && saw);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(ROOT, sizeof(ROOT), "%s/palanu-posixfs-XXXXXX", tmp);
    if (!mkdtemp(ROOT)) {
        printf("cannot create a temp directory under %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    void *fs = nema_posixfs_create(ROOT);
    CHECK(fs != NULL);
    nema_vfs_mount("/", nema_posixfs_ops(), fs);
    RUN(create_write_read_on_the_host);
    RUN(append_and_seek);
    RUN(mkdir_and_stat);
    RUN(readdir_lists_children);
    RUN(a_listing_in_any_order_and_after_a_change);
    RUN(rename_moves_files_across_directories);
    RUN(open_directory_is_refused);
    RUN(unlink_removes_files_and_empty_dirs_only);
    RUN(overlong_root_is_refused_not_truncated);
    remove_tree_on_host(ROOT);
    return TEST_DONE();
}
