# Sources and code provenance

## Conditions for reusing code from `refs/`
Code from other projects is adopted only when it passes all five:
1. **It answers the reason for its part.** It satisfies the "why does this exist" of the part being built.
2. **It is better than writing it ourselves.** Faster to finish, tested, and not larger than needed.
3. **It does not bring wrong assumptions.** Free of Arduino, heavy C++, a 128x64 screen, six buttons. If more than half has to be torn apart, take only the idea.
4. **The license fits and is recorded.** This project is MIT (ADR-0004). Files from an Apache-2.0 project keep their original header. GPLv3 code does not come in.
5. **Understood and tested.** Read to the end, adjusted to the code rules, given a test.

## Adapted files
| File | Origin | Origin license | Conditions it passed |
|---|---|---|---|
| `system/src/gfx.c` (line, circle, rounded-box primitives) | written for this project (own work) | MIT (this project) | 1, 2, 3, 4, 5: already tested on the board, rewritten for the indexed canvas |

## Third-party libraries (brought in whole, not adapted)
| Library | Used for | License | Reason chosen |
|---|---|---|---|
| libssh 0.12.2 (component `david-cermak/libssh`) | SSH client (`apps/cmd_ssh.c`); its SFTP part (`sftp.c`, `sftp_common.c`, `sftp_aio.c`, unchanged) for scp and sftp (`apps/cmd_sftp.c`, ADR-0017), compiled as the separate library `libssh_sftp` | LGPL-2.1 | the only one that builds on IDF 6.2 with mbedTLS 4 and meets the needs of a full terminal (PTY, host keys, user keys). libssh2 was rejected: it does not build on the S31. Linked as a separate library, so the LGPL is satisfied. |
| esp_littlefs (`joltwallet/littlefs`) | persistent storage | MIT | power-loss resilience and wear leveling without our own code; small RAM |
| Spleen 5x8, 6x12, 8x16 | display fonts (`system/src/font_spleen_*.c`, generated with `forge/tools/bdf2c.ts`) | BSD-2-Clause | tidy bitmap fonts in three sizes, covering terminal glyphs |
| GNU Unifont 17.0.05 (BMP and emoji U+1F000-1FAFF) | terminal fallback glyphs (`assets/fonts/unifont/`, `system/src/font_fallback_data.c`, file `unifont-cjk.pfnt`; generated with `forge/tools/fontpack.ts`) | SIL OFL 1.1 and GPL with the font embedding exception (`assets/fonts/unifont/LICENSE.txt`, read) | Scripts, symbols and emoji that Spleen lacks: without it the terminal draws `?` |
| Symbols Nerd Font Mono (Nerd Fonts) | prompt and tool icons (`assets/fonts/nerd/`, converted to 8x16 by `forge/tools/nerd2hex.py`) | MIT for Nerd Fonts; each icon set inside carries its own license (Font Awesome CC BY 4.0, Octicons, Seti-UI, Devicons, Codicons, Weather Icons, Material Design Icons Apache-2.0, Font Logos) | Icons used by Powerline, eza, lsd, and starship prompts; attribution in `assets/fonts/nerd/LICENSE` and in this section |
| Helvetica bitmap fonts (`helvR08`, `helvB08`, `helvB10`, `helvR10`, `helvR12`, `helvB12`) | `system/src/kui/font_{reg8,bold8,reg10,bold10,reg12,bold12,primary}.c` (data unchanged) | Adobe/DEC permission (BDF COMMENT: 'Copyright 1984-1989, 1994 Adobe Systems Incorporated', 'Copyright 1988, 1994 Digital Equipment Corporation'; permissive: use, copy, modify and distribute without fee as long as the copyright and permission notice stay; the full text is in NOTICE, copied from the BDF source file; verified) | UI text, titles, legends |
| LVGL 9.2.2 (`third_party/lvgl`) | the Athena desktop (`system/src/athena/`), in the firmware and the simulators | MIT (`third_party/lvgl/LICENCE.txt`, read); its Montserrat fonts are SIL OFL 1.1 (see NOTICE) and its Unscii fonts are public domain | a complete, widely used embedded GUI library: drawing widgets and layout ourselves for a second desktop would cost more than vendoring it; its pool and frame live in PSRAM. Unmodified; see `third_party/README.md`. |
| 4x6 (`4x6.bdf`) | used as `FONT_TINY` (`font_tiny.c`) | Public domain (BDF COPYRIGHT: 'Public domain font. Share and enjoy.') | small font |
| ProFont 11 (`profont11.bdf`) | used as `FONT_MONO` (`font_mono.c`) | NOT VERIFIED: the BDF has no copyright line; the upstream license (ProFont, Andreas Frische) has not been checked against its license text | mono font |
| 6x8 and 5x8 (`font_6x8.c`, `font_5x8.c`) | original source not recorded; not verified | NOT VERIFIED | 6x8 mono font for footer legends |
| Lua 5.4.7 (`third_party/lua`) | the `lua` command (`apps/lua_rt.c`, `apps/cmd_lua.c`, ADR-0019): the core and the base, string, table, math, utf8 and coroutine libraries | MIT | small portable C, the scripting language many embedded devices use; MicroPython needs far more room. The official archive `https://www.lua.org/ftp/lua-5.4.7.tar.gz` (SHA-256 `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`), `src/` copied without `lua.c` and `luac.c`; no changes. |
| Monocypher 4.0.2 (`third_party/monocypher`) | crypto primitives in both builds: ChaCha20, Poly1305, ChaCha20-Poly1305 (RFC 8439), X25519 (RFC 7748), Ed25519 (RFC 8032), and HMAC through `system/src/crypto_mac.c` (T2.6) | BSD-2-Clause or CC0-1.0 (BSD-2-Clause chosen, compatible with MIT) | one dependency-free, constant-time C file, and no chip accelerator exists for these primitives; the SHA-512 in Ed25519 comes along because the `optional` file contains its own. Tested with RFC vectors and a reference library. No changes to its code. |
| miniz 3.1.0, `tinfl` only (`third_party/miniz`) | DEFLATE decompression for `.papp` format 2 sections (`system/src/papp.c`, T2.10) | MIT (the `LICENSE` file in that folder, from the miniz repo tag 3.1.0; `tinfl.c` and `miniz_common.h`, `miniz_tinfl.h` unchanged; `miniz.h` and `miniz_export.h` rewritten minimally) | the only inflater that is small and heap-free (`MINIZ_NO_MALLOC`); used on host and ESP, not dependent on ROM. There is no compressor; package writing is done by Forge through `node:zlib`. |
| WAMR 2.4.3 (`third_party/wamr`) | WebAssembly runtime for apps (`system/src/wasm_rt.c`) | Apache-2.0 with LLVM exception | makes installed apps isolated and able to run in the simulator with the same binary; four patches are recorded in `third_party/README.md` |

## Font packs (Appearances > Font, `ui.font`)
The glyph data is carried as BMF files in `assets/fonts/<Pack>/`, unchanged, and then converted into C tables by `forge/tools/bmf2c.ts` into `system/src/kui/font_pack_*.c`. Each pack fills six slots (reg/bold 8, 10, 12).

| Pack | Source | License | Status |
|---|---|---|---|
| Haxrcorp4089 | u8g2 font `haxrcorp4089`, converted with `u8g2_to_bmf.py` | NOT VERIFIED | NOT VERIFIED |
| ProFont | `profont*.bdf` (from u8g2 `profont11/22`, Andreas Frische) | NOT VERIFIED: the BDF has no copyright line | NOT VERIFIED |
