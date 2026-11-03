import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testApp } from './helpers.js';
import { GameError } from '../src/errors.js';

const code = (c: string) => (e: unknown) => e instanceof GameError && e.code === c;

test('register, login, rotating refresh, logout', () => {
  const app = testApp();
  app.accounts.register('Sparrow_1', 'correct horse');
  assert.throws(() => app.accounts.register('sparrow_1', 'whatever12'), code('username_taken'));
  assert.throws(() => app.accounts.register('x', 'whatever12'), code('bad_username'));
  assert.throws(() => app.accounts.login('Sparrow_1', 'wrong pass'), code('bad_credentials'));
  const t = app.accounts.login('sparrow_1', 'correct horse');
  assert.equal(app.accounts.authenticate(t.access_token).username, 'Sparrow_1');
  const t2 = app.accounts.refresh(t.refresh_token);
  assert.throws(() => app.accounts.refresh(t.refresh_token), code('bad_token')); // rotated: old one is dead
  app.accounts.logout(t2.access_token);
  assert.throws(() => app.accounts.authenticate(t2.access_token), code('bad_token'));
  assert.throws(() => app.accounts.refresh(t2.refresh_token), code('bad_token'));
});

test('join tickets are single use', () => {
  const app = testApp();
  const a = app.accounts.register('Thorne', 'password123');
  const h = app.heroes.create(a.id, 'Thorne');
  const ticket = app.accounts.issueJoinTicket(a.id, h.id, 'albion-1');
  assert.deepEqual(app.accounts.redeemJoinTicket(ticket), { accountId: a.id, heroId: h.id, realm: 'albion-1' });
  assert.throws(() => app.accounts.redeemJoinTicket(ticket), code('bad_token'));
});

test('login attempts are rate limited', () => {
  const app = testApp();
  app.accounts.register('Brute', 'password123');
  for (let i = 0; i < 10; i++) assert.throws(() => app.accounts.login('Brute', 'nope', '1.2.3.4'), code('bad_credentials'));
  assert.throws(() => app.accounts.login('Brute', 'password123', '1.2.3.4'), code('rate_limited'));
});

test('heroes: names, limits, life stages only move forward', () => {
  const app = testApp({ maxHeroesPerAccount: 2 });
  const a = app.accounts.register('Maze', 'password123');
  const h = app.heroes.create(a.id, 'Whisper');
  assert.equal(h.phase, 'childhood');
  assert.equal(app.economy.balance(h.id), 0);
  assert.throws(() => app.heroes.create(a.id, 'whisper'), code('bad_name'));
  assert.throws(() => app.heroes.create(a.id, 'Whisper'), code('name_taken'));
  app.heroes.create(a.id, 'Briar');
  assert.throws(() => app.heroes.create(a.id, 'Lady'), code('hero_limit'));
  assert.throws(() => app.heroes.advancePhase(h.id, 'world'), code('bad_phase'));
  app.heroes.advancePhase(h.id, 'guild_training');
  assert.equal(app.heroes.advancePhase(h.id, 'world').region, 'HeroGuildComplex');
  assert.throws(() => app.heroes.advancePhase(h.id, 'childhood'), code('bad_phase'));
});
