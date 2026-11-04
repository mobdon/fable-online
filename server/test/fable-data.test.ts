import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameBin, NamesBin, fableHash, readF32, readI32 } from '../src/data/fable-bin.js';
import { extract, FIELDS } from '../src/data/extract.js';
import { extractBoasts } from '../src/data/boasts.js';
import { buildBins } from './fixtures/fable-bin-builder.js';
// @ts-expect-error plain JS tool
import { parseTng } from '../../tools/extractor/spawns.mjs';

test('fableHash is reflected CRC-32 with zero init and no final xor', () => {
  // Values checked against names.bin, which stores each name next to its hash.
  assert.equal(fableHash('FRONT_END'), 0x0a7d3e6c);
  assert.equal(fableHash('Type'), 0x0da8270b);
  assert.equal(fableHash('Health'), 0x78b48786);
});

const TYPES = ['CWeaponDef', 'CStockItemDef', 'CCreatureDef', 'CEnemyDef', 'CQuestCardDef', 'CAppearanceModifierDef'];
const DEFS = [
  { group: 'FACTION', name: 'FACTION_TEST_BANDITS' },
  { group: 'ARMOUR', name: 'ARMOUR_HERO_CHAINMAIL' },
  {
    group: 'OBJECT',
    name: 'OBJECT_TEST_SWORD',
    components: [
      { type: 'CWeaponDef', fields: [{ name: 'Type', i32: 0 }, { name: 'Class', i32: 2 }, { name: 'Damage', f32: 7.5 }] },
      { type: 'CStockItemDef', fields: [{ name: FIELDS.stock.value, i32: 1234 }] },
    ],
  },
  {
    group: 'OBJECT',
    name: 'OBJECT_HERO_SHIRT_TEST',
    components: [
      { type: 'CAppearanceModifierDef', fields: [{ name: FIELDS.clothing.armour, ref: 'ARMOUR_HERO_CHAINMAIL' }, { name: FIELDS.clothing.coverage, f32: 0.4 }] },
      { type: 'CStockItemDef', fields: [{ name: FIELDS.stock.value, i32: 99 }] },
    ],
  },
  {
    group: 'CREATURE',
    name: 'CREATURE_TEST_GRUNT',
    own: [
      { name: 'Health', f32: 42 },
      { name: FIELDS.creature.attackScale, f32: 1.25 },
      { name: FIELDS.creature.weapon, ref: 'OBJECT_TEST_SWORD' },
    ],
    components: [
      { type: 'CCreatureDef', fields: [{ name: FIELDS.creatureDef.experience, i32: 17 }, { name: FIELDS.creatureDef.reward2, i32: 9 }] },
      { type: 'CEnemyDef', fields: [{ name: 'Faction', ref: 'FACTION_TEST_BANDITS' }] },
    ],
  },
  { group: 'CREATURE_GENERATION_FAMILY', name: 'TEST_FAMILY', own: [{ name: FIELDS.family.creatures, refs: ['CREATURE_TEST_GRUNT', 'CREATURE_TEST_GRUNT'] }] },
  {
    group: 'OBJECT',
    name: 'OBJECT_QUEST_CARD_TEST',
    components: [
      {
        type: 'CQuestCardDef',
        fields: [
          { name: 'GoldReward', i32: 750 },
          { name: 'RenownReward', i32: 300 },
          { name: 'RewardObjects', refs: ['OBJECT_TEST_SWORD'] },
        ],
      },
    ],
  },
];

test('GameBin decodes the def tables, chunks and component lists', () => {
  const { game, names } = buildBins(TYPES, DEFS);
  const g = new GameBin(game, new NamesBin(names));
  assert.equal(g.types.length, TYPES.length);
  assert.equal(g.defs.length, DEFS.length);
  const sword = g.def('OBJECT_TEST_SWORD')!;
  assert.deepEqual(
    g.componentsOf(sword).map((c) => c.type),
    ['CWeaponDef', 'CStockItemDef'],
  );
  assert.equal(readF32(g.component(sword, 'CWeaponDef'), 'Damage'), 7.5);
  assert.equal(readI32(g.component(sword, 'CStockItemDef'), FIELDS.stock.value), 1234);
});

test('GameBin refuses a names.bin from another build', () => {
  const a = buildBins(TYPES, DEFS);
  const names = Buffer.from(a.names);
  names.writeUInt32LE(0x99999999, 4);
  assert.throws(() => new GameBin(a.game, new NamesBin(names)), /different builds/);
});

test('extract pulls weapons, armour, creatures, families and quest cards', () => {
  const { game, names } = buildBins(TYPES, DEFS);
  const d = extract(game, names);
  assert.deepEqual(d.items.OBJECT_TEST_SWORD, { value: 1234, weapon: { type: 'sword', class: 'heavy', damage: 7.5 } });
  assert.deepEqual(d.items.OBJECT_HERO_SHIRT_TEST, { value: 99, armour: 'ARMOUR_HERO_CHAINMAIL', coverage: 0.4 });
  assert.deepEqual(d.creatures.CREATURE_TEST_GRUNT, {
    health: 42,
    experience: 17,
    reward2: 9,
    faction: 'FACTION_TEST_BANDITS',
    combatType: null,
    weapon: 'OBJECT_TEST_SWORD',
    group: null,
    attackScale: 1.25,
  });
  assert.deepEqual(d.families.TEST_FAMILY, ['CREATURE_TEST_GRUNT', 'CREATURE_TEST_GRUNT']);
  assert.deepEqual(d.questCards.OBJECT_QUEST_CARD_TEST, { gold: 750, renown: 300, items: ['OBJECT_TEST_SWORD'] });
});

