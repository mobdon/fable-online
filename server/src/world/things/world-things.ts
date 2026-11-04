import fs from 'node:fs';

/**
 * Interactive things placed in Albion: region exits, doors, chests, searchable
 * containers, items lying in the world, digging spots, demon doors and doors that
 * teleport. Read from shared/defs/world-things.json, which tools/extractor/things.mjs
 * builds from the player's own FinalAlbion.wad / .gtg / .wld.
 *
 * Every position is in the frame of the region it belongs to (the frame of the region's
 * anchor level; other levels of the same region are shifted by their MapX/MapY offset),
 * which is the frame the region's sim and the hero rows in the database use.
 *
 * Ids are "<LevelScript>:<uid within the level>". Fable's thing UIDs repeat between
 * levels, so the bare UID is not enough.
 */
export type Vec3T = [number, number, number];

interface ThingBase {
  id: string;
  uid: string;
  def: string;
  level: string;
  region: string;
  /** "NULL" for things that always exist; otherwise the quest section that adds them. */
  section: string;
  script: string | null;
  pos: Vec3T;
}

export interface Link {
  entrance: string;
  region: string;
  level: string;
  pos: Vec3T;
  fwd: [number, number] | null;
}

export interface ExitThing extends ThingBase {
  active: boolean;
  radius: number;
  to: Link | null;
}
export interface DoorThing extends ThingBase {
  open: boolean;
  trigger: number;
  owner: string | null;
  prison: boolean;
}
export interface ChestThing extends ThingBase {
  contents: string[];
  open: boolean;
  /** Silver key chests: how many silver keys the hero must carry (they are not used up). */
  silverKeys: number;
  persistent: boolean;
}
export interface ContainerThing extends ThingBase {
  contents: string[];
}
export interface PickupThing extends ThingBase {
  forSale: boolean;
  stealable: boolean;
  price: number;
  persistent: boolean;
}
export interface DigSpotThing extends ThingBase {
  contents: string[];
  hidden: boolean;
}
export interface DemonDoorThing extends ThingBase {
  /** Id of the region exit this demon door stands in front of. */
  exit: string | null;
}
export interface HookThing extends ThingBase {
  to: Link | null;
}

/**
 * A Cullis Gate (or the Guild's teleport pedestal): a thing with Fable's CTCTeleporter
 * component. Kept apart from the usable things above because the travel rules own it
 * (src/world/travel/travel.ts), not WorldObjects.
 */
export interface GateThing extends ThingBase {
  /** The direction the gate faces; arrivals step out this way. */
  fwd: [number, number] | null;
  /** The Guild's own gate: every graduated hero can always travel there. */
  guild: boolean;
}

export interface WorldThingsFile {
  v: 1;
  /** Older extracts have no gates. */
  gates?: GateThing[];
  exits: ExitThing[];
  doors: DoorThing[];
  chests: ChestThing[];
  containers: ContainerThing[];
  pickups: PickupThing[];
  digSpots: DigSpotThing[];
  demonDoors: DemonDoorThing[];
  hooks: HookThing[];
}

export type ThingKind = 'exit' | 'door' | 'chest' | 'container' | 'pickup' | 'dig' | 'demon_door' | 'hook';
export type AnyThing =
  | ({ kind: 'exit' } & ExitThing)
  | ({ kind: 'door' } & DoorThing)
  | ({ kind: 'chest' } & ChestThing)
  | ({ kind: 'container' } & ContainerThing)
  | ({ kind: 'pickup' } & PickupThing)
  | ({ kind: 'dig' } & DigSpotThing)
  | ({ kind: 'demon_door' } & DemonDoorThing)
  | ({ kind: 'hook' } & HookThing);

export interface RegionThings {
  exits: ExitThing[];
  doors: DoorThing[];
  pickups: PickupThing[];
  byId: Map<string, AnyThing>;
}

const EMPTY: RegionThings = { exits: [], doors: [], pickups: [], byId: new Map() };

export class WorldThings {
  private regions = new Map<string, RegionThings>();
  private all = new Map<string, AnyThing>();
  /** demon door id by the id of the exit it guards */
  readonly guardedExits = new Map<string, string>();
  /** Cullis Gates, by id. Not part of get(): travel looks them up itself. */
  readonly gates = new Map<string, GateThing>();

  constructor(f: WorldThingsFile) {
    const add = (t: AnyThing) => {
      this.all.set(t.id, t);
      let r = this.regions.get(t.region);
      if (!r) this.regions.set(t.region, (r = { exits: [], doors: [], pickups: [], byId: new Map() }));
      r.byId.set(t.id, t);
      if (t.kind === 'exit') r.exits.push(t);
      if (t.kind === 'door') r.doors.push(t);
      if (t.kind === 'pickup') r.pickups.push(t);
    };
    for (const t of f.exits) add({ kind: 'exit', ...t });
    for (const t of f.doors) add({ kind: 'door', ...t });
    for (const t of f.chests) add({ kind: 'chest', ...t });
    for (const t of f.containers) add({ kind: 'container', ...t });
    for (const t of f.pickups) add({ kind: 'pickup', ...t });
    for (const t of f.digSpots) add({ kind: 'dig', ...t });
    for (const t of f.demonDoors) add({ kind: 'demon_door', ...t });
    for (const t of f.hooks) add({ kind: 'hook', ...t });
    for (const d of f.demonDoors) if (d.exit) this.guardedExits.set(d.exit, d.id);
    for (const g of f.gates ?? []) this.gates.set(g.id, g);
  }

  /** Load the extracted file, or an empty table so the server still runs without game files. */
  static load(file: string): WorldThings {
    if (!file || !fs.existsSync(file)) return WorldThings.empty();
    return new WorldThings(JSON.parse(fs.readFileSync(file, 'utf8')) as WorldThingsFile);
  }

  static empty(): WorldThings {
    return new WorldThings({ v: 1, exits: [], doors: [], chests: [], containers: [], pickups: [], digSpots: [], demonDoors: [], hooks: [] });
  }

  region(name: string): RegionThings {
    return this.regions.get(name) ?? EMPTY;
  }

  get(id: string): AnyThing | undefined {
    return this.all.get(id);
  }

  get size(): number {
    return this.all.size;
  }
}
