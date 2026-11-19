/* Runtime permissions (pal/perms.h): asked once and remembered, a denial sticks, reset asks again, permissions that are not asked
 * pass, and with nobody to ask the manifest decides without keeping anything. */
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "pal/perms.h"
#include "test.h"

static int g_asked;
static bool g_answer;
static bool asker(const char *name, uint32_t bit) {
    (void)name, (void)bit;
    g_asked++;
    return g_answer;
}

TEST(asked_once_then_remembered) {
    psys_perm_set_asker(asker);
    psys_perm_reset("chat");
    g_asked = 0, g_answer = true;
    CHECK(psys_perm_check("chat", "Chat", PAL_PERM_NET));
    CHECK(psys_perm_check("chat", "Chat", PAL_PERM_NET));
    CHECK_EQ(g_asked, 1);
    CHECK_EQ(psys_perm_state("chat", PAL_PERM_NET), PAL_PERM_ALLOWED);
}

TEST(a_denial_sticks_until_reset) {
    psys_perm_set_asker(asker);
    psys_perm_reset("spy");
    g_asked = 0, g_answer = false;
    CHECK(!psys_perm_check("spy", "Spy", PAL_PERM_CRYPTO_KEYS));
    CHECK(!psys_perm_check("spy", "Spy", PAL_PERM_CRYPTO_KEYS));
    CHECK_EQ(g_asked, 1);
    CHECK_EQ(psys_perm_reset("spy"), NEMA_OK);
    CHECK_EQ(psys_perm_state("spy", PAL_PERM_CRYPTO_KEYS), PAL_PERM_ASK);
    g_answer = true;
    CHECK(psys_perm_check("spy", "Spy", PAL_PERM_CRYPTO_KEYS));
    CHECK_EQ(g_asked, 2);
}

TEST(settings_can_change_one_answer) {
    psys_perm_set_asker(asker);
    psys_perm_reset("tool");
    CHECK_EQ(psys_perm_set("tool", PAL_PERM_FS_HOME, PAL_PERM_DENIED), NEMA_OK);
    g_asked = 0;
    CHECK(!psys_perm_check("tool", "Tool", PAL_PERM_FS_HOME));
    CHECK_EQ(g_asked, 0);
    CHECK_EQ(psys_perm_set("tool", PAL_PERM_FS_HOME, PAL_PERM_ASK), NEMA_OK);
    CHECK_EQ(psys_perm_state("tool", PAL_PERM_FS_HOME), PAL_PERM_ASK);
}

TEST(permissions_that_are_not_asked_pass) {
    psys_perm_set_asker(asker);
    g_asked = 0;
    CHECK(psys_perm_check("any", "Any", PAL_PERM_NOTIFY));
    CHECK(psys_perm_check("any", "Any", PAL_PERM_FS));
    CHECK_EQ(g_asked, 0);
}

TEST(with_nobody_to_ask_the_manifest_decides) {
    psys_perm_set_asker(NULL); /* the dialog, which cannot open here: there is no desktop */
    psys_perm_reset("headless");
    CHECK(psys_perm_check("headless", "Headless", PAL_PERM_NET));
    CHECK_EQ(psys_perm_state("headless", PAL_PERM_NET), PAL_PERM_ASK); /* nothing kept: asked when a screen can ask */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(asked_once_then_remembered);
    RUN(a_denial_sticks_until_reset);
    RUN(settings_can_change_one_answer);
    RUN(permissions_that_are_not_asked_pass);
    RUN(with_nobody_to_ask_the_manifest_decides);
    return TEST_DONE();
}
