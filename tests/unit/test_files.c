/* The Files app's model and operations (apps/files_model.c, apps/files_ops.c): the incremental listing, the sort, the roots and their
 * free space, the path helpers, and the copy, move and delete jobs on RAM filesystems (one mounted at "/", one at "/mnt2" so a move
 * across them takes the copy-and-delete path). */
#include <string.h>

#include "files_model.h"
#include "files_ops.h"
#include "nema/nema.h"
#include "nema/vfs.h"
#include "test.h"

static void put(const char *path, const char *text) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) return;
    nema_obj_write(f, text, strlen(text), 0);
    nema_obj_unref(f);
}

static int size_of(const char *path, uint32_t *out) {
    nema_stat_t st;
    if (nema_fs_stat(path, &st) != NEMA_OK) return -1;
    if (out) *out = st.size;
    return st.type;
}

/* Runs a job to its end, a zero budget per step (at least one unit of work each), and returns the number of steps. */
static int run_job(files_job_t *j) {
    int steps = 0;
    while (!files_job_step(j, 0) && steps < 100000) steps++;
    return steps;
}

TEST(sort_folders_first_then_case_insensitive) {
    files_ent_t e[4];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "beta.txt"), e[0].type = NEMA_T_FILE;
    strcpy(e[1].name, "Zeta"), e[1].type = NEMA_T_DIR;
    strcpy(e[2].name, "alpha.txt"), e[2].type = NEMA_T_FILE;
    strcpy(e[3].name, "Apps"), e[3].type = NEMA_T_DIR;
    files_sort(e, 4);
    CHECK(strcmp(e[0].name, "Apps") == 0);
    CHECK(strcmp(e[1].name, "Zeta") == 0);
    CHECK(strcmp(e[2].name, "alpha.txt") == 0);
    CHECK(strcmp(e[3].name, "beta.txt") == 0);
}

TEST(listing_reads_a_few_entries_per_step) {
    nema_fs_mkdir("/big");
    for (int i = 0; i < 30; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/big/f%02d.txt", i);
        put(p, "x");
    }
    nema_fs_mkdir("/big/sub");
    files_list_t L;
    memset(&L, 0, sizeof(L));
    CHECK(files_list_begin(&L, "/big") == NEMA_OK);
    CHECK(L.loading);
    int steps = 0, total = 0;
    while (L.loading && steps < 100) {
        int k = files_list_step(&L, 4);
        CHECK(k <= 4);
        total += k;
        steps++;
    }
    CHECK(!L.loading);
    CHECK_EQ(L.n, 31);
    CHECK_EQ(total, 31);
    CHECK(steps >= 8); /* 31 entries at four per step: the listing was spread over several steps */
    CHECK(L.ents[0].type == NEMA_T_DIR); /* the folder comes first */
    CHECK(strcmp(L.ents[1].name, "f00.txt") == 0);
    CHECK(files_find(&L, "f29.txt") >= 0);
    CHECK_EQ(files_find(&L, "nope"), -1);
    files_list_free(&L);
}

TEST(roots_list_mounted_folders_with_free_space) {
    nema_fs_mkdir("/home");
    nema_fs_mkdir("/tmp");
    files_list_t L;
    memset(&L, 0, sizeof(L));
    CHECK(files_list_begin(&L, "/") == NEMA_OK);
    CHECK(!L.loading);
    CHECK(L.roots);
    CHECK(L.n >= 2);
    CHECK(files_find(&L, "/home") >= 0);
    CHECK(files_find(&L, "/sd") < 0); /* not mounted in the test */
    int h = files_find(&L, "/home");
    CHECK(L.ents[h].type == FILES_T_ROOT);
    CHECK(L.ents[h].size > 0);            /* free bytes of the RAM filesystem */
    CHECK(files_kind(&L.ents[h]) == FILES_K_ROOT);
    files_list_free(&L);
}

