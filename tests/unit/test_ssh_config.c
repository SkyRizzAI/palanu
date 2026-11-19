/* SSH profiles (apps/ssh_config.c): a host's block, patterns and negation, the first value wins, ~ is /home, comments and quotes. */
#include <string.h>

#include "../../apps/ssh_config.h"
#include "test.h"

static const char *CFG = "# my servers\n"
                         "Host pve\n"
                         "    HostName 192.168.1.5\n"
                         "    User root\n"
                         "    Port 2222\n"
                         "Host *.lab !secret.lab\n"
                         "    User labuser\n"
                         "    IdentityFile ~/.ssh/lab_key\n"
                         "Host *\n"
                         "    User everyone   # comment\n"
                         "    Port=22\n";

TEST(a_host_block_and_the_fallback) {
    ssh_profile_t p;
    CHECK(ssh_config_lookup(CFG, strlen(CFG), "pve", &p));
    CHECK(!strcmp(p.hostname, "192.168.1.5") && !strcmp(p.user, "root") && p.port == 2222);
    CHECK(ssh_config_lookup(CFG, strlen(CFG), "other", &p));
    CHECK(!strcmp(p.user, "everyone") && p.port == 22 && p.hostname[0] == 0);
}

TEST(patterns_negation_and_home) {
    ssh_profile_t p;
    CHECK(ssh_config_lookup(CFG, strlen(CFG), "db.lab", &p));
    CHECK(!strcmp(p.user, "labuser") && !strcmp(p.identity, "/home/.ssh/lab_key"));
    CHECK(ssh_config_lookup(CFG, strlen(CFG), "secret.lab", &p));
    CHECK(!strcmp(p.user, "everyone")); /* the negation took it out of the lab block */
}

TEST(no_match_without_a_star) {
    const char *c = "Host a\n User x\n";
    ssh_profile_t p;
    CHECK(!ssh_config_lookup(c, strlen(c), "b", &p));
    CHECK(p.user[0] == 0);
}

int main(void) {
    RUN(a_host_block_and_the_fallback);
    RUN(patterns_negation_and_home);
    RUN(no_match_without_a_star);
    return TEST_DONE();
}
