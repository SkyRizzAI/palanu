/* The catalogue's list (pal_store_parse): the header is required, good lines become entries with their checksum, bad lines are
 * skipped, and the summary may be missing or hold spaces. */
#include <string.h>

#include "nema/nema.h"
#include "pal/store.h"
#include "test.h"

#define SHA "520f6ebf642e584363a36116e2163d1ff9bd4a1e671e8454615139a8392a4fa8"

TEST(a_list_becomes_entries) {
    const char *t = "palanu-store 1\n"
                    "devices\t0.1.0\tDevices\t572\t" SHA "\tdevices.papp\tLists the board's devices\n"
                    "bad line without tabs\n"
                    "short\t1\tShort\t10\tdeadbeef\tx.papp\tchecksum too short\n"
                    "nosum\t2.0\tNo summary\t99\t" SHA "\thttps://example.com/n.papp\n";
    pal_store_entry_t e[8];
    int n = pal_store_parse(t, strlen(t), e, 8);
    CHECK_EQ(n, 2);
    CHECK(!strcmp(e[0].id, "devices") && !strcmp(e[0].version, "0.1.0") && e[0].size == 572);
    CHECK(!strcmp(e[0].summary, "Lists the board's devices"));
    CHECK(e[0].sha256[0] == 0x52 && e[0].sha256[31] == 0xa8);
    CHECK(!strcmp(e[1].path, "https://example.com/n.papp") && e[1].summary[0] == 0);
}

TEST(not_a_list_is_refused_and_max_is_kept) {
    pal_store_entry_t e[1];
    CHECK_EQ(pal_store_parse("<html>", 6, e, 1), -1);
    const char *t = "palanu-store 1\na\t1\tA\t1\t" SHA "\ta.papp\tx\nb\t1\tB\t1\t" SHA "\tb.papp\ty\n";
    CHECK_EQ(pal_store_parse(t, strlen(t), e, 1), 1);
    CHECK(!strcmp(e[0].id, "a"));
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(a_list_becomes_entries);
    RUN(not_a_list_is_refused_and_max_is_kept);
    return TEST_DONE();
}
