# hhv2 keyboard

A 4x10 matrix (TCA8418), 39 keys, columns run from right to left (C9 is leftmost). The data is in `boards/hhv2/hhv2_keyboard.c`.

| Row | left → right |
|---|---|
| R0 | Q W E R T Y U I O P |
| R1 | A S D F G H J K L del |
| R2 | shift Z X C V B N M . enter |
| R3 | esc fn Nm ⟳ (empty) space tab ctrl shift alt |

## Layers
- **Base:** letters, `.`, space, named keys.
- **Nm:** the symbols printed on the keys. Hold Nm, or tap once for one following character. `Nm + space` = `0`.
  `Q #  W 1  E 2  R 3  T (  Y )  U _  I -  O +  P @  A *  S 4  D 5  F 6  G /  H :  J ;  K '  L "  Z 7  X 8  C 9  V %  B ?  N !  M ,  . $`
- **Nm + Shift:** the remaining ASCII symbols, placed on the top keys: `Q [  W ]  E {  R }  T <  Y >  U =  I |  O \  P ~  A &  S ^  D \``
- **Fn:** navigation. `W A S D` arrows, `Q` Home, `E` End, `R` PgUp, `F` PgDn, `del` Delete.
- **Fn twice quickly:** the input menu (on-screen keyboard, Symbols lock, Numbers, Caps Lock, one-key Ctrl); inside the menu a single tap of Fn
  moves down and Fn twice closes it (like Esc). **Shift twice quickly:** Caps Lock. Details in `VKEYBOARD.md`.
- **⟳ (cycle):** tap = switch app, hold = home.
- **Alt + N / W / Tab:** new terminal tab / close / next. **Shift + PgUp/PgDn** (Fn+R/F): scroll history.

## Not yet confirmed
The secondary symbol of the `P` key and the `0` function on the `Nm` key are presumed from photos; confirm on the board.
