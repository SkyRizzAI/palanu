#include <string.h>

#include "nema/nema.h"
#include "test.h"

static int app_noop(int argc, char **argv) { return 0; }
static const nema_app_t A1 = {"alpha", "", app_noop, 0, 0}, A2 = {"beta", "", app_noop, 0, 0},
                        AH = {"secret", "", app_noop, 0, NEMA_APP_HIDDEN};

TEST(path_normalisation) {
    char o[64];
    CHECK(nema_path_resolve("/a/b/../c/./d", o, sizeof(o)) == NEMA_OK);
    CHECK(strcmp(o, "/a/c/d") == 0);
    CHECK(nema_path_resolve("//x///y/", o, sizeof(o)) == NEMA_OK);
    CHECK(strcmp(o, "/x/y") == 0);
    CHECK(nema_path_resolve("/..", o, sizeof(o)) == NEMA_OK);
    CHECK(strcmp(o, "/") == 0);
    CHECK(nema_path_resolve("rel/ative", o, sizeof(o)) == NEMA_OK);
    CHECK(strcmp(o, "/rel/ative") == 0); /* no process: cwd is "/" */
}

TEST(create_write_read_seek) {
    nema_obj_t *f;
    CHECK(nema_fs_open("/note.txt", NEMA_O_READ | NEMA_O_WRITE, &f) == NEMA_ERR_NOENT);
    CHECK(nema_fs_open("/note.txt", NEMA_O_READ | NEMA_O_WRITE | NEMA_O_CREATE, &f) == NEMA_OK);
    CHECK_EQ(nema_obj_write(f, "hello world", 11, 0), 11);
    nema_seek_t sk = {0, 0};
    CHECK(f->ops->ioctl(f, NEMA_FIOC_SEEK, &sk) == NEMA_OK);
    char b[32] = {0};
    CHECK_EQ(nema_obj_read(f, b, sizeof(b), 0), 11);
    CHECK(strcmp(b, "hello world") == 0);
    CHECK_EQ(nema_obj_read(f, b, sizeof(b), 0), 0); /* end of file */
    sk.off = 6;
    f->ops->ioctl(f, NEMA_FIOC_SEEK, &sk);
    CHECK_EQ(nema_obj_read(f, b, sizeof(b), 0), 5);
    CHECK(memcmp(b, "world", 5) == 0);
    uint32_t size = 0;
    f->ops->ioctl(f, NEMA_FIOC_SIZE, &size);
    CHECK_EQ(size, 11);
    nema_obj_unref(f);
    nema_stat_t st;
    CHECK(nema_fs_stat("/note.txt", &st) == NEMA_OK);
    CHECK_EQ(st.type, NEMA_T_FILE);
    CHECK_EQ(st.size, 11);
}

TEST(append_and_truncate) {
    nema_obj_t *f;
    nema_fs_open("/log", NEMA_O_WRITE | NEMA_O_CREATE, &f);
    nema_obj_write(f, "one", 3, 0);
    nema_obj_unref(f);
    nema_fs_open("/log", NEMA_O_WRITE | NEMA_O_APPEND, &f);
    nema_obj_write(f, "two", 3, 0);
    nema_obj_unref(f);
    nema_stat_t st;
    nema_fs_stat("/log", &st);
    CHECK_EQ(st.size, 6);
    nema_fs_open("/log", NEMA_O_WRITE | NEMA_O_TRUNC, &f);
    nema_obj_unref(f);
    nema_fs_stat("/log", &st);
    CHECK_EQ(st.size, 0);
}

TEST(directories) {
    CHECK(nema_fs_mkdir("/home") == NEMA_OK);
    CHECK(nema_fs_mkdir("/home") == NEMA_ERR_EXIST);
    CHECK(nema_fs_mkdir("/no/parent") == NEMA_ERR_NOENT);
    nema_obj_t *f;
    nema_fs_open("/home/a.txt", NEMA_O_WRITE | NEMA_O_CREATE, &f);
    nema_obj_unref(f);
    nema_fs_open("/home/b.txt", NEMA_O_WRITE | NEMA_O_CREATE, &f);
    nema_obj_unref(f);
    nema_dirent_t e;
    CHECK(nema_fs_readdir("/home", 0, &e) == NEMA_OK);
    CHECK(strcmp(e.name, "a.txt") == 0);
    CHECK(nema_fs_readdir("/home", 1, &e) == NEMA_OK);
    CHECK(strcmp(e.name, "b.txt") == 0);
    CHECK(nema_fs_readdir("/home", 2, &e) == NEMA_ERR_NOENT);
    CHECK(nema_fs_unlink("/home") == NEMA_ERR_STATE); /* not empty */
    CHECK(nema_fs_open("/home", NEMA_O_READ, &f) == NEMA_ERR_ISDIR);
}

