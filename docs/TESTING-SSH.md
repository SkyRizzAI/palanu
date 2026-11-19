# Testing the SSH client on a computer

The SSH client (`apps/cmd_ssh.c`) uses libssh. On the board it is built with the `david-cermak/libssh` component (mbedTLS). To
test it without a board and without Wi-Fi, the simulator can be built with libssh from the same source using OpenSSL:

```bash
L=targets/hhv2/managed_components/david-cermak__libssh/libssh-mirror   # exists after the first `./pal build`
cmake -G Ninja -S $L -B /tmp/libssh-host -DCMAKE_BUILD_TYPE=Release -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF \
  -DCLIENT_TESTING=OFF -DWITH_SERVER=OFF -DWITH_GSSAPI=OFF -DBUILD_SHARED_LIBS=OFF
ninja -C /tmp/libssh-host
cmake build/host -DPAL_LIBSSH_BUILD=/tmp/libssh-host -DPAL_LIBSSH_SRC=$PWD/$L && ninja -C build/host
bun forge/tests/ssh_e2e.ts
```
scp and sftp (ADR-0017) are tested the same way by `forge/tests/sftp_e2e.ts`, against a simulator built with libssh in its own
folder (the default host build stays without it):
```bash
cmake -S platform/host -B build/host-ssh -G Ninja -DPAL_LIBSSH_BUILD=/tmp/libssh-host -DPAL_LIBSSH_SRC=$PWD/$L
ninja -C build/host-ssh palanu-sim && bun forge/tests/sftp_e2e.ts
```
It runs `sshd` with `internal-sftp` on `127.0.0.1:2223` and checks an upload, a download into a folder, 200 KB byte for byte, a
missing remote file, and sftp's mkdir, put, ls, rename, get, rm and rmdir.
The test runs a temporary user-owned `sshd` on `127.0.0.1:2222`, creates a key in the simulator with `ssh-keygen`,
and then runs `ssh` to it: the host key is prompted for, public key authentication succeeds, the remote shell sees the terminal size,
a resize reaches it, and `exit` returns to the local shell.

Proven this way (OpenSSH on macOS): public key authentication, remote shell with a PTY, window size and resize,
session close. Password authentication was tested against the libssh example server. What this test cannot reach
is the board network layer (lwIP over Wi-Fi) and the mbedTLS backend, which will only be proven once the board is connected.

## Testing the SSH app (`sdk/examples/ssh`, library `sdk/palssh`)

An SSH client written with the public API only (T4.1 to T4.3). The library is in `sdk/palssh/` (a single header with
`.inc` parts), the app is in `sdk/examples/ssh/`. Testing it does not need a board:

- **Host test** `tests/unit/test_palssh.c` (CTest, `./pal test`): base64, fingerprints, wire format, known_hosts
  (plain, `[host]:port`, hashed, wildcard, negation, `@revoked`, different key types), exchange hash and key derivation
  against vectors from `tests/unit/tools/palssh_vectors.py` (only `hashlib` and `hmac`), all three packet ciphers
  (both directions, with byte tampering and lengths above the limit), and a full handshake against an in-memory server
  that speaks as an RFC 4253 peer. Its backend uses the same system primitives as the simulator.
- **E2E** `forge/tests/ssh_app_e2e.ts`: the app is built with `palanu build`, installed in the host simulator, and run
  from the simulator shell to a temporary OpenSSH `sshd` on `127.0.0.1:2222` (non-root, public key authentication,
  `StrictModes no`, ed25519 host key). Run it from the repo root:

```bash
cmake --build build/host --target palanu-sim     # simulator (once, or after changes)
bun forge/tests/ssh_app_e2e.ts
```

