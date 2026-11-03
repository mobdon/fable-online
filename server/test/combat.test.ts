import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CombatSystem } from '../src/combat/combat.js';
import { creatureFromDef, heroCombatant } from '../src/combat/loadout.js';
import { BALANCE, armourPoints, armourReduction } from '../src/combat/rules.js';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import type { Hero } from '../src/world/entities.js';

// yaw 0 faces +y. Put targets 2 m ahead on +y.
const at = (x: number, y: number) => ({ x, y, z: 0 });
const sword = { equipped: ['OBJECT_IRON_LONGSWORD'] };

function setup() {
  const combat = new CombatSystem(D);
  const hero = heroCombatant(1, at(0, 0), D, sword);
  const bandit = creatureFromDef(1_000_001, 'CREATURE_BANDIT_GRUNT_LEVEL1', at(0, 2), D);
  bandit.health = bandit.maxHealth = 1000; // survive a few hits
  combat.add(hero);
  combat.add(bandit);
  return { combat, hero, bandit };
}

test('a swing in reach and in front hits; behind or too far misses', () => {
  const { combat, hero, bandit } = setup();
  const r = combat.attack(hero.id, null, 'light');
  assert.ok(r.ok && r.hit, 'hit in front');
  assert.equal(bandit.health, bandit.maxHealth - (r as { damage: number }).damage);

  combat.step(1000);
  hero.yaw = Math.PI; // turn around
  assert.deepEqual(combat.attack(hero.id, null, 'light'), { ok: true, hit: false, reason: 'miss' });

  combat.step(1000);
  hero.yaw = 0;
  bandit.pos.y = 10;
  assert.deepEqual(combat.attack(hero.id, bandit.id, 'light'), { ok: true, hit: false, reason: 'miss' });
});

test('cooldowns stop button mashing', () => {
  const { combat, hero } = setup();
  assert.ok(combat.attack(hero.id, null, 'light').ok);
  assert.deepEqual(combat.attack(hero.id, null, 'light'), { ok: false, reason: 'cooldown' });
  combat.step(BALANCE.cooldownMs.light!);
  assert.ok(combat.attack(hero.id, null, 'light').ok);
});

test('weapon damage comes from the data and scales with Strength', () => {
  const a = setup();
  const r1 = a.combat.attack(a.hero.id, null, 'light') as { damage: number };
  assert.equal(r1.damage, Math.round(D.items.OBJECT_IRON_LONGSWORD!.weapon!.damage * BALANCE.weaponScale));

  const combat = new CombatSystem(D);
  const strong = heroCombatant(2, at(0, 0), D, { ...sword, strength: 4 });
  const bandit = creatureFromDef(1_000_002, 'CREATURE_BANDIT_GRUNT_LEVEL1', at(0, 2), D);
  combat.add(strong);
  combat.add(bandit);
  const r2 = combat.attack(strong.id, null, 'light') as { damage: number };
  assert.ok(r2.damage > r1.damage);
});

test('a front block soaks most of a swing; a flourish breaks the block and knocks down', () => {
  const { combat, hero, bandit } = setup();
  bandit.yaw = Math.PI; // facing the hero
  combat.setBlocking(bandit.id, true);
  const r = combat.attack(hero.id, null, 'light') as { damage: number; blocked: boolean };
  assert.equal(r.blocked, true);
  const full = D.items.OBJECT_IRON_LONGSWORD!.weapon!.damage * BALANCE.weaponScale;
  assert.equal(r.damage, Math.round(full * BALANCE.blockLeak.light!));

  // Flourish needs a full charge first.
  combat.step(2000);
  assert.deepEqual(combat.attack(hero.id, null, 'flourish'), { ok: false, reason: 'not_allowed' });
  combat.step(BALANCE.flourishChargeMs, new Set([hero.id]));
  const f = combat.attack(hero.id, null, 'flourish') as { damage: number; blocked: boolean };
  assert.equal(f.blocked, false);
  assert.equal(bandit.blocking, false);
  assert.ok(bandit.stunnedMs >= BALANCE.flourishStunMs - 1);
});

test('a block from behind does nothing', () => {
  const { combat, hero, bandit } = setup();
  bandit.yaw = 0; // back turned to the hero
  combat.setBlocking(bandit.id, true);
  const r = combat.attack(hero.id, null, 'light') as { blocked: boolean };
  assert.equal(r.blocked, false);
});

test('no stun-lock: after a knockdown the target is immune for a while', () => {
  const { combat, hero, bandit } = setup();
  bandit.health = bandit.maxHealth = 10_000;
  combat.step(BALANCE.flourishChargeMs, new Set([hero.id]));
  combat.attack(hero.id, null, 'flourish');
  combat.step(BALANCE.flourishStunMs + 1);
  assert.equal(bandit.stunnedMs, 0);
  combat.attack(hero.id, null, 'light');
  assert.equal(bandit.stunnedMs, 0, 'immune right after getting up');
});

