#!/usr/bin/env bun
// Serves Palanu Forge on http://localhost:5174 (`./pal web`).
//   /          the page: a remote board and the simulator (PalanuOS built to WebAssembly, run in the page). Every response is
//              cross-origin isolated (COOP and COEP headers) for the simulator's worker threads.
//   /store/    the repository's app catalogue (store/), so the Store section can install from it without a web host
//   /sim/      the old simulator address: it leads to the page
//   /tcp/H/P   a WebSocket that forwards raw TCP to H:P, for the simulator's network bridge. Same-origin pages only, and the
//              server listens on this computer only.
// localhost counts as a secure context, so Web Serial and Web Bluetooth work, and ws:// to the device is allowed.
import { existsSync } from "node:fs";
import { extname, join, normalize } from "node:path";

const DIST = join(import.meta.dir, "dist");
const PORT = Number(process.env.PORT ?? 5174);
const STORE = join(import.meta.dir, "../../store");
if (!existsSync(join(DIST, "index.html"))) {
  console.error("build it first: bun forge/web/build.ts");
  process.exit(1);
}

const TYPES: Record<string, string> = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".wasm": "application/wasm", ".css": "text/css; charset=utf-8",
  ".json": "application/json", ".png": "image/png", ".svg": "image/svg+xml", ".txt": "text/plain; charset=utf-8", ".papp": "application/octet-stream",
};
const ISOLATION = { "cross-origin-opener-policy": "same-origin", "cross-origin-embedder-policy": "require-corp", "cross-origin-resource-policy": "same-origin" };

interface Tunnel { host: string; port: number; sock?: { write(d: Uint8Array | string): number; end(): void }; queued: (string | Uint8Array)[]; closed: boolean }

function serve(port: number) {
  return Bun.serve<Tunnel>({
    port,
    hostname: "127.0.0.1",
    fetch(req, server) {
      const url = new URL(req.url);
      const t = /^\/tcp\/([^/]+)\/(\d{1,5})$/.exec(url.pathname);
      if (t) {
        const origin = req.headers.get("origin");
        if (origin !== `http://${req.headers.get("host")}`) return new Response("forbidden", { status: 403 });
        const ok = server.upgrade(req, { headers: { "sec-websocket-protocol": "binary" }, data: { host: decodeURIComponent(t[1]), port: Number(t[2]), queued: [], closed: false } });
        return ok ? undefined : new Response("WebSocket upgrade required", { status: 426 });
      }
      if (url.pathname === "/sim" || url.pathname.startsWith("/sim/")) return Response.redirect("/", 302);
      let path = url.pathname;
      if (path.endsWith("/")) path += "index.html";
      const root = path.startsWith("/store/") ? STORE : DIST;
      const file = join(root, normalize(root === STORE ? path.slice(6) : path));
      if (!file.startsWith(root) || !existsSync(file)) return new Response("not found", { status: 404, headers: ISOLATION });
      const headers: Record<string, string> = { "content-type": TYPES[extname(file)] ?? "application/octet-stream", "cache-control": "no-store", ...ISOLATION };
      return new Response(Bun.file(file), { headers });
    },
    websocket: {
      async open(ws) {
        const d = ws.data;
        try {
          d.sock = await Bun.connect({
            hostname: d.host,
            port: d.port,
            socket: {
              data(_s, buf) { ws.send(buf); },
              close() { d.closed = true; ws.close(); },
              error() { d.closed = true; ws.close(); },
              open() {},
            },
          });
          for (const q of d.queued) d.sock.write(q);
          d.queued.length = 0;
          if (d.closed) d.sock.end();
        } catch {
          ws.close(1011, "cannot reach the target");
        }
      },
      message(ws, msg) {
        const d = ws.data;
        if (d.sock) d.sock.write(msg as string | Uint8Array);
        else d.queued.push(msg as string | Uint8Array);
      },
      close(ws) {
        ws.data.closed = true;
        ws.data.sock?.end();
      },
    },
  });
}

/* the next free port when this one is taken (an earlier Web Forge still running) */
for (let port = PORT; port < PORT + 20; port++) {
  try {
    serve(port);
    console.log(`Palanu Forge on http://localhost:${port}  (a board, and the simulator in the same page)`);
    break;
  } catch (e) {
    if ((e as { code?: string }).code !== "EADDRINUSE" || port === PORT + 19) throw e;
  }
}
