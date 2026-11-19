/* The shell parser: tokens, commands, pipelines, and the operators that join pipelines.
 * Driven through sh_tokenize / sh_parse_pipeline / sh_run_line in apps/sh_parse.h; no process is spawned for
 * the parser checks. The operator checks use builtins only, so they run in-process too. */
#include <string.h>

#include "nema/nema.h"
#include "sh_parse.h"
#include "test.h"

static sh_t SH;
static toks_t K;
static cmd_t C[STAGES_MAX];
static char G[512];
static size_t GUSED;

static void reset_sh(void) {
    memset(&SH, 0, sizeof(SH));
    SH.owner = nema_owner_register("shparse");
    SH.cols = 80;
}
static void lex(const char *line) { sh_tokenize(&SH, line, &K); }
static bool tok_is(int i, int kind, const char *s) {
    if (i >= K.n || K.t[i].kind != kind) return false;
    if (kind != T_WORD) return s == NULL;
    return strcmp(K.t[i].s, s) == 0;
}
static bool word(int i, const char *s) { return tok_is(i, T_WORD, s); }
static int parse_at(int *i, int *nc, bool *bg) {
    GUSED = 0;
    return sh_parse_pipeline(&SH, &K, i, C, nc, bg, G, &GUSED, sizeof(G));
}
static void set_var(const char *name, const char *val) {
    for (int i = 0; i < SH_VARS_MAX; i++)
        if (!SH.var[i].name[0]) {
            nema_strlcpy(SH.var[i].name, name, sizeof(SH.var[i].name));
            nema_strlcpy(SH.var[i].val, val, sizeof(SH.var[i].val));
            return;
        }
}

/* ---------------------------------------------------------------- quoting and escapes */
TEST(single_quotes_are_literal) {
    reset_sh();
    set_var("NAME", "palanu");
    lex("echo 'a b' '$NAME \\n' 'x|y;z'");
    CHECK_EQ(K.n, 4);
    CHECK(word(1, "a b"));
    CHECK(word(2, "$NAME \\n")); /* backslash and $ are plain inside single quotes */
    CHECK(word(3, "x|y;z"));    /* operators inside quotes are plain text */
}

TEST(double_quotes_keep_spaces_and_expand) {
    reset_sh();
    set_var("NAME", "palanu");
    lex("echo \"x $NAME  y\" \"a\\\"b\" \"a\\\\b\" \"a\\$b\" \"a\\nb\"");
    CHECK_EQ(K.n, 6);
    CHECK(word(1, "x palanu  y"));
    CHECK(word(2, "a\"b"));
    CHECK(word(3, "a\\b"));
    CHECK(word(4, "a$b"));
    CHECK(word(5, "a\\nb")); /* \n is not an escape in double quotes */
}

TEST(backslash_escapes_outside_quotes) {
    reset_sh();
    lex("a\\ b \\*x \\$HOME");
    CHECK_EQ(K.n, 3);
    CHECK(word(0, "a b"));
    CHECK(word(1, "*x"));
    CHECK(!K.t[1].glob); /* an escaped star is not a glob */
    CHECK(word(2, "$HOME"));
}

TEST(empty_and_adjacent_quotes) {
    reset_sh();
    lex("x '' a''b \"\"");
    CHECK_EQ(K.n, 4);
    CHECK(word(1, ""));
    CHECK(word(2, "ab"));
    CHECK(word(3, ""));
}

TEST(tilde_expands_only_at_word_start) {
    reset_sh();
    set_var("HOME", "/home/u");
    lex("cd ~ ~/src a~ '~'");
    CHECK(word(1, "/home/u"));
    CHECK(word(2, "/home/u/src"));
    CHECK(word(3, "a~"));
    CHECK(word(4, "~"));
}

/* ---------------------------------------------------------------- variables */
TEST(dollar_vars_and_braces) {
    reset_sh();
    set_var("NAME", "palanu");
    lex("$NAME ${NAME}x pre$NAME-post a_1 $NAME2");
    CHECK(word(0, "palanu"));
    CHECK(word(1, "palanux"));
    CHECK(word(2, "prepalanu-post"));
    CHECK(word(3, "a_1"));
    CHECK(word(4, "")); /* NAME2 is unset: expands to nothing */
}

