import { expect, test } from "bun:test";
import { parse, run, type Host } from "./script.ts";

test("parse keeps steps in order and skips comments and blanks", () => {
  const steps = parse("# hello\n\nkey up up enter\n  cmd ls -l \nwait 100\n");
  expect(steps.map((s) => s.verb)).toEqual(["key", "cmd", "wait"]);
  expect(steps[0].arg).toBe("up up enter");
  expect(steps[1].arg).toBe("ls -l");
  expect(steps[1].line).toBe(5 - 1);
});

function fake() {
  const log: string[] = [];
  const h: Host = {
    cmd: async (l) => { log.push(`cmd:${l}`); },
    type: async (t) => { log.push(`type:${t}`); },
    key: (n) => { log.push(`key:${n}`); return n !== "bogus"; },
    tap: async (r, c) => { log.push(`tap:${r},${c}`); },
    battery: (p) => { log.push(`battery:${p}`); },
    sensor: (i, c) => { log.push(`sensor:${i},${c}`); },
    gpio: (i, l) => { log.push(`gpio:${i},${l}`); },
    shot: () => { log.push("shot"); },
    restart: () => { log.push("restart"); },
    note: (t) => { log.push(`note:${t}`); },
  };
  return { h, log };
}

test("run executes every verb and repeats", async () => {
  const { h, log } = fake();
  await run(h, "cmd ls\nkey enter esc\ntap 0 9\nbattery 15\nsensor 0 30.5\ngpio 1 1\nnote x\nshot", 2, () => false, () => {});
  expect(log.length).toBe(2 * 9);
  expect(log.slice(0, 4)).toEqual(["cmd:ls", "key:enter", "key:esc", "tap:0,9"]);
});

test("run reports the line of a step that cannot run", async () => {
  const { h } = fake();
  await expect(run(h, "cmd ok\nkey bogus", 1, () => false, () => {})).rejects.toThrow("line 2");
  await expect(run(h, "wait soon", 1, () => false, () => {})).rejects.toThrow("line 1");
  await expect(run(h, "dance", 1, () => false, () => {})).rejects.toThrow('unknown step "dance"');
});

test("run stops when asked", async () => {
  const { h, log } = fake();
  let stop = false;
  await run(h, "cmd a\ncmd b\ncmd c", 5, () => stop, (s) => { if (s.line === 2) stop = true; });
  expect(log).toEqual(["cmd:a"]);
});
