# ADR-0012: Every built-in app has a look per desktop: the app describes, the session presents

Status: accepted (19 November 2026). Builds on ADR-0010 §8 (apps are tty, form or canvas).

## Context
Under Athena, Files, Logs and the text viewer were still drawn the classic way (1-bit kui lists and mono text in the 8-bit canvas), and
the Terminal kept the classic theme's colours. The owner asked for each of these apps to have two looks: the classic desktop's and
Athena's. Writing `if (desktop == athena)` in each app would tie every app to every desktop, and a third desktop would touch them all.

## Decision
An app describes its screen as data and keeps its state and keys; the running session presents it. The classic session keeps calling
the view's draw, so the classic look stays exactly as it was. A session that draws forms and documents itself (Athena) reads the
description instead.

- **Lists are forms** (`pal_menu_bind`). Files binds its listing: one row per entry (a folder is a submenu row, a file an action row
  with its size), the breadcrumb as the title. Two pages take turns, so a new listing is a new page to the session; the selection is
  the app's, copied in. Athena draws a long page as a window of 40 rows around the selection.
- **Reading screens are documents** (new `pal/doc.h`, `pal_doc_bind`): a title, up to three toolbar rows (a switch or a value), and
  lines from a callback, with pointers to the app's first line and toolbar focus. The presenter writes back how many lines fit, so
  the app's paging follows the look on screen. Logs and the text viewer bind one; Athena draws the lines on a card, fixed-width
  (unscii 8) for a file or a hex dump.
- **Terminals stay tty**; the session picks their colours. `pal_desktop_chrome_t` gains `tty_colors`, `tty_bg`, `tty_fg`, applied
  over the theme's default terminal colours in the palette (`pal_theme_term_override`). Each Athena theme has its own pair.
- **Menus the app opens are shell services.** The Files operations menu describes itself as a picker (`pal/shell.h`), so Athena shows it
  as its modal; under classic it draws itself as before.
- On a theme with Options (Softkey, Trackball), Options gains **App menu**, which sends the app its Tab (the app's own menu key), and
  Select and Back send Enter and Esc to the screen in front.

The document API is internal to the system and the built-in apps for now; it can join the app API later with `PAL_SINCE`.

## Consequences
- No app tests which desktop runs. A new desktop gets every built-in app by drawing forms, documents and shell services.
- Over an app, Athena's bar names the app in front.
- Not covered yet: Files' rename and new-item entry already go through the virtual keyboard service; the text viewer's line numbers
  are drawn as text in the document, not a separate gutter.