test('armour curve: reduction = A / (A + K), capped, staff platinum fixed', () => {
  assert.equal(armourReduction(0), 0);
  const fullPlate = armourPoints([{ coverage: 1, armour: 'ARMOUR_HERO_PLATE_ARMOUR' }]);
  assert.equal(fullPlate, 938 - 382);
  assert.equal(Math.round(armourReduction(fullPlate) * 100), 50);
  assert.equal(armourReduction(1e9), BALANCE.armourCap);
  assert.equal(armourReduction(0, BALANCE.staffReduction), 0.85);
});

test('platinum only works for staff; anyone else gets it ignored and reported', () => {
  const issues: string[] = [];
  const player = heroCombatant(5, at(0, 0), D, { equipped: ['OBJECT_HERO_HELMET_PLATINUM'] }, (i) => issues.push(i.reason));
  assert.equal(player.armourFixed, undefined);
  assert.deepEqual(issues, ['staff_only']);
  const gm = heroCombatant(6, at(0, 0), D, { equipped: ['OBJECT_HERO_HELMET_PLATINUM'], staff: true });
  assert.equal(gm.armourFixed, BALANCE.staffReduction);
});

test('PvP: heroes only hurt each other when both have PvP on, at reduced damage', () => {
  const combat = new CombatSystem(D);
  const a = heroCombatant(1, at(0, 0), D, sword);
  const b = heroCombatant(2, at(0, 2), D, sword);
  combat.add(a);
  combat.add(b);
  assert.deepEqual(combat.attack(1, null, 'light'), { ok: true, hit: false, reason: 'miss' });
  a.pvp = b.pvp = true;
  combat.step(1000);
  const r = combat.attack(1, null, 'light') as { damage: number };
  assert.equal(r.damage, Math.round(D.items.OBJECT_IRON_LONGSWORD!.weapon!.damage * BALANCE.weaponScale * BALANCE.pvpScale));
});

test('Will spells cost Will, respect levels and cooldowns, and heal/shield work', () => {
  const combat = new CombatSystem(D);
  const h = heroCombatant(1, at(0, 0), D, { willLevel: 2, spells: { fireball: 2, heal_life: 1, physical_shield: 1 } }) as Hero;
  const wolf = creatureFromDef(1_000_003, 'CREATURE_WOLF', at(0, 10), D);
  combat.add(h);
  combat.add(wolf);
  assert.deepEqual(combat.cast(1, 'lightning', null), { ok: false, reason: 'not_allowed' }, 'not learned');
  const will0 = h.will;
  const r = combat.cast(1, 'fireball', null) as { hit: boolean; damage: number };
  assert.equal(r.hit, true);
  assert.equal(h.will, will0 - 20);
  assert.deepEqual(combat.cast(1, 'fireball', null), { ok: false, reason: 'cooldown' });

  h.health = 10;
  combat.cast(1, 'heal_life', null);
  assert.equal(h.health, 25);
  combat.cast(1, 'physical_shield', null);
  assert.equal(h.shield, 30);
  h.will = 0;
  combat.step(5000);
  assert.deepEqual(combat.cast(1, 'fireball', null), { ok: false, reason: 'no_will' });
});

test('death fires once with damage credit; dead things cannot act', () => {
  const { combat, hero, bandit } = setup();
  const deaths: number[] = [];
  combat.onDeath((d) => {
    deaths.push(d.victim.id);
    assert.ok((d.damageBy.get(hero.id) ?? 0) > 0);
  });
  bandit.health = 1;
  const r = combat.attack(hero.id, null, 'light') as { killed: boolean };
  assert.equal(r.killed, true);
  assert.equal(bandit.alive, false);
  assert.deepEqual(deaths, [bandit.id]);
  assert.deepEqual(combat.attack(bandit.id, hero.id, 'light'), { ok: false, reason: 'dead' });
  combat.step(1000);
  assert.deepEqual(combat.attack(hero.id, bandit.id, 'light'), { ok: true, hit: false, reason: 'miss' }, 'corpses take no hits');
});

test('creatures hit heroes, and armour reduces it', () => {
  const combat = new CombatSystem(D);
  const naked = heroCombatant(1, at(0, 0), D);
  const armoured = heroCombatant(2, at(10, 0), D, { equipped: ['OBJECT_HERO_SHIRT_CHAINMAIL'] });
  const b1 = creatureFromDef(1_000_010, 'CREATURE_BANDIT_GRUNT_LEVEL1', at(0, 2), D);
  const b2 = creatureFromDef(1_000_011, 'CREATURE_BANDIT_GRUNT_LEVEL1', at(10, 2), D);
  b1.yaw = b2.yaw = Math.PI;
  for (const c of [naked, armoured, b1, b2]) combat.add(c);
  const h1 = combat.attack(b1.id, naked.id, 'light') as { damage: number };
  const h2 = combat.attack(b2.id, armoured.id, 'light') as { damage: number };
  assert.ok(h1.damage > 0);
  assert.ok(h2.damage < h1.damage, `armour should help: ${h2.damage} vs ${h1.damage}`);
});