TEST(rename_and_unlink) {
    nema_obj_t *f;
    nema_fs_open("/r1", NEMA_O_WRITE | NEMA_O_CREATE, &f);
    nema_obj_write(f, "x", 1, 0);
    nema_obj_unref(f);
    CHECK(nema_fs_rename("/r1", "/home/r2") == NEMA_OK);
    nema_stat_t st;
    CHECK(nema_fs_stat("/r1", &st) == NEMA_ERR_NOENT);
    CHECK(nema_fs_stat("/home/r2", &st) == NEMA_OK);
    CHECK(nema_fs_unlink("/home/r2") == NEMA_OK);
    CHECK(nema_fs_stat("/home/r2", &st) == NEMA_ERR_NOENT);
}

TEST(unlink_with_open_handle_defers_free) {
    nema_obj_t *f;
    nema_fs_open("/tmpf", NEMA_O_READ | NEMA_O_WRITE | NEMA_O_CREATE, &f);
    nema_obj_write(f, "abc", 3, 0);
    CHECK(nema_fs_unlink("/tmpf") == NEMA_OK);
    nema_seek_t sk = {0, 0};
    f->ops->ioctl(f, NEMA_FIOC_SEEK, &sk);
    char b[4] = {0};
    CHECK_EQ(nema_obj_read(f, b, 3, 0), 3); /* still readable */
    nema_obj_unref(f);
}

TEST(budget_is_enforced) {
    void *small = nema_ramfs_create(1024);
    CHECK(small != NULL);
    CHECK(nema_vfs_mount("/small", nema_ramfs_ops(), small) == NEMA_OK);
    nema_obj_t *f;
    CHECK(nema_fs_open("/small/big", NEMA_O_WRITE | NEMA_O_CREATE, &f) == NEMA_OK);
    char blk[512];
    memset(blk, 'x', sizeof(blk));
    int total = 0, r;
    while ((r = nema_obj_write(f, blk, sizeof(blk), 0)) > 0) total += r;
    CHECK_EQ(r, NEMA_ERR_NOSPC);
    CHECK(total <= 1024);
    nema_obj_unref(f);
}

TEST(mounts_appear_in_listings_and_route_by_prefix) {
    nema_dirent_t e;
    bool saw_small = false;
    for (size_t i = 0; nema_fs_readdir("/", i, &e) == NEMA_OK; i++)
        if (strcmp(e.name, "small") == 0 && e.type == NEMA_T_DIR) saw_small = true;
    CHECK(saw_small);
    nema_stat_t st;
    CHECK(nema_fs_stat("/small", &st) == NEMA_OK);
    CHECK_EQ(st.type, NEMA_T_DIR);
    CHECK(nema_fs_rename("/note.txt", "/small/note.txt") == NEMA_ERR_UNSUPPORTED); /* across filesystems */
}

TEST(bin_lists_visible_apps) {
    CHECK(nema_vfs_mount("/bin", nema_binfs_ops(), NULL) == NEMA_OK);
    nema_dirent_t e;
    CHECK(nema_fs_readdir("/bin", 0, &e) == NEMA_OK);
    CHECK(strcmp(e.name, "alpha") == 0);
    CHECK(nema_fs_readdir("/bin", 1, &e) == NEMA_OK);
    CHECK(strcmp(e.name, "beta") == 0);
    CHECK(nema_fs_readdir("/bin", 2, &e) == NEMA_ERR_NOENT); /* the hidden app is not listed */
    nema_stat_t st;
    CHECK(nema_fs_stat("/bin/secret", &st) == NEMA_OK);
    nema_obj_t *f;
    CHECK(nema_fs_open("/bin/alpha", NEMA_O_READ, &f) == NEMA_OK);
    char b[64] = {0};
    nema_obj_read(f, b, sizeof(b) - 1, 0);
    CHECK(strcmp(b, "#!palanu alpha\n") == 0);
    nema_obj_unref(f);
    CHECK(nema_fs_open("/bin/alpha", NEMA_O_WRITE, &f) == NEMA_ERR_PERM);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_app_register(&A1);
    nema_app_register(&A2);
    nema_app_register(&AH);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(1024 * 1024));
    RUN(path_normalisation);
    RUN(create_write_read_seek);
    RUN(append_and_truncate);
    RUN(directories);
    RUN(rename_and_unlink);
    RUN(unlink_with_open_handle_defers_free);
    RUN(budget_is_enforced);
    RUN(mounts_appear_in_listings_and_route_by_prefix);
    RUN(bin_lists_visible_apps);
    return TEST_DONE();
}
