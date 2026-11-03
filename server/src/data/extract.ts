import fs from 'node:fs';
import path from 'node:path';
import { GameBin, NamesBin, fieldOffset, readF32, readI32, type Def } from './fable-bin.js';
import type { CreatureData, GameData, ItemData, QuestCardData, SpellData, WeaponClass, WeaponType } from './game-data.js';

/**
 * Pulls the numbers the server needs out of game.bin. Each field below was identified by
 * comparing values across many defs (iron < steel < ebony < crystal < legendary weapons,
 * bandit grunt level 1 < 2 < 3, and so on). Field names that Fable.exe still carries are
 * looked up by name; the rest are their CRC-32 hashes.
 */
export const FIELDS = {
  weapon: { type: 'Type', class: 'Class', damage: 'Damage' },
  stock: { value: 0x70395b57 },
  clothing: { coverage: 0xdebc9e21, armour: 0x909e89d5 },
  creature: { health: 'Health', attackScale: 0xa4159d90, weapon: 0xba0f59e8, combatType: 'CombatTypeDef', group: 'GroupDef' },
  creatureDef: { experience: 0x2638684a, reward2: 0x277ede57 },
  enemy: { faction: 'Faction' },
  questCard: { gold: 'GoldReward', renown: 'RenownReward', objects: 'RewardObjects' },
  /** CREATURE_GENERATION_FAMILY: u32 count, then that many creature def refs. */
  family: { creatures: 0xc50c0369 },
} as const;

const WEAPON_TYPES: WeaponType[] = ['sword', 'axe', 'hammer', 'longbow', 'crossbow'];
const WEAPON_CLASSES: Record<number, WeaponClass> = { 1: 'light', 2: 'heavy', 3: 'ranged' };

