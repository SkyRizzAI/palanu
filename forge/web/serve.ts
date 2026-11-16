#!/usr/bin/env bun
// Serves the built Web Forge page on http://localhost:5174 (`./pal web`). Only static files: the page does everything in
// the browser. localhost counts as a secure context, so Web Serial and Web Bluetooth work, and ws:// to the device is
// allowed (an https page would block it). Opening dist/web-forge.html straight from disk works too in Chrome and Edge.
import { existsSync } from "node:fs";
import { join } from "node:path";

const FILE = join(import.meta.dir, "dist/web-forge.html");
const PORT = Number(process.env.PORT ?? 5174);
if (!existsSync(FILE)) {
  console.error("build it first: bun forge/web/build.ts");
  process.exit(1);
}
/* the next free port when this one is taken (an earlier Web Forge still running) */
for (let port = PORT; port < PORT + 20; port++) {
  try {
    Bun.serve({
      port,
      hostname: "127.0.0.1",
      fetch: () => new Response(Bun.file(FILE), { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" } }),
    });
    console.log(`Web Forge on http://localhost:${port}`);
    break;
  } catch (e) {
    if ((e as { code?: string }).code !== "EADDRINUSE" || port === PORT + 19) throw e;
  }
}