The key is created in the simulator keystore (`ssh keygen`), and its public key is written to `authorized_keys` with `ssh pub`. The test
checks: `exec` (`echo hello; uname`) with status 0; the fingerprint prompt for a new host key; a PTY with a terminal size
of 30x100; a window-change to 40x120 when the terminal is resized; Ctrl-C reaching the server (`sleep 30`
is stopped, the next command runs); `exit 7` on the server producing status 7 in the local shell; `ssh keys`,
`ssh import`, and `ssh pub` (the imported key equals the original file); with no matching key: `Permission denied`
and status 255; `~.` at the start of a line disconnects the session with status 255; a changed host key is refused with a warning
and status 255. Two other ciphers are tested with a second sshd (ports 2223 and 2224) that offers only one cipher:
`aes256-gcm@openssh.com` and `chacha20-poly1305@openssh.com`, each with a successful `exec`.

Protocol note found through this test: OpenSSH aligns AEAD cipher packets (AES-GCM and
chacha20-poly1305) without counting the packet length field (`padding_length || payload || padding` is a multiple of the block size),
whereas with no cipher the length is counted (RFC 4253). The server rejects a wrong packet with `padding error`.

Notes on deviations from the original design:

- The app is installed with the id `sshapp` (the same source, `app.toml` with `id = "sshapp"` in a temporary copy). The shell
  recognizes the name `ssh` first, as the built-in command `apps/cmd_ssh.c` (libssh, removed in T4.5), so
  `/apps/ssh.papp` is shadowed by it.
- The simulator is run with `--link-open` (for tests only, see `docs/PLP.md`). Without it, writing a file through
  Link is rejected (`PAL_ERR_AUTH`), and `ssh.papp` cannot be installed from the test.
- `--wait` in the simulator is a script step (sleep), so it bounds the simulator's lifetime. Its value is 600000 ms.
- Password and keyboard-interactive authentication are not tested: a non-root sshd on macOS has no test password without
  PAM. The code is in `palssh_auth.inc`, not yet proven against a real server.

Transcript of one run (shortened):

```
ok   ssh keygen makes a key in the keystore
ok   ssh pub prints the authorized_keys line
ok   unknown host key is shown with its SHA256 fingerprint and asked
ok   exec command runs on the server (echo hello; uname)
     The authenticity of host '127.0.0.1 (127.0.0.1:2222)' can't be established.
     ED25519 key fingerprint is SHA256:<43 characters>.
     Are you sure you want to continue connecting (yes/no)? yes
     Warning: Permanently added '127.0.0.1' (ED25519) to the list of known hosts.
     hello
     Darwin
     __RC_0_RC
ok   exec returns the remote status (0)
ok   the accepted host is stored in known_hosts
     exec: connect, key exchange, auth and run took 103 ms after the host key was accepted (204 ms in all)
ok   interactive shell has a PTY of the terminal size (30 rows, 100 columns)
ok   window change reaches the server (40 rows, 120 columns)
ok   Ctrl-C reaches the server (sleep 30 is interrupted)
ok   remote exit closes the session and returns its status (7)
ok   ssh keys lists the keystore
ok   ssh import brings an OpenSSH ed25519 key into the keystore
ok   the imported key has the public key of the file
ok   no usable key: permission denied with status 255
ok   a second session starts
ok   ~. at the start of a line disconnects with status 255
ok   exec over aes256-gcm@openssh.com (the server offers only it)
ok   exec over chacha20-poly1305@openssh.com (the server offers only it)
ok   a changed host key is refused with a warning
ok   the refused connection exits 255
all checks passed
```

Sizes (measured with this test): `ssh.papp` 24,252 bytes (raw wasm 62,588 bytes, compressed per section). Initial linear memory
3 pages (192 KB) with a limit of `mem_kb = 256`. Inside it: two packet buffers of 35,104 bytes (`pssh_t` is 81,976 bytes
on the host), known_hosts 16 KB, and a 24 KB stack. The 160 KB target is exceeded by about 30 KB; the packet buffers are the largest
part. Handshake time: about 100 ms from accepting the host key to the command running, in the simulator (host, WAMR interpreter).
