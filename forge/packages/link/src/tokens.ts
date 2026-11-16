// The token store of the computer side: ~/.config/palanu/links.json (mode 600), one entry per device id.
// PALANU_CONFIG_DIR moves the directory (tests use a temporary one). The file holds bearer tokens: keep it private.
import { chmodSync, mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

export type Pairing = { clientId: string; token: string; name: string; paired: string };

export function configDir(): string {
  return process.env.PALANU_CONFIG_DIR || join(homedir(), ".config", "palanu");
}

export function linksPath(): string {
  return join(configDir(), "links.json");
}

function readAll(): Record<string, Pairing> {
  try {
    return JSON.parse(readFileSync(linksPath(), "utf8")) as Record<string, Pairing>;
  } catch {
    return {}; // no file, or one that does not parse: nothing is paired
  }
}

export function loadPairing(deviceId: string): Pairing | undefined {
  return readAll()[deviceId];
}

function writeAll(all: Record<string, Pairing>): void {
  mkdirSync(configDir(), { recursive: true, mode: 0o700 });
  const tmp = linksPath() + ".tmp";
  writeFileSync(tmp, JSON.stringify(all, null, 2) + "\n", { mode: 0o600 });
  chmodSync(tmp, 0o600); // writeFileSync keeps the mode of a file that exists, so set it on the new one too
  renameSync(tmp, linksPath());
}

export function savePairing(deviceId: string, p: Pairing): void {
  const all = readAll();
  all[deviceId] = p;
  writeAll(all);
}

export function forgetPairing(deviceId: string): void {
  const all = readAll();
  delete all[deviceId];
  writeAll(all);
}
