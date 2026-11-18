# ADR-0009: Desktop environments are replaceable, and one of them is drawn with LVGL

Status: accepted.

## Context
The compositor already has a home view that can be replaced (`pal_desktop_set_home`), a launcher with four skins and themes with replaceable components, but nothing
that treats "the desktop" as a unit a user can pick: home keys, the home view and the app listing are wired in one place for one look. We want several desktops, the
way a Linux system has several desktop environments, and installed apps that carry what a desktop needs (name, icon, category, resources). We also want to
try LVGL, the widely used embedded GUI library, for a desktop without making the board pay for it.

## Decision
- A registry of desktop environments (`pal/desktops.h`). A desktop has an id, a start and an optional stop; exactly one is active; the choice is the `desktop.env` setting; the
  shell command `desktop` lists and switches. The compositor stays the same for every desktop. The desktop with the id `classic` is the fallback.
- The current look becomes the `classic` desktop, unchanged: the golden images of every existing scene are the same.
- Athena is a second desktop whose home is drawn with LVGL 9.2.2 (MIT, vendored in `third_party/lvgl`). It is built only into the simulators. LVGL renders into its own RGB565 frame and the
  view copies it into the 8-bit canvas, dithering and snapping colours to the palette's colour cube. Its allocator is a static pool; its type is Montserrat (SIL OFL).
- The app registry already carries name, version, icon id and category; the `.papp` format has icon and resource sections. Desktops read the registry. Reading the icon section and
  shortcut entries are later work and need no change to the format.

## Consequences
- A new desktop is one file and one `pal_desktops_register` call; the firmware size is unchanged because Athena is not linked into it.
- LVGL adds 18 MB of vendored sources and a build of about 150 files to the simulators. If it is ever put on a board it needs its own RAM budget and an ADR.
- Two screens exist that draw home differently, so a change to the launcher contract (`psys_launcher_get`, `pal_launcher_run`) must keep both working.
