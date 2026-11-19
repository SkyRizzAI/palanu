/* The release keys compiled into the firmware (system/src/trust.c, docs/RELEASE.md): Ed25519 public keys whose signatures a device
 * trusts without anything in /etc/trust, for packages and for the catalogue (index.txt.sig, ADR-0016). Empty until the owner makes
 * the release key: `bun forge/tools/palanu.ts keygen release.key` prints the line to put here; the .key file never enters the
 * repository. A key here is trusted by every device built from this tree, so only the project's release key belongs here. */
#ifndef PAL_RELEASE_KEYS_H
#define PAL_RELEASE_KEYS_H

#define PAL_RELEASE_KEYS /* {0x.., 0x.., ... 32 bytes}, one line per key */

#endif