TEST(path_helpers) {
    char o[NEMA_PATH_MAX];
    files_join(o, sizeof(o), "/", "home");
    CHECK(strcmp(o, "/home") == 0);
    files_join(o, sizeof(o), "/home/a", "b.txt");
    CHECK(strcmp(o, "/home/a/b.txt") == 0);
    CHECK(files_parent(o, sizeof(o), "/home/a/b.txt") && strcmp(o, "/home/a") == 0);
    CHECK(files_parent(o, sizeof(o), "/home") && strcmp(o, "/") == 0);
    CHECK(!files_parent(o, sizeof(o), "/"));
    CHECK(strcmp(files_base("/home/a/b.txt"), "b.txt") == 0);
    CHECK(strcmp(files_base("/"), "/") == 0);
    CHECK(files_under("/apps/x/app.papp", "/apps"));
    CHECK(files_under("/apps", "/apps"));
    CHECK(!files_under("/appsx", "/apps"));
    CHECK(files_under("/anything", "/"));
    files_path_fit(o, sizeof(o), "/home/short", 30);
    CHECK(strcmp(o, "/home/short") == 0);
    files_path_fit(o, sizeof(o), "/home/docs/projects/2026/notes.txt", 16);
    CHECK(strlen(o) <= 16);
    CHECK(strncmp(o, "...", 3) == 0);
    CHECK(strstr(o, "notes.txt") != NULL);
    CHECK(files_valid_name("notes.txt"));
    CHECK(!files_valid_name(""));
    CHECK(!files_valid_name("."));
    CHECK(!files_valid_name(".."));
    CHECK(!files_valid_name("a/b"));
}

TEST(sizes_kinds_and_icons) {
    char s[16];
    files_size_str(s, sizeof(s), 512);
    CHECK(strcmp(s, "512B") == 0);
    files_size_str(s, sizeof(s), 12 * 1024);
    CHECK(strcmp(s, "12K") == 0);
    files_size_str(s, sizeof(s), 3u * 1024u * 1024u);
    CHECK(strcmp(s, "3M") == 0);
    CHECK(files_is_text_name("notes.TXT"));
    CHECK(!files_is_text_name("photo.png"));
    CHECK(!files_is_text_name(".profile"));
    files_ent_t e = {"app.papp", NEMA_T_FILE, 10};
    CHECK(files_kind(&e) == FILES_K_PACKAGE);
    CHECK(strcmp(files_icon_name(files_kind(&e)), "package") == 0);
    strcpy(e.name, "readme.md");
    CHECK(files_kind(&e) == FILES_K_TEXT);
    strcpy(e.name, "image.jpg");
    CHECK(files_kind(&e) == FILES_K_OTHER);
    e.type = NEMA_T_DIR;
    CHECK(files_kind(&e) == FILES_K_DIR);
}

TEST(copy_file_and_folder_recursively) {
    files_job_t *j = files_job_new();
    CHECK(j != NULL);
    nema_fs_mkdir("/src");
    nema_fs_mkdir("/src/tree");
    nema_fs_mkdir("/src/tree/deep");
    put("/src/tree/a.txt", "alpha");
    put("/src/tree/deep/b.txt", "bravo bravo");
    put("/src/single.txt", "0123456789");
    CHECK(files_job_copy(j, "/src/single.txt", "/dst1.txt", false) == NEMA_OK);
    run_job(j);
    CHECK(files_job_result(j) == NEMA_OK);
    uint32_t sz = 0;
    CHECK(size_of("/dst1.txt", &sz) == NEMA_T_FILE && sz == 10);
    CHECK(size_of("/src/single.txt", NULL) == NEMA_T_FILE); /* a copy keeps the source */

    CHECK(files_job_copy(j, "/src/tree", "/copy", false) == NEMA_OK);
    run_job(j);
    CHECK(files_job_result(j) == NEMA_OK);
    CHECK(size_of("/copy/deep", NULL) == NEMA_T_DIR);
    CHECK(size_of("/copy/a.txt", &sz) == NEMA_T_FILE && sz == 5);
    CHECK(size_of("/copy/deep/b.txt", &sz) == NEMA_T_FILE && sz == 11);
    CHECK(size_of("/src/tree/deep/b.txt", NULL) == NEMA_T_FILE);
    files_job_free(j);
}

TEST(copy_overwrites_files_and_merges_folders) {
    files_job_t *j = files_job_new();
    nema_fs_mkdir("/m1");
    nema_fs_mkdir("/m1/x");
    put("/m1/x/same.txt", "new contents");
    nema_fs_mkdir("/m2");
    nema_fs_mkdir("/m2/x");
    put("/m2/x/same.txt", "old");
    put("/m2/x/keep.txt", "kept");
    CHECK(files_job_copy(j, "/m1/x", "/m2/x", false) == NEMA_OK);
    run_job(j);
    CHECK(files_job_result(j) == NEMA_OK);
    uint32_t sz = 0;
    CHECK(size_of("/m2/x/same.txt", &sz) == NEMA_T_FILE && sz == 12);
    CHECK(size_of("/m2/x/keep.txt", &sz) == NEMA_T_FILE && sz == 4);
    files_job_free(j);
}