export function extract(gameBin: Buffer, namesBin: Buffer): GameData {
  const names = new NamesBin(namesBin);
  const g = new GameBin(gameBin, names);
  const ref = (v: number | undefined) => (v === undefined || v < 0 ? null : (g.defNameAt(v) ?? null));

  const items: Record<string, ItemData> = {};
  for (const def of g.group('OBJECT')) {
    if (def.name.endsWith('_TEMPLATE')) continue;
    const stock = g.component(def, 'CStockItemDef');
    const weaponC = g.component(def, 'CWeaponDef');
    const value = readI32(stock, FIELDS.stock.value);
    if (value === undefined && !weaponC) continue;
    const item: ItemData = { value: Math.max(0, value ?? 0) };
    if (weaponC) {
      const t = readI32(weaponC, FIELDS.weapon.type) ?? -1;
      const c = readI32(weaponC, FIELDS.weapon.class) ?? 0;
      const dmg = readF32(weaponC, FIELDS.weapon.damage) ?? 0;
      item.weapon = { type: WEAPON_TYPES[t] ?? 'other', class: WEAPON_CLASSES[c] ?? 'other', damage: dmg };
    }
    const look = g.component(def, 'CAppearanceModifierDef');
    if (look && def.name.startsWith('OBJECT_HERO_')) {
      const armour = ref(readI32(look, FIELDS.clothing.armour));
      const coverage = readF32(look, FIELDS.clothing.coverage);
      if (armour?.startsWith('ARMOUR_')) item.armour = armour;
      if (coverage !== undefined && coverage > 0 && coverage <= 1) item.coverage = coverage;
    }
    items[def.name] = item;
  }

  const creatures: Record<string, CreatureData> = {};
  for (const def of g.group('CREATURE')) {
    if (def.name.endsWith('_TEMPLATE') || def.name.endsWith('_BASE')) continue;
    const own = g.entry(def.index);
    const health = readF32(own, FIELDS.creature.health);
    if (health === undefined || health <= 0) continue;
    const cd = g.component(def, 'CCreatureDef');
    const enemy = g.component(def, 'CEnemyDef');
    creatures[def.name] = {
      health,
      experience: Math.max(0, readI32(cd, FIELDS.creatureDef.experience) ?? 0),
      reward2: Math.max(0, readI32(cd, FIELDS.creatureDef.reward2) ?? 0),
      faction: ref(readI32(enemy, FIELDS.enemy.faction)),
      combatType: ref(readI32(own, FIELDS.creature.combatType)),
      weapon: ref(readI32(own, FIELDS.creature.weapon)),
      group: ref(readI32(own, FIELDS.creature.group)),
      attackScale: readF32(own, FIELDS.creature.attackScale) ?? 1,
    };
  }

  const spells: Record<string, SpellData> = {};
  for (const def of g.defs) {
    if (!def.group.startsWith('SPECIAL_ABILITIES_')) continue;
    spells[def.name] = { levels: levelArrays(g.own(def)) };
  }

  const families: Record<string, string[]> = {};
  for (const def of g.group('CREATURE_GENERATION_FAMILY')) {
    const b = g.entry(def.index);
    const o = fieldOffset(b, FIELDS.family.creatures);
    if (o < 0 || o + 4 > b.length) continue;
    const n = b.readUInt32LE(o);
    const list: string[] = [];
    for (let i = 0; i < n && o + 8 + i * 4 <= b.length; i++) {
      const name = ref(b.readInt32LE(o + 4 + i * 4));
      if (name) list.push(name);
    }
    if (list.length) families[def.name] = list;
  }

  // Quest cards carry the rewards the game pays: the cap for client-side quests.
  const questCards: Record<string, QuestCardData> = {};
  for (const def of g.group('OBJECT')) {
    const qc = g.component(def, 'CQuestCardDef');
    if (!qc || def.name.endsWith('_TEMPLATE')) continue;
    const items: string[] = [];
    const o = fieldOffset(qc, FIELDS.questCard.objects);
    if (o >= 0 && o + 4 <= qc.length) {
      const n = qc.readUInt32LE(o);
      for (let i = 0; i < n && i < 16 && o + 8 + i * 4 <= qc.length; i++) {
        const name = ref(qc.readInt32LE(o + 4 + i * 4));
        if (name) items.push(name);
      }
    }
    questCards[def.name] = {
      gold: Math.max(0, readI32(qc, FIELDS.questCard.gold) ?? 0),
      renown: Math.max(0, readI32(qc, FIELDS.questCard.renown) ?? 0),
      items,
    };
  }

  return {
    source: { kind: 'game.bin', namesCrc: names.crc.toString(16), extractedAt: new Date().toISOString() },
    items,
    creatures,
    abilities: g.group('HERO_ABILITY').map((d: Def) => d.name),
    spells,
    families,
    questCards,
  };
}

/** Spell defs store per-level tuning as { u32 hash, u32 4, f32 x4 }. */
function levelArrays(b: Buffer): Record<string, number[]> {
  const out: Record<string, number[]> = {};
  let p = 0;
  while (p + 24 <= b.length) {
    if (b.readUInt32LE(p + 4) === 4) {
      const vals = [0, 1, 2, 3].map((i) => Math.round(b.readFloatLE(p + 8 + i * 4) * 1000) / 1000);
      if (vals.every((v) => Number.isFinite(v) && Math.abs(v) < 1e6)) {
        out[b.readUInt32LE(p).toString(16).padStart(8, '0')] = vals;
        p += 24;
        continue;
      }
    }
    p++;
  }
  return out;
}

/** Finds the two files inside a Fable install folder (case-insensitive, as on Windows). */
export function findDefs(fableDir: string): { game: string; names: string } {
  const pick = (dir: string, want: string) => {
    if (!fs.existsSync(dir)) return undefined;
    const hit = fs.readdirSync(dir).find((f) => f.toLowerCase() === want.toLowerCase());
    return hit && path.join(dir, hit);
  };
  const data = pick(fableDir, 'data') ?? fableDir;
  const defs = pick(data, 'CompiledDefs') ?? data;
  const game = pick(defs, 'game.bin');
  const names = pick(defs, 'names.bin');
  if (!game || !names) throw new Error(`Could not find data/CompiledDefs/game.bin and names.bin under ${fableDir}`);
  return { game, names };
}
