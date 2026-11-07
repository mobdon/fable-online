// One realm clock and sky: the staff time and weather tools, and every player getting the
// same sky frame when they change.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WorldClock } from '../src/world/clock.js';
import { decode } from '../src/net/protocol.js';
import { listen } from '../src/net/server.js';
import { joinAs, testApp, wsClient } from './helpers.js';

test('clock: setHour moves the realm forward to that hour, never back', () => {
  let now = Date.UTC(2026, 10, 4, 12);
  const c = new WorldClock({}, () => now);
  const day = c.day();
  c.setHour(22);
  assert.ok(Math.abs(c.hour() - 22) < 1e-6);
  assert.equal(c.isNight(), true);
  // 06:00 is earlier on the clock face, so it is tomorrow morning: the day count goes up.
  c.setHour(6);
  assert.ok(Math.abs(c.hour() - 6) < 1e-6);
  assert.equal(c.day(), day + 1);
  // The clock keeps running from there at the normal speed.
  now += c.opts.dayMs / 24;
  assert.ok(Math.abs(c.hour() - 7) < 1e-6);
  assert.throws(() => c.setHour(25));
});

test('clock: forced weather covers outdoor regions until it runs out, never interiors', () => {
  let now = Date.UTC(2026, 10, 4, 12);
  const c = new WorldClock({}, () => now);
  const before = c.skyKey('Oakvale');
  c.forceWeather('storm', 60_000);
  assert.notEqual(c.skyKey('Oakvale'), before, 'a staff change must make channels resend');
  for (const r of ['Oakvale', 'Darkwood_4', 'Snowspire', 'HookCoast']) {
    const s = c.sky(r);
    assert.equal(s.weather, 'storm', r);
    assert.equal(s.intensity, 1);
    assert.equal(s.until, now + 60_000);
  }
  assert.equal(c.sky('OakvaleTavernInside').weather, 'clear');
  now += 61_000;
  const rolled = new WorldClock({}, () => now).weatherAt('Oakvale');
  assert.deepEqual(c.weatherAt('Oakvale'), rolled, 'back to the seeded rolls afterwards');
  c.forceWeather('fog');
  c.forceWeather('rain');
  assert.equal(c.sky('Oakvale').previous, 'fog', 'a force replacing a force fades from it');
  c.forceWeather(null);
  assert.deepEqual(c.weatherAt('Oakvale'), rolled);
});

test('protocol: dev_time takes an hour 0-24, dev_weather a known weather or auto', () => {
  assert.deepEqual(decode(JSON.stringify({ t: 'dev_time', hour: 21.5 })), { t: 'dev_time', hour: 21.5 });
  assert.throws(() => decode(JSON.stringify({ t: 'dev_time', hour: 30 })));
  assert.throws(() => decode(JSON.stringify({ t: 'dev_time' })));
  assert.deepEqual(decode(JSON.stringify({ t: 'dev_weather', weather: 'rain' })), { t: 'dev_weather', weather: 'rain', minutes: 10 });
  assert.deepEqual(decode(JSON.stringify({ t: 'dev_weather', weather: 'auto', minutes: 5 })), { t: 'dev_weather', weather: 'auto', minutes: 5 });
  assert.throws(() => decode(JSON.stringify({ t: 'dev_weather', weather: 'hail' })));
});

test('every player gets the same sky; staff can change it for everyone, players cannot', async () => {
  const app = testApp({ tickRate: 30 });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    const one = await joinAs(app, base, 'Mobdon');
    const two = await joinAs(app, base, 'Scythe');
    app.db.run("UPDATE accounts SET role = 'gm' WHERE id = (SELECT account_id FROM heroes WHERE id = ?)", one.hero.id);

    const a = await wsClient(`ws://127.0.0.1:${port}/play`);
    const b = await wsClient(`ws://127.0.0.1:${port}/play`);
    a.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket });
    b.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: two.ticket });
    const [sa, sb] = await Promise.all([a.next((m) => m.t === 'sky'), b.next((m) => m.t === 'sky')]);
    assert.equal(sa.day_ms, sb.day_ms);
    assert.ok(Math.abs(sa.day_fraction - sb.day_fraction) < 0.001, 'same time of day for both');

    // A player account is refused and nothing changes.
    b.send({ t: 'dev_time', hour: 3 });
    const refused = await b.next((m) => m.t === 'dev_result');
    assert.equal(refused.ok, false);

    // Staff sets 22:00: both players get a night sky straight away.
    a.send({ t: 'dev_time', hour: 22 });
    const ok = await a.next((m) => m.t === 'dev_result');
    assert.equal(ok.ok, true);
    assert.match(ok.message, /22:00/);
    const night = (m: any) => m.t === 'sky' && m.night === true && Math.abs(m.day_fraction - 22 / 24) < 0.002;
    await Promise.all([a.next(night), b.next(night)]);

    a.send({ t: 'dev_weather', weather: 'rain', minutes: 5 });
    await a.next((m) => m.t === 'dev_result' && m.ok && /rain/.test(m.message));
    const rain = (m: any) => m.t === 'sky' && (m.climate === 'indoor' ? m.weather === 'clear' : m.weather === 'rain');
    await Promise.all([a.next(rain), b.next(rain)]);
    a.ws.close();
    b.ws.close();
    await new Promise((r) => setTimeout(r, 100));
  } finally {
    server.close();
    app.close();
  }
});
