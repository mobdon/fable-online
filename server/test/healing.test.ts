import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { CombatSim } from '../src/world/combat-sim.js';
import { claimHeal, pickHeal } from '../src/combat/healing.js';
import { decode, ProtocolError } from '../src/net/protocol.js';
import { key, makeHero, testApp } from './helpers.js';

const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };

test('pickHeal takes the nearest option by ratio, spell first on a tie, nothing far off', () => {
  const spell = { source: 'spell' as const, share: 0.1 };
  const potion = { source: 'item' as const, def: 'HealthPotion', share: 0.4 };
  assert.equal(pickHeal(0.38, [spell, potion]), potion);
  assert.equal(pickHeal(0.12, [spell, potion]), spell);
  assert.equal(pickHeal(0.2, [spell, { ...potion, share: 0.4 }]), spell, 'equal ratio: the spell wins');
  assert.equal(pickHeal(0.25, [spell]), null, 'two and a half times the spell is not the spell');
  assert.equal(pickHeal(0, [spell]), null);
  assert.equal(pickHeal(NaN, [spell]), null);
  assert.equal(pickHeal(0.4, []), null);
});

test('heal protocol frame is checked', () => {
  assert.deepEqual(decode('{"t":"heal","share":0.4,"key":"abcdefgh1"}'), { t: 'heal', share: 0.4, key: 'abcdefgh1' });
  for (const bad of ['{"t":"heal","share":0,"key":"abcdefgh1"}', '{"t":"heal","share":5,"key":"abcdefgh1"}', '{"t":"heal","share":0.4}'])
    assert.throws(() => decode(bad), ProtocolError);
});

test('a reported heal spends a carried potion once, or Heal Life, or is refused', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  const sim = new CombatSim(REGION, 'TestRegion#h', {
    data: D,
    spawns: { regions: {} },
    events: app.sink,
    economy: app.economy,
    progress: app.progress,
    loadout: () => ({ willLevel: 1, spells: { heal_life: 1 } }),
  });
  sim.addHero(hero.id, { x: 0, y: 0, z: 0 });
  const h = sim.hero(hero.id)!;
  const max = h.maxHealth;

  // Nothing carried, and a heal far bigger than Heal Life: refused, health unchanged.
  h.health = 10;
  assert.deepEqual(claimHeal(sim.healing, app.economy, hero.id, 0.9, key()), { ok: false, reason: 'no_match' });
  assert.equal(h.health, 10);

  // A potion in the bank cannot be drunk; one in the bag can, once per key.
  app.economy.mintItem(hero.id, 'HealthPotion', 1, 'test', key(), 'gm:1');
  const bankId = app.economy.inventory(hero.id)[0]!.id;
  app.economy.moveItem(hero.id, bankId, 'bank', key());
  const banked = claimHeal(sim.healing, app.economy, hero.id, 0.4, key());
  assert.equal(banked.ok, false, JSON.stringify({ banked, max, inv: app.economy.inventory(hero.id) }));
  app.economy.mintItem(hero.id, 'HealthPotion', 2, 'test', key(), 'gm:1');
  const k = key();
  const r = claimHeal(sim.healing, app.economy, hero.id, 0.4, k);
  assert.equal(r.ok, true);
  assert.equal(r.source, 'item');
  assert.equal(h.health, Math.min(max, 10 + 0.4 * max));
  assert.equal(claimHeal(sim.healing, app.economy, hero.id, 0.4, k).replayed, true);
  assert.equal(app.economy.countOf(hero.id, 'HealthPotion'), 2, 'one of three used; the replay used none');
  assert.equal(app.economy.audit().ok, true);

  // A small heal matches Heal Life, which costs Will instead.
  h.health = 10;
  const will = h.will;
  const spellShare = 15 / max;
  const s = claimHeal(sim.healing, app.economy, hero.id, spellShare, key());
  assert.equal(s.source, 'spell');
  assert.ok(h.will < will);
  assert.ok(h.health > 10);

  // A dead hero heals nothing.
  h.alive = false;
  assert.deepEqual(claimHeal(sim.healing, app.economy, hero.id, 0.4, key()), { ok: false, reason: 'dead' });
});