TEST(last_status_and_lone_dollar) {
    reset_sh();
    SH.status = 7;
    lex("echo $? a$ \\$? $1");
    CHECK(word(1, "7"));
    CHECK(word(2, "a$"));
    CHECK(word(3, "$?"));
    CHECK(word(4, "$1")); /* digits do not start a name, so the $ stays */
}

TEST(single_quoted_var_is_literal) {
    reset_sh();
    set_var("NAME", "palanu");
    lex("'$NAME' \"$NAME\"");
    CHECK(word(0, "$NAME"));
    CHECK(word(1, "palanu"));
}

/* ---------------------------------------------------------------- operators and redirections */
TEST(operator_tokens) {
    reset_sh();
    lex("a|b||c&&d&e;f");
    CHECK_EQ(K.n, 11);
    CHECK(word(0, "a"));
    CHECK(tok_is(1, T_PIPE, NULL));
    CHECK(word(2, "b"));
    CHECK(tok_is(3, T_OR, NULL));
    CHECK(word(4, "c"));
    CHECK(tok_is(5, T_AND, NULL));
    CHECK(word(6, "d"));
    CHECK(tok_is(7, T_BG, NULL));
    CHECK(word(8, "e"));
    CHECK(tok_is(9, T_SEMI, NULL));
    CHECK(word(10, "f"));
}

TEST(redirection_tokens) {
    reset_sh();
    lex("cat <in >out >>app 2>err 2>&1 x2>y");
    CHECK(tok_is(1, T_IN, NULL));
    CHECK(tok_is(3, T_OUT, NULL));
    CHECK(tok_is(5, T_APP, NULL));
    CHECK(tok_is(7, T_ERR, NULL));
    CHECK(tok_is(9, T_ERR2OUT, NULL));
    CHECK(word(10, "x2")); /* 2> only counts as a redirection at the start of a word */
    CHECK(tok_is(11, T_OUT, NULL));
}

TEST(comments_end_the_line) {
    reset_sh();
    lex("echo hi # there ; more");
    CHECK_EQ(K.n, 2);
    lex("# only a comment");
    CHECK_EQ(K.n, 0);
    lex("echo a#b");
    CHECK_EQ(K.n, 2);
    CHECK(word(1, "a#b")); /* a # inside a word is ordinary text */
}

TEST(glob_flags) {
    reset_sh();
    lex("*.txt a?c '*' \\* x*y");
    CHECK(K.t[0].glob);
    CHECK(K.t[1].glob);
    CHECK(!K.t[2].glob);
    CHECK(!K.t[3].glob);
    CHECK(K.t[4].glob);
}

TEST(too_many_words_is_an_error) {
    reset_sh();
    char line[400];
    line[0] = 0;
    for (int i = 0; i < TOK_MAX + 2; i++) strcat(line, "w ");
    lex(line);
    CHECK(K.error != NULL);
}

/* ---------------------------------------------------------------- pipelines */
TEST(pipeline_of_three_stages) {
    reset_sh();
    lex("ls -l | grep x | wc");
    int i = 0, nc = 0;
    bool bg = true;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK_EQ(nc, 3);
    CHECK(!bg);
    CHECK_EQ(i, K.n);
    CHECK_EQ(C[0].argc, 2);
    CHECK(strcmp(C[0].argv[0], "ls") == 0 && strcmp(C[0].argv[1], "-l") == 0 && C[0].argv[2] == NULL);
    CHECK(strcmp(C[1].argv[0], "grep") == 0 && strcmp(C[1].argv[1], "x") == 0);
    CHECK(strcmp(C[2].argv[0], "wc") == 0);
}

TEST(four_stages_ok_five_is_an_error) {
    reset_sh();
    lex("a | b | c | d");
    int i = 0, nc = 0;
    bool bg = false;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK_EQ(nc, 4);
    lex("a | b | c | d | e");
    i = 0;
    CHECK_EQ(parse_at(&i, &nc, &bg), -1);
}

