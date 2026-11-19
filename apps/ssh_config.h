/* SSH profiles (ssh_config.c): the Host blocks of /home/.ssh/config, as OpenSSH reads them: Host (names and * patterns), HostName,
 * User, Port and IdentityFile. The first value found for a key wins, so a block for one host before a `Host *` block sets that
 * host's values first. Pure, so tests/unit/test_ssh_config.c checks it. */
#ifndef PAL_APPS_SSH_CONFIG_H
#define PAL_APPS_SSH_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char hostname[96], user[40], identity[96];
    int port; /* 0 when no block sets it */
} ssh_profile_t;

/* Fills what the config gives for alias (fields it does not set stay empty or 0). True when at least one block matched. */
bool ssh_config_lookup(const char *text, size_t len, const char *alias, ssh_profile_t *out);
/* The same from /home/.ssh/config; false when there is no such file or no block matched. */
bool ssh_config_load(const char *alias, ssh_profile_t *out);

#endif
