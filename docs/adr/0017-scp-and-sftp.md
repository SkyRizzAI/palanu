# ADR-0017: scp and sftp from the device

Status: accepted (19 November 2026).

## Context
SSH from the device reaches a server's shell, but getting a file there or back meant Palanu Link and a computer. A pocket terminal
without file transfer is half a tool: every SSH client on a computer has scp and sftp next to it. The board's libssh component
leaves libssh's SFTP part out of its build.

## Decision
- **libssh's own SFTP client**, not a new one: `sftp.c`, `sftp_common.c` and `sftp_aio.c` from the component's source are compiled
  with `WITH_SFTP` into a static library of their own (`libssh_sftp`, apps/CMakeLists.txt), against the component's headers. The
  component is not edited (it is managed and re-downloaded), and libssh's code stays in a library apart from ours (LGPL-2.1).
  The host build uses libssh's default build, which has SFTP.
- **Two commands on the ssh connection.** `ssh_open` (apps/cmd_ssh.c, `ssh_session.h`) is the ssh command's connect, host key
  check and authentication, now shared: `scp` and `sftp` take the same keys, `known_hosts` and `/home/.ssh/config` profiles.
  `scp [-P port] [-i key] SRC DST` copies one file either way (one side is `[user@]host:path`; a folder as the destination takes the
  file's name). `sftp [-P port] [-i key] [user@]host` is the interactive client: ls, cd, pwd, get, put, mkdir, rmdir, rm, rename,
  lcd, lpwd. The transfer is SFTP (as OpenSSH's scp has used by default since 9.0), in 8 KB pieces.
- Built where libssh has SFTP (`PAL_HAVE_SFTP`): the board, and a host simulator built with libssh.

## Consequences
- The firmware grows by about 21 KB (84.3% of the update slot); internal RAM is unchanged.
- Proven by `forge/tests/sftp_e2e.ts` against OpenSSH on the computer (upload, download into a folder, 200 KB byte for byte, a
  missing file, and the sftp commands); not yet from the board over Wi-Fi.
