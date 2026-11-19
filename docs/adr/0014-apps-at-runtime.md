# ADR-0014: Permissions at first use, events for apps, and the log on flash

Status: accepted (19 November 2026).

## Context
The gap analysis found three things an app platform needs and PalanuOS lacked: permissions were granted wholesale at install (a
signed package that asked for the network got it with no question to the user); the event queue reserved KEY, FOCUS, BLUR,
NOTIFY, SETTING and EXPOSE but produced none of them; and the log lived only in a 16 KB ring in memory, so a crash took the
evidence with it.

## Decision
- **Permissions at first use.** The install policy (signature or developer mode) stays. On top of it, the sensitive permissions
  (`net`, `net.listen`, `fs.home`, `fs.media`, `proc.spawn`, `crypto.keys`, `settings.write`) are asked the first time an installed
  app uses one, through the shell dialog (every session draws it). The answer is kept per app in `/data/perms`; a run caches it in
  its context so a call costs a bit test. Settings > Apps > the app changes or resets them. Listing the roots of `/` never asks.
  Where nobody can be asked (no desktop running, a host test) the manifest decides, as before, and nothing is kept: a headless
  board behaves as it did, and a screen asks the next time.
- **Events for apps.** Each event queue has a ring of 8 posted events and the queues are listed, so the system posts from any task
  without the owner's handle table: FOCUS, BLUR and EXPOSE when the app's screen comes to the front, leaves, or shows again; KEY
  for a key on its canvas; NOTIFY (the id) and SETTING (the FNV-1a hash of the key) to every app. The event format did not change,
  so the API level did not either.
- **The log on flash.** The kernel log gained a reader by position (`nema_log_total`, `nema_log_read`, which reports what the ring
  dropped). The `log-file` service appends to `/data/log/system.log` in batches (every 10 s or 2 KB) and rotates at 128 KB to
  `system.log.1`; `log.file` turns it off.

## Consequences
- An app must expect `PAL_ERR_PERM` from a call its manifest allows.
- Tests: `test_perms`, `test_app_events`, `test_log_read`. The question was shown in the native simulator with a real package
  (`net-client`); the board path compiles and has not run on hhv2.