TEST(pipeline_stops_at_separator_and_reports_background) {
    reset_sh();
    lex("echo hi & echo two");
    int i = 0, nc = 0;
    bool bg = false;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK(bg);
    CHECK_EQ(nc, 1);
    CHECK(tok_is(i, T_BG, NULL)); /* left at the operator for the caller */
    lex("echo hi ; echo two");
    i = 0;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK(!bg);
    CHECK(tok_is(i, T_SEMI, NULL));
}

TEST(empty_stage_is_a_syntax_error) {
    reset_sh();
    int i = 0, nc = 0;
    bool bg = false;
    lex("ls |");
    CHECK_EQ(parse_at(&i, &nc, &bg), -1);
    lex("| ls");
    i = 0;
    CHECK_EQ(parse_at(&i, &nc, &bg), -1);
    lex("cat <");
    i = 0;
    CHECK_EQ(parse_at(&i, &nc, &bg), -1); /* a redirection needs a file name */
    lex("ls | | ls");
    i = 0;
    CHECK_EQ(parse_at(&i, &nc, &bg), -1); /* an empty middle stage */
}

TEST(redirections_are_attached_to_the_command) {
    reset_sh();
    lex("cat <in >out 2>err | sort >>app 2>&1");
    int i = 0, nc = 0;
    bool bg = false;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK_EQ(nc, 2);
    CHECK(strcmp(C[0].in, "in") == 0);
    CHECK(strcmp(C[0].out, "out") == 0);
    CHECK(strcmp(C[0].err, "err") == 0);
    CHECK(!C[0].append);
    CHECK(!C[1].in && strcmp(C[1].out, "app") == 0);
    CHECK(C[1].append);
    CHECK(C[1].err_to_out);
    CHECK_EQ(C[0].argc, 1);
}

TEST(alias_splits_into_words) {
    reset_sh();
    nema_strlcpy(SH.alias[0].name, "ll", sizeof(SH.alias[0].name));
    nema_strlcpy(SH.alias[0].val, "ls -l", sizeof(SH.alias[0].val));
    lex("ll /x");
    int i = 0, nc = 0;
    bool bg = false;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK_EQ(C[0].argc, 3);
    CHECK(strcmp(C[0].argv[0], "ls") == 0 && strcmp(C[0].argv[1], "-l") == 0 && strcmp(C[0].argv[2], "/x") == 0);
}

/* ---------------------------------------------------------------- globbing against the VFS */
static void touch(const char *path) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE, &f) == NEMA_OK) nema_obj_unref(f);
}

TEST(glob_expands_sorted_matches) {
    reset_sh();
    lex("echo /gl/*.txt");
    int i = 0, nc = 0;
    bool bg = false;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK_EQ(C[0].argc, 3);
    CHECK(strcmp(C[0].argv[1], "/gl/a.txt") == 0);
    CHECK(strcmp(C[0].argv[2], "/gl/b.txt") == 0);
}

TEST(glob_question_mark_and_hidden_files) {
    reset_sh();
    lex("echo /gl/?.txt /gl/*");
    int i = 0, nc = 0;
    bool bg = false;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    /* "?.txt" gives a.txt b.txt; "*" gives a.txt b.txt c.log (dotfiles are skipped) */
    CHECK_EQ(C[0].argc, 1 + 2 + 3);
    CHECK(strcmp(C[0].argv[1], "/gl/a.txt") == 0);
    CHECK(strcmp(C[0].argv[3], "/gl/a.txt") == 0);
    CHECK(strcmp(C[0].argv[5], "/gl/c.log") == 0);
}

TEST(glob_without_match_keeps_the_word) {
    reset_sh();
    lex("echo /gl/*.zip");
    int i = 0, nc = 0;
    bool bg = false;
    CHECK_EQ(parse_at(&i, &nc, &bg), 0);
    CHECK_EQ(C[0].argc, 2);
    CHECK(strcmp(C[0].argv[1], "/gl/*.zip") == 0);
}

/* ---------------------------------------------------------------- running lines (builtins only) */
static void run(const char *line) { sh_run_line(&SH, line); }
static const char *var(const char *name) {
    const char *v = sh_var(&SH, name);
    return v ? v : "(unset)";
}

