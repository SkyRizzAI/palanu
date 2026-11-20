// Which build of the simulator to start (ADR-0022). On GitHub Pages, firmware.json next to the page lists the build of main ("dev")
// and each release's, all on this origin; served by ./pal web or opened elsewhere, there is only the build next to the page.
// The choice is remembered in this browser. The simulator's files (IndexedDB /data) stay across builds, as a board keeps its data
// through an update.
export interface SimBuild {
  id: string;
  version: string;
  channel: "release" | "prerelease" | "dev" | "local";
  path: string; /* the folder of palanu-web.js, relative to the page */
  date?: string;
  commit?: string;
  notes?: string;
  signed: boolean;
}

const KEY = "palanu.forge.simbuild";
let builds: SimBuild[] = [{ id: "local", version: "", channel: "local", path: "", signed: false }];

interface Manifest {
  dev?: { version: string; path: string; date: string; commit?: string; signed?: boolean };
  releases?: { version: string; path: string; date: string; prerelease: boolean; notes?: string; signed?: boolean }[];
}

/** Reads firmware.json; without it (a local build) the list is the build next to the page. */
export async function loadBuilds(): Promise<SimBuild[]> {
  if (!location.protocol.startsWith("http")) return builds;
  try {
    const r = await fetch("firmware.json", { cache: "no-store" });
    if (!r.ok) return builds;
    const m = (await r.json()) as Manifest;
    const list: SimBuild[] = [];
    for (const x of m.releases ?? [])
      list.push({ id: `v${x.version}`, version: x.version, channel: x.prerelease ? "prerelease" : "release", path: x.path, date: x.date, notes: x.notes, signed: !!x.signed });
    if (m.dev) list.push({ id: "dev", version: m.dev.version, channel: "dev", path: m.dev.path, date: m.dev.date, commit: m.dev.commit, signed: !!m.dev.signed });
    if (list.length) builds = list;
  } catch { /* no manifest: the local build */ }
  return builds;
}

export function allBuilds() { return builds; }

/** The remembered build, else the newest stable release, else the first one. */
export function chosenBuild(): SimBuild {
  let id = "";
  try { id = localStorage.getItem(KEY) ?? ""; } catch { /* private window */ }
  return builds.find((b) => b.id === id) ?? builds.find((b) => b.channel === "release") ?? builds[0];
}

export function chooseBuild(id: string) {
  try { localStorage.setItem(KEY, id); } catch { /* private window */ }
}

/** "0.10.0", "0.10.0 · prerelease", "dev 0.10.0 @1a2b3c4", "this build". */
export function buildLabel(b: SimBuild): string {
  if (b.channel === "local") return "this build";
  if (b.channel === "dev") return `dev ${b.version}${b.commit ? ` @${b.commit.slice(0, 7)}` : ""}`;
  return b.channel === "prerelease" ? `${b.version} · prerelease` : b.version;
}
