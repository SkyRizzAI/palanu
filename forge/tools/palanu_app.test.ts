// Tests for palanu app projects (T2.8): the app.toml subset, templates, new/build, and the deploy --run shell runner.
import { describe, expect, test } from "bun:test";
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { packPapp } from "../packages/papp/src/index.ts";
import { appSpecFrom, buildProject, exitCodeIn, findWasmLd, manifestOf, newProject, pappId, parseToml, readAppSpec, runOnShell, templateFiles, type ShellLink } from "./palanu_app.ts";

const TOOL = join(import.meta.dir, "palanu.ts");
const dirs: string[] = [];
function tmp(): string {
  const d = mkdtempSync(join(tmpdir(), "palanu-test-"));
  dirs.push(d);
  return d;
}
function haveWasmLd(): boolean {
  try {
    findWasmLd();
    return Bun.spawnSync(["which", "clang"]).exitCode === 0;
  } catch {
    return false;
  }
}

describe("parseToml", () => {
  test("strings with escapes and literal strings, integers, booleans", () => {
    const t = parseToml(`a = "x\\"y\\n\\t\\u00e9"\nb = 'lit # not a comment'\nc = 1_000\nd = 0x1F\ne = -3\nf = true\ng = "# inside"  # real comment`);
    expect(t.a).toBe('x"y\n\t\u00e9');
    expect(t.b).toBe("lit # not a comment");
    expect(t.c).toBe(1000);
    expect(t.d).toBe(31);
    expect(t.e).toBe(-3);
    expect(t.f).toBe(true);
    expect(t.g).toBe("# inside");
  });

  test("arrays of strings may span lines and end with a comma", () => {
    const t = parseToml(`caps = [\n  "net", # the socket client\n  "fs.home",\n]\nempty = []`);
    expect(t.caps).toEqual(["net", "fs.home"]);
    expect(t.empty).toEqual([]);
  });

  test("tables are parsed; dotted names nest", () => {
    const t = parseToml(`id = "x"\n[service]\nrestart = "on-failure"\n[launcher.look]\nhidden = false`);
    expect(t.id).toBe("x");
    expect(t.service).toEqual({ restart: "on-failure" });
    expect(t.launcher).toEqual({ look: { hidden: false } });
  });

  test("errors name the line", () => {
    expect(() => parseToml(`a = 1\nb = 2.5`)).toThrow(/line 2: unsupported value 2\.5/);
    expect(() => parseToml(`a = 1\na = 2`)).toThrow(/line 2: duplicate key a/);
    expect(() => parseToml(`x = [1,\n2`)).toThrow(/array is not closed/);
    expect(() => parseToml(`[[apps]]`)).toThrow(/line 1: arrays of tables/);
    expect(() => parseToml(`just words`)).toThrow(/line 1: expected key = value/);
    expect(() => parseToml(`s = "open`)).toThrow(/unterminated string/);
    expect(() => parseToml(`s = "bad \\q"`)).toThrow(/unsupported escape/);
    expect(() => parseToml(`k = {a = 1}`)).toThrow(/unsupported value/);
  });
});

describe("appSpecFrom", () => {
  test("defaults and mapping", () => {
    const spec = appSpecFrom(parseToml(`id = "demo"\ncaps = ["net"]\nmem_kb = 128\nstack_kb = 12\nfuel_m = 5`));
    expect(spec).toMatchObject({ id: "demo", name: "demo", version: "0.1.0", api: 2, kind: "tty", sources: ["main.c"], caps: ["net"], mem_kb: 128, stack_kb: 12, fuel_m: 5 });
    expect(manifestOf(spec)).toMatchObject({ id: "demo", name: "demo", ver: "0.1.0", api: 2, perm: ["net"], mem: 128, stack: 12, fuel: 5 });
  });

  test("kind goes into the manifest; a missing fuel stays out", () => {
    const spec = appSpecFrom(parseToml(`id = "svc"\nkind = "service"`));
    expect(spec.kind).toBe("service");
    expect(manifestOf(spec)).toMatchObject({ kind: "service" });
    expect(manifestOf(spec).fuel).toBeUndefined();
  });

  test("rejects bad values with the key named", () => {
    expect(() => appSpecFrom(parseToml(`name = "x"`))).toThrow(/id is required/);
    expect(() => appSpecFrom(parseToml(`id = "Bad Name"`))).toThrow(/id must be/);
    expect(() => appSpecFrom(parseToml(`id = "a"\nkind = "desktop"`))).toThrow(/kind must be one of tty, gui, service/);
    expect(() => appSpecFrom(parseToml(`id = "a"\nmem_kb = -1`))).toThrow(/mem_kb/);
    expect(() => appSpecFrom(parseToml(`id = "a"\nmem_kb = "256"`))).toThrow(/mem_kb/);
    expect(() => appSpecFrom(parseToml(`id = "a"\ncaps = "net"`))).toThrow(/caps must be an array/);
    expect(() => appSpecFrom(parseToml(`id = "a"\nsources = ["../x.c"]`))).toThrow(/inside the project/);
    expect(() => appSpecFrom(parseToml(`id = "a"\nsources = ["/abs.c"]`))).toThrow(/inside the project/);
    expect(() => appSpecFrom(parseToml(`id = "a"\nsources = ["x.o"]`))).toThrow(/must be a .c file/);
  });

  test("unknown top-level keys are reported, tables are not", () => {
    const spec = appSpecFrom(parseToml(`id = "a"\nfoo = 1\n[service]\nx = 1`));
    expect(spec.unknown).toEqual(["foo"]);
  });

  test("readAppSpec explains a missing file", () => {
    expect(() => readAppSpec(join(tmp(), "app.toml"))).toThrow(/palanu new/);
  });
});

