# hhv2 keyboard map (TCA8418, 4x10 matrix)

Measured on 12 November 2026 with this program. All 39 keys were pressed in order from left to right, top row to bottom. The result: 39 press events and 39 release events, paired, with no ghosting and no warnings. PBINT (GPIO7) was seen to go LOW whenever there was an event.

The C code is in [`../components/hhv2_keymap/include/hhv2_keymap.h`](../components/hhv2_keymap/include/hhv2_keymap.h).

## Physical layout → matrix position

The matrix columns are **reversed**: the leftmost physical key is at C9, the rightmost at C0.

| Physical row | C9 | C8 | C7 | C6 | C5 | C4 | C3 | C2 | C1 | C0 |
|---|---|---|---|---|---|---|---|---|---|---|
| R0 | Q | W | E | R | T | Y | U | I | O | P |
| R1 | A | S | D | F | G | H | J | K | L | del |
| R2 | ⇧ shift | Z | X | C | V | B | N | M | `.` | enter |
| R3 | esc | fn | Nm | ⟳ | *(empty)* | space | tab | ctrl | shift | alt |

TCA8418 event code = `row*10 + col + 1` (example: Q = R0 C9 = code 10).

## Secondary symbols (from the PCB silkscreen)

Read from a photo of the board, for the symbol/number layer later:

| Key | Secondary | Key | Secondary | Key | Secondary |
|---|---|---|---|---|---|
| Q | `#` | A | `*` | Z | `7` |
| W | `1` | S | `4` | X | `8` |
| E | `2` | D | `5` | C | `9` |
| R | `3` | F | `6` | V | `%` |
| T | `(` | G | `/` | B | `?` |
| Y | `)` | H | `:` | N | `!` |
| U | `_` | J | `;` | M | `,` |
| I | `-` | K | `'` | `.` | `$` |
| O | ? | L | `"` | Nm | `0` |
| P | ? | | | | |

The secondary symbols of O and P are not readable in the photo and need to be checked on the board itself. The function of the ⟳ key is also not decided yet.
