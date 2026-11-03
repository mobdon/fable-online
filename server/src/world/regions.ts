import fs from 'node:fs';

/** One Albion area (a .lev). Keyed by its level script name, e.g. "HeroGuildComplex". */
export interface RegionDef {
  name: string;
  uid: number;
  sea: boolean;
  neighbours: string[];
}

/** Where each life stage lives. Script names come from FinalAlbion.wld. */
export const STAGE_REGIONS = {
  childhood: 'StartOakValeEast',
  guild_training: 'HeroGuildComplex',
  /** First shared-world spawn after graduating. */
  world: 'HeroGuildComplex',
} as const;

export class RegionTable {
  private byName = new Map<string, RegionDef>();

  constructor(defs: RegionDef[]) {
    for (const d of defs) this.byName.set(d.name, d);
  }

  /**
   * Load shared/defs/regions.json (produced by tools/extractor/regions.mjs from the
   * player's own install). Falls back to a tiny built-in table so the server and tests run
   * without game files.
   */
  static load(file: string): RegionTable {
    if (!fs.existsSync(file)) return RegionTable.fallback();
    const raw = JSON.parse(fs.readFileSync(file, 'utf8')) as {
      maps: { uid: number; script: string; sea: boolean }[];
      graph: Record<string, string[]>;
    };
    const defs = new Map<string, RegionDef>();
    for (const m of raw.maps) {
      if (!m.script || defs.has(m.script)) continue;
      defs.set(m.script, { name: m.script, uid: m.uid, sea: m.sea, neighbours: raw.graph[m.script] ?? [] });
    }
    for (const [name, n] of Object.entries(raw.graph)) {
      if (!defs.has(name)) defs.set(name, { name, uid: 0, sea: false, neighbours: n });
    }
    return new RegionTable([...defs.values()]);
  }

  static fallback(): RegionTable {
    const link: [string, string][] = [
      ['StartOakValeEast', 'StartOakValeWest'],
      ['HeroGuildComplex', 'GuildWoods'],
      ['HeroGuildComplex', 'LookoutPoint'],
      ['LookoutPoint', 'PicnicArea'],
      ['LookoutPoint', 'BowerstoneSlums'],
      ['LookoutPoint', 'GreatwoodEntrance'],
      ['GreatwoodEntrance', 'Greatwood'],
      ['Greatwood', 'OakValeWest'],
      ['Greatwood', 'ArenaExterior'],
    ];
    const defs = new Map<string, RegionDef>();
    const get = (n: string) => {
      let d = defs.get(n);
      if (!d) defs.set(n, (d = { name: n, uid: defs.size + 1, sea: false, neighbours: [] }));
      return d;
    };
    for (const [a, b] of link) {
      get(a).neighbours.push(b);
      get(b).neighbours.push(a);
    }
    return new RegionTable([...defs.values()]);
  }

  get(name: string): RegionDef | undefined {
    return this.byName.get(name);
  }
  has(name: string): boolean {
    return this.byName.has(name);
  }
  get size(): number {
    return this.byName.size;
  }
  areNeighbours(a: string, b: string): boolean {
    return this.byName.get(a)?.neighbours.includes(b) ?? false;
  }
}
