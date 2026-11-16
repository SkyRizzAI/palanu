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
