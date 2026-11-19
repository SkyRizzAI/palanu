#!/usr/bin/env bun
// Builds Web Forge into one self-contained file, forge/web/dist/web-forge.html: the script, its libraries (xterm) and the
// styles are inlined, so the page needs no server and no network. Open it straight from disk in Chrome or Edge (USB,
// Bluetooth, Wi-Fi/LAN), or serve the folder on http://localhost. An https host would block ws:// to the device.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const DIR = import.meta.dir;
const out = await Bun.build({ entrypoints: [join(DIR, "src/main.ts")], target: "browser", minify: true, format: "esm" });
if (!out.success) {
  for (const m of out.logs) console.error(m);
  process.exit(1);
}
const js = await out.outputs[0].text();
const xtermCss = readFileSync(join(DIR, "node_modules/@xterm/xterm/css/xterm.css"), "utf8");
const css = xtermCss + "\n" + readFileSync(join(DIR, "style.css"), "utf8");
/* a "</script>" inside the bundle would end the inline script early */
const safeJs = js.replace(/<\/script/gi, "<\\/script");
const html = readFileSync(join(DIR, "index.html"), "utf8").replace("/*__CSS__*/", () => css).replace("/*__JS__*/", () => safeJs);
mkdirSync(join(DIR, "dist"), { recursive: true });
const dest = join(DIR, "dist/web-forge.html");
writeFileSync(dest, html);
console.log(`wrote ${dest} (${(html.length / 1024).toFixed(0)} KB)`);

// ---- the simulator page: dist/sim/ holds the page and, when the Emscripten build exists, the system (palanu-web.js + .wasm)
import { copyFileSync, existsSync } from "node:fs";
const simOut = await Bun.build({ entrypoints: [join(DIR, "src/sim/main.ts")], target: "browser", minify: true, format: "esm" });
if (!simOut.success) {
  for (const m of simOut.logs) console.error(m);
  process.exit(1);
}
const simJs = (await simOut.outputs[0].text()).replace(/<\/script/gi, "<\\/script");
const simCss = readFileSync(join(DIR, "style.css"), "utf8") + "\n" + readFileSync(join(DIR, "sim.css"), "utf8");
const simHtml = readFileSync(join(DIR, "sim.html"), "utf8").replace("/*__CSS__*/", () => simCss).replace("/*__JS__*/", () => simJs);
mkdirSync(join(DIR, "dist/sim"), { recursive: true });
writeFileSync(join(DIR, "dist/sim/index.html"), simHtml);
copyFileSync(join(DIR, "coi-sw.js"), join(DIR, "dist/sim/coi-sw.js"));
const webBuild = join(DIR, "../../build/web");
for (const f of ["palanu-web.js", "palanu-web.wasm"]) {
  if (existsSync(join(webBuild, f))) copyFileSync(join(webBuild, f), join(DIR, "dist/sim", f));
  else console.warn(`simulator: ${join(webBuild, f)} missing; build it first (docs/SIMULATOR.md)`);
}
console.log(`wrote ${join(DIR, "dist/sim/index.html")} (${(simHtml.length / 1024).toFixed(0)} KB)`);

// ---- the install page: dist/flash/ holds the page and, when ./pal build made one, the firmware image and its manifest
const flashDir = join(DIR, "dist/flash");
mkdirSync(flashDir, { recursive: true });
const flashHtml = readFileSync(join(DIR, "flash.html"), "utf8").replace("/*__CSS__*/", () => readFileSync(join(DIR, "style.css"), "utf8"));
writeFileSync(join(flashDir, "index.html"), flashHtml);
const fw = join(DIR, "../../targets/hhv2/build");
if (existsSync(join(fw, "flasher_args.json"))) {
  const args = JSON.parse(readFileSync(join(fw, "flasher_args.json"), "utf8"));
  const parts: { path: string; offset: number }[] = [];
  for (const [off, file] of Object.entries(args.flash_files as Record<string, string>)) {
    const name = file.split("/").pop()!;
    copyFileSync(join(fw, file), join(flashDir, name));
    parts.push({ path: name, offset: Number(off) });
  }
  const desc = JSON.parse(readFileSync(join(fw, "project_description.json"), "utf8"));
  const manifest = {
    board: "hhv2", chip: args.extra_esptool_args?.chip ?? "esp32s31", version: desc.project_version ?? "", built: new Date().toISOString().slice(0, 10),
    flash_mode: args.flash_settings?.flash_mode ?? "keep", flash_freq: args.flash_settings?.flash_freq ?? "keep", flash_size: args.flash_settings?.flash_size ?? "keep",
    parts,
  };
  writeFileSync(join(flashDir, "manifest.json"), JSON.stringify(manifest, null, 2));
  console.log(`wrote ${flashDir} (firmware ${manifest.version}, ${parts.length} parts)`);
} else {
  console.log(`wrote ${flashDir} (no firmware image: run ./pal build first)`);
}