describe("templates and palanu new", () => {
  test("each kind has main.c, app.toml, README.md; app.toml parses back to the same kind", () => {
    for (const kind of ["tty", "gui", "service"] as const) {
      const files = templateFiles(kind, "demo-app", "Demo app");
      expect(Object.keys(files).sort()).toEqual(["README.md", "app.toml", "main.c"]);
      const spec = appSpecFrom(parseToml(files["app.toml"]));
      expect(spec).toMatchObject({ id: "demo-app", name: "Demo app", kind });
      expect(files["main.c"]).toContain("PALANU_LIBC_IMPLEMENTATION");
      expect(files["main.c"]).toContain("void _start(void)");
    }
    expect(templateFiles("tty", "a", "A")["main.c"]).toContain('#include "paltui/paltui.h"');
    expect(templateFiles("gui", "a", "A")["main.c"]).toContain("T5.2");
    expect(templateFiles("service", "a", "A")["main.c"]).toContain("PAL_EV_QUIT");
  });

  test("newProject writes the files; refuses a non-empty directory and bad names", () => {
    const d = tmp();
    const made = newProject("my-app", "service", d);
    expect(made).toBe(join(d, "my-app"));
    for (const f of ["main.c", "app.toml", "README.md"]) expect(existsSync(join(made, f))).toBe(true);
    expect(readFileSync(join(made, "app.toml"), "utf8")).toContain('kind = "service"');
    expect(() => newProject("my-app", "tty", d)).toThrow(/not empty/);
    expect(() => newProject("Bad Name", "tty", d)).toThrow(/also the app id/);
    expect(() => newProject("ok", "desktop", d)).toThrow(/--kind must be one of/);
    expect(readdirSync(d)).toEqual(["my-app"]);
  });

  test("palanu new through the CLI creates a project in the working directory", () => {
    const d = tmp();
    const r = Bun.spawnSync(["bun", TOOL, "new", "cli-app", "--kind", "gui"], { cwd: d });
    expect(r.exitCode).toBe(0);
    expect(readFileSync(join(d, "cli-app", "app.toml"), "utf8")).toContain('kind = "gui"');
  });
});

describe("build", () => {
  test.skipIf(!haveWasmLd())("the generated tty template builds to a .papp", async () => {
    const d = tmp();
    const dir = newProject("tty-demo", "tty", d);
    const built = await buildProject(dir);
    expect(built.path).toBe(join(dir, "tty-demo.papp"));
    const bytes = new Uint8Array(readFileSync(built.path));
    expect(String.fromCharCode(...bytes.slice(0, 4))).toBe("PLNU");
    expect(bytes.length).toBe(built.bytes);
  }, 60000);

  test.skipIf(!haveWasmLd())("every generated kind builds (gui and service too)", async () => {
    for (const kind of ["gui", "service"] as const) {
      const dir = newProject(`k-${kind}`, kind, tmp());
      const built = await buildProject(dir);
      expect(built.bytes).toBeGreaterThan(100);
    }
  }, 120000);

  test.skipIf(!haveWasmLd())("CLI: project build (no arguments) and the old flag form both work", () => {
    const d = tmp();
    const dir = newProject("cli-build", "service", d);
    const r1 = Bun.spawnSync(["bun", TOOL, "build"], { cwd: dir });
    expect(r1.exitCode).toBe(0);
    expect(existsSync(join(dir, "cli-build.papp"))).toBe(true);
    const src = join(d, "legacy.c");
    writeFileSync(src, "void _start(void) {}\n");
    const out = join(d, "legacy.papp");
    const r2 = Bun.spawnSync(["bun", TOOL, "build", src, "--id", "legacy", "-o", out]);
    expect(r2.exitCode).toBe(0);
    expect(existsSync(out)).toBe(true);
  }, 120000);

  test("CLI: project build without app.toml fails with a hint", () => {
    const r = Bun.spawnSync(["bun", TOOL, "build"], { cwd: tmp() });
    expect(r.exitCode).toBe(1);
    expect(new TextDecoder().decode(r.stderr)).toContain("palanu new");
  });
});