TEST(into_itself_and_same_path_are_refused) {
    files_job_t *j = files_job_new();
    nema_fs_mkdir("/loop");
    CHECK(files_job_copy(j, "/loop", "/loop/inside", false) == NEMA_ERR_INVAL);
    CHECK(files_job_copy(j, "/loop", "/loop", false) == NEMA_ERR_INVAL);
    CHECK(files_job_copy(j, "/no-such", "/x", false) == NEMA_ERR_NOENT);
    files_job_free(j);
}

TEST(move_is_a_rename_on_one_filesystem) {
    files_job_t *j = files_job_new();
    nema_fs_mkdir("/mv");
    put("/mv/a.txt", "moved");
    CHECK(files_job_copy(j, "/mv/a.txt", "/mv/b.txt", true) == NEMA_OK);
    CHECK(!files_job_busy(j)); /* done at once: a rename */
    CHECK(files_job_result(j) == NEMA_OK);
    CHECK(size_of("/mv/a.txt", NULL) < 0);
    CHECK(size_of("/mv/b.txt", NULL) == NEMA_T_FILE);
    files_job_free(j);
}

TEST(move_across_filesystems_copies_then_deletes) {
    nema_fs_mkdir("/mnt2");
    CHECK(nema_vfs_mount("/mnt2", nema_ramfs_ops(), nema_ramfs_create(256 * 1024)) == NEMA_OK);
    files_job_t *j = files_job_new();
    nema_fs_mkdir("/cross");
    nema_fs_mkdir("/cross/dir");
    put("/cross/dir/f.txt", "payload");
    CHECK(files_job_copy(j, "/cross/dir", "/mnt2/dir", true) == NEMA_OK);
    run_job(j);
    CHECK(files_job_result(j) == NEMA_OK);
    CHECK(size_of("/mnt2/dir/f.txt", NULL) == NEMA_T_FILE);
    CHECK(size_of("/cross/dir", NULL) < 0); /* the source is gone */
    files_job_free(j);
    nema_vfs_unmount("/mnt2");
}

TEST(delete_removes_a_tree_and_counts_it) {
    files_job_t *j = files_job_new();
    nema_fs_mkdir("/del");
    nema_fs_mkdir("/del/one");
    nema_fs_mkdir("/del/one/two");
    put("/del/one/two/x.txt", "x");
    put("/del/one/y.txt", "y");
    bool more = true;
    CHECK_EQ(files_count_tree("/del", 100, &more), 5); /* the root, one, two, x.txt, y.txt */
    CHECK(!more);
    CHECK_EQ(files_count_tree("/del", 3, &more), 3);
    CHECK(more);
    CHECK(files_job_delete(j, "/del") == NEMA_OK);
    run_job(j);
    CHECK(files_job_result(j) == NEMA_OK);
    CHECK(size_of("/del", NULL) < 0);
    put("/single.txt", "s");
    CHECK(files_job_delete(j, "/single.txt") == NEMA_OK);
    run_job(j);
    CHECK(size_of("/single.txt", NULL) < 0);
    CHECK(files_job_result(j) == NEMA_OK);
    CHECK(files_job_delete(j, "/gone") == NEMA_ERR_NOENT);
    files_job_free(j);
}

TEST(status_text_names_the_operation) {
    files_job_t *j = files_job_new();
    char s[48];
    files_job_status(j, s, sizeof(s));
    CHECK(strcmp(s, "Working") == 0);
    nema_fs_mkdir("/st");
    put("/st/a", "aaaa");
    CHECK(files_job_copy(j, "/st/a", "/st/b", false) == NEMA_OK);
    files_job_status(j, s, sizeof(s));
    CHECK(strncmp(s, "Copying", 7) == 0);
    run_job(j);
    files_job_free(j);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(4 * 1024 * 1024));
    nema_fs_mkdir("/home");
    nema_fs_mkdir("/data");
    nema_fs_mkdir("/apps");
    nema_fs_mkdir("/tmp");
    RUN(sort_folders_first_then_case_insensitive);
    RUN(listing_reads_a_few_entries_per_step);
    RUN(roots_list_mounted_folders_with_free_space);
    RUN(path_helpers);
    RUN(sizes_kinds_and_icons);
    RUN(copy_file_and_folder_recursively);
    RUN(copy_overwrites_files_and_merges_folders);
    RUN(into_itself_and_same_path_are_refused);
    RUN(move_is_a_rename_on_one_filesystem);
    RUN(move_across_filesystems_copies_then_deletes);
    RUN(delete_removes_a_tree_and_counts_it);
    RUN(status_text_names_the_operation);
    return TEST_DONE();
}
