#!/usr/bin/env bun
// Builds Palanu Forge into one self-contained page, forge/web/dist/index.html (also web-forge.html): the script, its libraries
// (xterm), the styles and the device's pixel font are inlined, so the page needs no server and no network for a board (USB,
// Bluetooth, Wi-Fi/LAN): open it straight from disk in Chrome or Edge. The simulator in the same page needs the WebAssembly
// system next to it (palanu-web.js and .wasm, copied here when platform/web is built) and cross-origin isolation: ./pal web.
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { fontFace } from "./pixfont.ts";

const DIR = import.meta.dir;
const out = await Bun.build({ entrypoints: [join(DIR, "src/main.ts")], target: "browser", minify: true, format: "esm" });
if (!out.success) {
  for (const m of out.logs) console.error(m);
  process.exit(1);
}
const js = await out.outputs[0].text();
const xtermCss = readFileSync(join(DIR, "node_modules/@xterm/xterm/css/xterm.css"), "utf8");
/* "Spleen 6x12": Spleen 6x12, the device's own letters (system/src/font_spleen_6x12.c), crisp at 12 px and 24 px */
const pixel = fontFace(join(DIR, "../../system/src/font_spleen_6x12.c"), "Spleen 6x12");
const style = readFileSync(join(DIR, "style.css"), "utf8");
const css = pixel + "\n" + xtermCss + "\n" + style;
/* a "</script>" inside the bundle would end the inline script early */
const safeJs = js.replace(/<\/script/gi, "<\\/script");
const html = readFileSync(join(DIR, "index.html"), "utf8").replace("/*__CSS__*/", () => css).replace("/*__JS__*/", () => safeJs);
mkdirSync(join(DIR, "dist"), { recursive: true });
for (const name of ["index.html", "web-forge.html"]) writeFileSync(join(DIR, "dist", name), html);
console.log(`wrote ${join(DIR, "dist/index.html")} (${(html.length / 1024).toFixed(0)} KB)`);

/* the simulator's system and the service worker that adds isolation on a host without headers */
rmSync(join(DIR, "dist/sim"), { recursive: true, force: true }); /* the old separate page: /sim/ now leads to the one page */
copyFileSync(join(DIR, "coi-sw.js"), join(DIR, "dist/coi-sw.js"));
const webBuild = join(DIR, "../../build/web");
for (const f of ["palanu-web.js", "palanu-web.wasm"]) {
  if (existsSync(join(webBuild, f))) copyFileSync(join(webBuild, f), join(DIR, "dist", f));
  else console.warn(`simulator: ${join(webBuild, f)} missing; build it first (docs/SIMULATOR.md)`);
}

// ---- the install page: dist/flash/ holds the page and, when ./pal build made one, the firmware image and its manifest
const flashDir = join(DIR, "dist/flash");
mkdirSync(flashDir, { recursive: true });
const flashHtml = readFileSync(join(DIR, "flash.html"), "utf8").replace("/*__CSS__*/", () => pixel + "\n" + style);
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