test('boasts from script.bin raise the matching quest card cap', () => {
  // A script.bin is the same container as game.bin, with one CScriptDef holding constants.
  const DEFS_WITH_FARM = [
    ...DEFS,
    {
      group: 'OBJECT',
      name: 'OBJECT_QUEST_CARD_PROTECT_FARM',
      components: [{ type: 'CQuestCardDef', fields: [{ name: 'GoldReward', i32: 750 }, { name: 'RenownReward', i32: 400 }] }],
    },
  ];
  const { game, names } = buildBins(TYPES, DEFS_WITH_FARM);
  const script = buildBins(['CScriptDef'], [
    {
      group: 'CScriptDef',
      name: 'SCRIPT_DEF',
      own: [
        { name: 'SOME_OTHER_CONSTANT', i32: 7 },
        { name: 'OFGoodNakedBoastCost', i32: 80 },
        { name: 'OFGoodNakedBoastReward', i32: 160 },
        { name: 'OFGoodNoDamageBoastReward', i32: 400 },
        { name: 'OFGoodNoWeaponsBoastReward', i32: -5 }, // nonsense is skipped
        { name: 'ArenaNakedBoastReward', i32: 4000 }, // no ARENA card in this file
      ],
    },
  ]);
  // The script.bin above has its own names.bin; extractBoasts takes the pair that belong together.
  assert.deepEqual(extractBoasts(script.game, script.names).OBJECT_QUEST_CARD_PROTECT_FARM, { gold: 560, renown: 80, found: 2 });
  const d = extract(game, names, undefined);
  assert.equal(d.questCards.OBJECT_QUEST_CARD_PROTECT_FARM!.boastGold, undefined);
});

test('extract reads the experience price tables: stat tracks in slot order, ability costs and slots', () => {
  const track = (category: number, costs: number[]) => [
    { name: FIELDS.heroStats.category, i32: category },
    { name: FIELDS.heroStats.levels, i32: costs.length },
    ...costs.flatMap((c, i) => [
      { name: FIELDS.heroStats.cost, i32: c },
      { name: 0xafe941d7, i32: 0 },
      { name: 0x80ff7e1d, i32: 100 + i },
    ]),
  ];
  const defs = [
    ...DEFS,
    // Ten tracks like the retail file: six real ones, three unused, then Will.
    { group: 'HERO_STATS', name: 'MAIN_HERO_STATS', own: [0, 0, 0, 1, 1, 1, 2, 2, 2, 2].flatMap((c, i) => track(c, [0, 10 * (i + 1), 20 * (i + 1)])) },
    { group: 'HERO_ABILITY', name: 'HERO_ABILITY_WILL_FIREBALL', own: [{ name: FIELDS.heroAbility.slot, i32: 2 }, { name: FIELDS.heroAbility.costs, i32s: [5, 6, 7, 8] }] },
    { group: 'HERO_ABILITY', name: 'HERO_ABILITY_STENGTH_BERSERK', own: [{ name: FIELDS.heroAbility.slot, i32: 10 }, { name: FIELDS.heroAbility.costs, i32s: [1, 2, 3, 4] }] },
  ];
  const { game, names } = buildBins([...TYPES, 'HERO_STATS', 'HERO_ABILITY'], defs);
  const p = extract(game, names).progression!;
  assert.deepEqual(
    p.stats.map((s) => [s.slot, s.id, s.discipline, s.costs[2]]),
    [
      [0, 'physique', 'strength', 20],
      [1, 'health', 'strength', 40],
      [2, 'toughness', 'strength', 60],
      [3, 'speed', 'skill', 80],
      [4, 'accuracy', 'skill', 100],
      [5, 'guile', 'skill', 120],
      [9, 'will', 'will', 200],
    ],
  );
  assert.deepEqual(p.abilities, [
    { slot: 2, def: 'HERO_ABILITY_WILL_FIREBALL', discipline: 'will', costs: [5, 6, 7, 8] },
    { slot: 10, def: 'HERO_ABILITY_STENGTH_BERSERK', discipline: 'strength', costs: [1, 2, 3, 4] },
  ]);
});

test('parseTng keeps placed creatures and creature generators', () => {
  const tng = `Version 2;
XXXSectionStart NULL;
NewThing AICreature;
UID 1;
DefinitionType "CREATURE_TEST_GRUNT";
ScriptName NULL;
PositionX 10.5;
PositionY -3.0;
PositionZ 2.0;
RHSetForwardX 0.0;
RHSetForwardY 1.0;
EndThing;
NewThing Marker;
UID 2;
DefinitionType "MARKER_CREATURE_GENERATOR";
ScriptName NULL;
PositionX 1.0;
PositionY 2.0;
PositionZ 3.0;
CreatureFamilies[0] "TEST_FAMILY";
CreatureFamilies[1] "OTHER";
GenerationRadius 8.0;
SelfTriggerRadius 12.0;
ActiveCreatureLimit 4;
TotalGenerationLimit -1;
EndThing;
NewThing Object;
UID 3;
DefinitionType "OBJECT_BARREL";
EndThing;
XXXSectionEnd;`;
  const r = parseTng(tng);
  assert.equal(r.creatures.length, 1);
  assert.deepEqual(r.creatures[0], { uid: '1', def: 'CREATURE_TEST_GRUNT', x: 10.5, y: -3, z: 2, yaw: 0, script: null });
  assert.equal(r.generators.length, 1);
  assert.deepEqual(r.generators[0].families, ['TEST_FAMILY', 'OTHER']);
  assert.equal(r.generators[0].activeLimit, 4);
  assert.equal(r.generators[0].radius, 8);
});
