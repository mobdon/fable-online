import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ItemCatalog, catalogFromGameData } from '../src/economy/catalog.js';
import { BALANCE } from '../src/combat/rules.js';
import { FALLBACK_TOWN, type TownData } from '../src/world/town/town-data.js';
import { shopsFromTown } from '../src/world/town/town-shops.js';

const ITEMS = {
  OBJECT_HEALTH_POTION: { value: 100 },
  OBJECT_RESURRECTION_PHIAL: { value: 1000 },
  OBJECT_HERO_SHIRT_PLATE: { value: 4000, armour: 'ARMOUR_HERO_PLATE_ARMOUR', coverage: 0.4 },
  OBJECT_HERO_SHIRT_CHAINMAIL: { value: 2000, armour: 'ARMOUR_HERO_CHAINMAIL', coverage: 0.4 },
  OBJECT_HERO_SHIRT_PLATINUM: { value: 5000, armour: 'ARMOUR_HERO_PLATINUM_ARMOUR', coverage: 0.4 },
};

test('catalog: vendors never sell plate, phials or platinum; plate can still be sold back', () => {
  const c = new ItemCatalog(catalogFromGameData(ITEMS, BALANCE.staffArmour));
  assert.equal(c.get('OBJECT_HERO_SHIRT_PLATE').buy, null);
  assert.equal(c.get('OBJECT_HERO_SHIRT_PLATE').sell, 1000);
  assert.equal(c.get('OBJECT_HERO_SHIRT_PLATE').tradeable, true);
  assert.equal(c.get('OBJECT_RESURRECTION_PHIAL').buy, null);
  assert.equal(c.get('OBJECT_HERO_SHIRT_PLATINUM').buy, null);
  assert.equal(c.get('OBJECT_HERO_SHIRT_PLATINUM').sell, null);
  assert.equal(c.get('OBJECT_HERO_SHIRT_CHAINMAIL').buy, 2000);
});

test('shops from town data leave out plate armour and Resurrection Phials', () => {
  const c = new ItemCatalog(catalogFromGameData(ITEMS, BALANCE.staffArmour));
  const row = (def: string) => ({ def, quantity: 1, maxStock: 1, priceMultiplier: 1, restockDays: 1 });
  const town: TownData = {
    ...FALLBACK_TOWN,
    shopTypes: {
      ...FALLBACK_TOWN.shopTypes,
      BUILDING_TEST_GENERAL_STORE: ['OBJECT_HEALTH_POTION', 'OBJECT_RESURRECTION_PHIAL', 'OBJECT_HERO_SHIRT_PLATE', 'OBJECT_HERO_SHIRT_CHAINMAIL', 'OBJECT_HERO_SHIRT_PLATINUM'].map(row),
    },
  };
  const stocked = new Set(shopsFromTown(town, c).flatMap((s) => s.stock));
  assert.ok(stocked.has('OBJECT_HEALTH_POTION'));
  assert.ok(stocked.has('OBJECT_HERO_SHIRT_CHAINMAIL'));
  for (const banned of ['OBJECT_RESURRECTION_PHIAL', 'OBJECT_HERO_SHIRT_PLATE', 'OBJECT_HERO_SHIRT_PLATINUM']) assert.ok(!stocked.has(banned), banned);
});
