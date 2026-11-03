/**
 * Test setup for Fable's in-game scoreboard and clan screens.
 *   npm run scoreboard-setup -- <username> <password>
 * Creates the account if it doesn't exist, prints the scoreboard code to type into Fable's
 * scoreboard login, and makes a "Testers" clan with a generated logo that the account joins.
 * Uses the same database as `npm run dev` (FO_DB).
 */
import jpeg from 'jpeg-js';
import { createApp } from '../src/app.js';
import { NullSink } from '../src/analytics/events.js';

const [username, password] = process.argv.slice(2);
if (!username || !password) {
  console.error('usage: npm run scoreboard-setup -- <username> <password>');
  process.exit(1);
}
const app = createApp({ eventsPath: '' }, { events: new NullSink() });
try {
  const found = app.db.get<{ id: number }>('SELECT id FROM accounts WHERE username = ?', username);
  const accountId = found?.id ?? app.accounts.register(username, password).id;
  if (found) app.accounts.login(username, password); // throws on a wrong password
  const { username: name, code } = app.scoreboard.rotateCode(accountId);

  const clan = app.scoreboard.clanByName('Testers') ?? app.scoreboard.createClan('Testers', 'Scoreboard test clan');
  app.scoreboard.addMember(clan.id, accountId);
  if (!clan.logo_jpeg) app.scoreboard.setLogo(clan.id, testLogo());

  console.log('\nIn Fable: Options > Scoreboard, then log in with');
  console.log(`  user name: ${name}`);
  console.log(`  password:  ${code}`);
  console.log('\nYou are in the clans "Chapters of the Lost" and "Testers" (with a test logo).\n');
} finally {
  app.close();
}

/** A shield-like gold-on-red test picture, drawn in code (no game assets). */
function testLogo(): Buffer {
  const s = 128;
  const data = Buffer.alloc(s * s * 4);
  for (let y = 0; y < s; y++)
    for (let x = 0; x < s; x++) {
      const i = (y * s + x) * 4;
      const dx = Math.abs(x - s / 2);
      const inShield = y > 12 && y < 116 && dx < 48 - Math.max(0, y - 70) * 0.9;
      const band = inShield && Math.abs(x - s / 2) < 8;
      const cross = inShield && Math.abs(y - 52) < 8;
      const gold = band || cross || (inShield && 48 - Math.max(0, y - 70) * 0.9 - dx < 5);
      const [r, g, b] = !inShield ? [24, 20, 16] : gold ? [214, 170, 72] : [128, 24, 20];
      data[i] = r!;
      data[i + 1] = g!;
      data[i + 2] = b!;
      data[i + 3] = 255;
    }
  return Buffer.from(jpeg.encode({ width: s, height: s, data }, 95).data);
}