describe("pappId", () => {
  const wasm = Uint8Array.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);
  /* A format 1 package: header, CBOR manifest map, then the module (the id is all that is read). */
  const v1 = (manifest: number[]) => {
    const b = new Uint8Array(20 + manifest.length + wasm.length);
    b.set([0x50, 0x4c, 0x4e, 0x55, 1, 0], 0);
    new DataView(b.buffer).setUint16(6, 20, true);
    new DataView(b.buffer).setUint32(8, manifest.length, true);
    b.set(manifest, 20);
    b.set(wasm, 20 + manifest.length);
    return b;
  };

  test("reads the id from a format 2 package (deflated or stored manifest)", () => {
    expect(pappId(packPapp(wasm, { id: "note-2", name: "Note", ver: "1.2", perm: ["fs", "net"], mem: 300, fuel: 5 }))).toBe("note-2");
    expect(pappId(packPapp(wasm, { id: "a" }, { compress: false }))).toBe("a");
  });

  test("reads the id from a format 1 package", () => {
    // {"id": "old"}
    expect(pappId(v1([0xa1, 0x62, 0x69, 0x64, 0x63, 0x6f, 0x6c, 0x64]))).toBe("old");
  });

  test("rejects files that are not packages", () => {
    expect(() => pappId(new Uint8Array(30))).toThrow(/not a .papp file/);
    expect(() => pappId(v1([0x01, 0x62, 0x69, 0x64, 0x63, 0x6f, 0x6c, 0x64]))).toThrow(/bad .papp manifest/);
    expect(() => pappId(v1([0xa1, 0x62, 0x6e, 0x6d, 0x63, 0x6f, 0x6c, 0x64]))).toThrow(/no id/);
  });
});

describe("deploy --run shell runner", () => {
  test("exit mark: digits only, the echoed command line does not match", () => {
    expect(exitCodeIn("out\r\n__PLN_0_END\r\n")).toBe(0);
    expect(exitCodeIn("__PLN_127_END")).toBe(127);
    expect(exitCodeIn("__PLN_-1_END")).toBe(-1);
    expect(exitCodeIn("palanu:~$ hello; echo __PLN_$?_END")).toBeNull();
    expect(exitCodeIn("nothing yet")).toBeNull();
  });

  function fakeLink(reply: (typed: string) => string | null) {
    const typed: string[] = [];
    let closed = 0;
    const link: ShellLink & { closedCount: () => number } = {
      onTty: () => {},
      onTtyClose: () => {},
      openShell() {},
      ttyWrite(d: Uint8Array) {
        const s = new TextDecoder().decode(d);
        typed.push(s);
        const out = reply(s);
        if (out !== null) link.onTty(new TextEncoder().encode(out));
      },
      closeShell() {
        closed++;
      },
      closedCount: () => closed,
    };
    return { link, typed };
  }

  test("types the app name with the exit mark and resolves with the app's code", async () => {
    const out: string[] = [];
    const { link, typed } = fakeLink((s) => (s.includes("hello-tty;") ? "hi there\r\n__PLN_3_END\r\n" : null));
    const code = await runOnShell(link, "hello-tty", (d) => out.push(new TextDecoder().decode(d)), 80, 24, async () => {});
    expect(code).toBe(3);
    expect(typed[0]).toBe("hello-tty; echo __PLN_$?_END\n");
    expect(out.join("")).toContain("hi there");
    expect(link.closedCount()).toBe(1);
  });

  test("the exit mark and the prompt after it are not streamed, even when split across chunks", async () => {
    const out: string[] = [];
    const chunks = ["menu frame\r\n__PL", "N_1_E", "ND\r\npalanu:~$ "];
    const { link } = fakeLink((s) => (s.includes("app;") ? chunks.join("") : null));
    const code = await runOnShell(link, "app", (d) => out.push(new TextDecoder().decode(d)), 80, 24, async () => {});
    expect(code).toBe(1);
    expect(out.join("")).toBe("menu frame\r\n");
  });

  test("a line that only looks like the mark is streamed", async () => {
    const out: string[] = [];
    const { link } = fakeLink((s) => (s.includes("app;") ? "value __PLN_$?_END and __PLN_x\r\n__PLN_2_END" : null));
    const code = await runOnShell(link, "app", (d) => out.push(new TextDecoder().decode(d)), 80, 24, async () => {});
    expect(code).toBe(2);
    expect(out.join("")).toBe("value __PLN_$?_END and __PLN_x\r\n");
  });

  test("resolves -1 when the shell closes before the app finishes", async () => {
    const { link } = fakeLink(() => null);
    const pending = runOnShell(link, "spin", () => {}, 80, 24, async () => {});
    setTimeout(() => link.onTtyClose(), 5);
    expect(await pending).toBe(-1);
  });
});

describe("cleanup", () => {
  test("temp directories are removed", () => {
    for (const d of dirs) rmSync(d, { recursive: true, force: true });
    expect(dirs.every((d) => !existsSync(d))).toBe(true);
  });
});