TEST(semicolon_runs_both) {
    reset_sh();
    run("export S1=1; export S2=2");
    CHECK(strcmp(var("S1"), "1") == 0);
    CHECK(strcmp(var("S2"), "2") == 0);
}

TEST(and_runs_right_only_on_success) {
    reset_sh();
    run("cd /no/such/dir && export A1=1");
    CHECK(strcmp(var("A1"), "(unset)") == 0);
    run("export A2=1 && export A3=1");
    CHECK(strcmp(var("A3"), "1") == 0);
}

TEST(or_runs_right_only_on_failure) {
    reset_sh();
    run("cd /no/such/dir || export O1=ran");
    CHECK(strcmp(var("O1"), "ran") == 0);
    run("export O2=1 || export O2=2");
    CHECK(strcmp(var("O2"), "1") == 0);
}

TEST(mixed_chain_follows_left_to_right) {
    reset_sh();
    run("cd /no/such/dir && export M1=1 || export M2=2");
    CHECK(strcmp(var("M1"), "(unset)") == 0);
    CHECK(strcmp(var("M2"), "2") == 0);
}

TEST(a_list_ending_in_ampersand_runs_whole_in_the_background) {
    reset_sh();
    run("export B1=1 && export B2=1 & export B3=1");
    CHECK(strcmp(var("B1"), "(unset)") == 0); /* the first command went with the list, not into this shell */
    CHECK(strcmp(var("B2"), "(unset)") == 0);
    CHECK(strcmp(var("B3"), "1") == 0);       /* what follows the & runs here */
}

TEST(comment_is_not_run) {
    reset_sh();
    run("export C1=1 # export C1=2");
    CHECK(strcmp(var("C1"), "1") == 0);
}

TEST(assignment_sets_a_shell_variable) {
    reset_sh();
    run("FOO=bar");
    CHECK(strcmp(var("FOO"), "bar") == 0);
}

TEST(variable_from_an_earlier_command_in_the_same_line) {
    reset_sh();
    /* POSIX shells expand each command when it runs, so $A here is "1". This shell tokenizes the whole line
     * first; the test pins down what actually happens. */
    run("export A=1; export B=$A");
    CHECK(strcmp(var("B"), "1") == 0);
}

TEST(six_stage_pipe_is_a_syntax_error) {
    reset_sh();
    CHECK_EQ(sh_run_line(&SH, "ls | ls | ls | ls | ls | ls"), 2);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(256 * 1024));
    nema_fs_mkdir("/gl");
    touch("/gl/a.txt");
    touch("/gl/b.txt");
    touch("/gl/c.log");
    touch("/gl/.hidden.txt");
    RUN(single_quotes_are_literal);
    RUN(double_quotes_keep_spaces_and_expand);
    RUN(backslash_escapes_outside_quotes);
    RUN(empty_and_adjacent_quotes);
    RUN(tilde_expands_only_at_word_start);
    RUN(dollar_vars_and_braces);
    RUN(last_status_and_lone_dollar);
    RUN(single_quoted_var_is_literal);
    RUN(operator_tokens);
    RUN(redirection_tokens);
    RUN(comments_end_the_line);
    RUN(glob_flags);
    RUN(too_many_words_is_an_error);
    RUN(pipeline_of_three_stages);
    RUN(four_stages_ok_five_is_an_error);
    RUN(pipeline_stops_at_separator_and_reports_background);
    RUN(empty_stage_is_a_syntax_error);
    RUN(redirections_are_attached_to_the_command);
    RUN(alias_splits_into_words);
    RUN(glob_expands_sorted_matches);
    RUN(glob_question_mark_and_hidden_files);
    RUN(glob_without_match_keeps_the_word);
    RUN(semicolon_runs_both);
    RUN(and_runs_right_only_on_success);
    RUN(or_runs_right_only_on_failure);
    RUN(mixed_chain_follows_left_to_right);
    RUN(a_list_ending_in_ampersand_runs_whole_in_the_background);
    RUN(comment_is_not_run);
    RUN(assignment_sets_a_shell_variable);
    RUN(variable_from_an_earlier_command_in_the_same_line);
    RUN(six_stage_pipe_is_a_syntax_error);
    return TEST_DONE();
}
