// Key sequences: one step per line, the same words as the native simulator's flags (palanu-sim --cmd, --key, --wait).
//   cmd LINE         type LINE and press Enter          type TEXT       type TEXT (no Enter)
//   key NAME...      named keys: enter esc tab backspace up down left right home end pgup pgdn delete switch home-app vkb fntap
//                    ctrl-X, alt-X                      wait MS         pause
//   tap ROW COL      press and release a physical key   battery PCT     set the charge (-1: no battery)
//   sensor I C       sensor I reads C degrees           gpio I 0|1      drive pin I
//   shot             save a screenshot                  restart         restart the system
//   note TEXT        put a line in the log              # comment
export interface Step { verb: string; arg: string; line: number }

export function parse(src: string): Step[] {
  const steps: Step[] = [];
  src.split("\n").forEach((raw, i) => {
    const t = raw.trim();
    if (!t || t.startsWith("#")) return;
    const sp = t.indexOf(" ");
    steps.push({ verb: (sp < 0 ? t : t.slice(0, sp)).toLowerCase(), arg: sp < 0 ? "" : t.slice(sp + 1), line: i + 1 });
  });
  return steps;
}

export interface Host {
  cmd(line: string): Promise<void>; type(text: string): Promise<void>; key(name: string): boolean; tap(row: number, col: number): Promise<void>;
  battery(p: number): void; sensor(i: number, c: number): void; gpio(i: number, l: number): void;
  shot(): void; restart(): void; note(text: string): void;
}

const sleep = (ms: number) => new Promise<void>((ok) => setTimeout(ok, ms));

/** Runs one step; throws with the line number for a step that cannot run. */
export async function runStep(h: Host, s: Step, stopped: () => boolean): Promise<void> {
  const nums = s.arg.split(/\s+/).filter(Boolean).map(Number);
  const fail = (why: string) => { throw new Error(`line ${s.line}: ${why}`); };
  switch (s.verb) {
    case "cmd": await h.cmd(s.arg); await sleep(80); break;
    case "type": await h.type(s.arg); await sleep(40); break;
    case "key": for (const k of s.arg.split(/\s+/).filter(Boolean)) { if (!h.key(k)) fail(`unknown key "${k}"`); await sleep(60); } break;
    case "wait": { const ms = nums[0]; if (!(ms >= 0)) fail("wait needs milliseconds"); for (let t = 0; t < ms && !stopped(); t += 100) await sleep(Math.min(100, ms - t)); break; }
    case "tap": if (nums.length < 2 || nums.some((n) => !Number.isInteger(n))) fail("tap needs ROW COL"); await h.tap(nums[0], nums[1]); break;
    case "battery": if (nums.length < 1 || Number.isNaN(nums[0])) fail("battery needs a percent"); h.battery(nums[0]); break;
    case "sensor": if (nums.length < 2 || nums.some(Number.isNaN)) fail("sensor needs INDEX DEGREES"); h.sensor(nums[0], nums[1]); break;
    case "gpio": if (nums.length < 2 || nums.some(Number.isNaN)) fail("gpio needs INDEX LEVEL"); h.gpio(nums[0], nums[1]); break;
    case "shot": h.shot(); break;
    case "restart": h.restart(); break;
    case "note": h.note(s.arg); break;
    default: fail(`unknown step "${s.verb}"`);
  }
}

export async function run(h: Host, src: string, repeat: number, stopped: () => boolean, onStep: (s: Step) => void): Promise<void> {
  const steps = parse(src);
  for (let n = 0; n < repeat && !stopped(); n++)
    for (const s of steps) {
      if (stopped()) return;
      onStep(s);
      if (stopped()) return;
      await runStep(h, s, stopped);
    }
}
