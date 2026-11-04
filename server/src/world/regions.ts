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

/** One level's rectangle on the world map (metres), and the region it belongs to. */
export interface LevelRect {
  region: string;
  x: number;
  y: number;
  w: number;
  h: number;
}

export class RegionTable {
  private byName = new Map<string, RegionDef>();
  private rects: LevelRect[] = [];
  /** Level script name -> the region (graph name) it belongs to, where they differ. */
  private regionOfScript = new Map<string, string>();

  constructor(defs: RegionDef[], rects: LevelRect[] = [], scripts: [string, string][] = []) {
    for (const d of defs) this.byName.set(d.name, d);
    for (const [script, region] of scripts) if (script !== region) this.regionOfScript.set(script, region);
    // Only levels of regions the server runs; filler and sea levels are scenery.
    this.rects = rects.filter((r) => this.byName.has(r.region) && r.w > 0 && r.h > 0);
  }

  /**
   * The region whose level holds this world position (x, y on Albion's map, which is where
   * Fable's own coordinates are), or undefined when it is in none the server knows. Levels
   * don't overlap in the retail world file, so the first match is the only one.
   */
  locate(x: number, y: number): string | undefined {
    for (const r of this.rects) if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return r.region;
    return undefined;
  }

  /**
   * The region graph's name for a region or level script name. Some server names are level
   * scripts (HeroGuildComplex) where the graph says HeroGuildComplexInside: same place.
   */
  canonical(name: string): string {
    return this.regionOfScript.get(name) ?? name;
  }

  /** True when locate() can answer (regions.json was extracted with level sizes). */
  get canLocate(): boolean {
    return this.rects.length > 0;
  }

  /**
   * Load shared/defs/regions.json (produced by tools/extractor/regions.mjs from the
   * player's own install). Falls back to a tiny built-in table so the server and tests run
   * without game files.
   */
  static load(file: string): RegionTable {
    if (!fs.existsSync(file)) return RegionTable.fallback();
    const raw = JSON.parse(fs.readFileSync(file, 'utf8')) as {
      maps: { uid: number; script: string; sea: boolean; region?: string; x?: number; y?: number; w?: number; h?: number }[];
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
    const rects: LevelRect[] = [];
    for (const m of raw.maps)
      // Only the playable regions (the ones in the region graph), never filler or sea.
      if (m.region && raw.graph[m.region] && !m.sea && m.w && m.h && Number.isFinite(m.x) && Number.isFinite(m.y))
        rects.push({ region: m.region, x: m.x!, y: m.y!, w: m.w, h: m.h });
    const scripts: [string, string][] = raw.maps.filter((m) => m.script && m.region).map((m) => [m.script, m.region!]);
    return new RegionTable([...defs.values()], rects, scripts);
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
      ['BowerstoneSlums', 'BowerstonePosh'],
      ['BowerstonePosh', 'BowerstoneJail'],
      ['PrisonCourtyard', 'PrisonCells_1'],
      ['PrisonCourtyard', 'PrisonCells_2'],
      ['PrisonCourtyard', 'PrisonCells_3'],
      ['PrisonCourtyard', 'PrisonOffice'],
      ['PrisonPath', 'GibbetWoods'],
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
