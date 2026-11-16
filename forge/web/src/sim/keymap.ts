// The hhv2 keyboard as a picture: 4 rows by 10 columns, the layers of boards/hhv2/hhv2_keyboard.c (docs/KEYMAP.md).
// Pressing a cap sends the physical key to the OS (pal_input_hw), so Fn, Nm, Shift and the long press of the cycle key work
// exactly as on the board. The matrix columns run right to left: column 9 is the leftmost key.
export interface Cap { base: string; sym?: string; fn?: string; mod?: boolean; wide?: number }

const c = (base: string, sym?: string, fn?: string): Cap => ({ base, sym, fn });
const m = (base: string, wide?: number): Cap => ({ base, mod: true, wide });

/** By matrix column 0..9 of each row (the drawing reverses them). */
export const ROWS: Cap[][] = [
  [c("P", "@"), c("O", "+"), c("I", "-"), c("U", "_"), c("Y", ")"), c("T", "("), c("R", "3", "PgUp"), c("E", "2", "End"), c("W", "1", "↑"), c("Q", "#", "Home")],
  [c("DEL", undefined, "Del"), c("L", '"'), c("K", "'"), c("J", ";"), c("H", ":"), c("G", "/"), c("F", "6", "PgDn"), c("D", "5", "→"), c("S", "4", "↓"), c("A", "*", "←")],
  [c("↵"), c(".", "$"), c("M", ","), c("N", "!"), c("B", "?"), c("V", "%"), c("C", "9"), c("X", "8"), c("Z", "7"), m("Shift")],
  [m("Alt"), m("Shift"), m("Ctrl"), c("Tab"), c("␣", "0"), { base: "" }, m("Cycle"), m("Nm"), m("Fn"), c("Esc")],
];

/** The caps that stay down when sticky modifiers are on. */
export const STICKY = new Set(["Alt", "Shift", "Ctrl", "Nm", "Fn"]);

export interface KeyboardHooks { down(row: number, col: number): void; up(row: number, col: number): void; sticky(): boolean }

export function buildKeyboard(root: HTMLElement, hooks: KeyboardHooks) {
  root.textContent = "";
  ROWS.forEach((row, r) => {
    const line = document.createElement("div");
    line.className = "kb-row";
    for (let col = 9; col >= 0; col--) {
      const cap = row[col];
      const b = document.createElement("button");
      b.type = "button";
      b.className = "cap" + (cap.mod ? " mod" : "") + (cap.base ? "" : " none");
      b.disabled = !cap.base;
      if (cap.base) {
        b.title = [cap.base, cap.sym ? `Nm: ${cap.sym}` : "", cap.fn ? `Fn: ${cap.fn}` : ""].filter(Boolean).join("  ·  ");
        b.innerHTML = `<i>${esc(cap.sym ?? "")}</i><b>${esc(cap.base)}</b><u>${esc(cap.fn ?? "")}</u>`;
      }
      let held = false;
      const release = () => { if (held) { held = false; b.classList.remove("down"); hooks.up(r, col); } };
      b.addEventListener("pointerdown", (ev) => {
        ev.preventDefault();
        if (held && STICKY.has(cap.base) && hooks.sticky()) return release();
        held = true;
        b.classList.add("down");
        hooks.down(r, col);
        if (!(STICKY.has(cap.base) && hooks.sticky())) {
          const up = () => { removeEventListener("pointerup", up); removeEventListener("pointercancel", up); release(); };
          addEventListener("pointerup", up);
          addEventListener("pointercancel", up);
        }
      });
      line.append(b);
    }
    root.append(line);
  });
}

const esc = (s: string) => s.replace(/[&<>"]/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[ch]!));
