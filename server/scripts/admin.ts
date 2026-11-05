/**
 * Small admin jobs on the server's database, for test realms. Run them while the hero is
 * offline: an online hero's position and region are rewritten by the server when they leave.
 *
 *   npm run admin -- list                         every hero, where they are and their life stage
 *   npm run admin -- graduate <Name>              move a hero straight to the shared world
 *   npm run admin -- place <Name> <Region> [x y z]  put a hero in a region (0 0 0 = the next report anchors)
 *   npm run admin -- password <Account> <New>     set an account's password and log it out everywhere
 *   npm run admin -- role <Account> <player|gm|admin>  set an account's role (gm and admin get the dev tools)
 *
 * Uses FO_DB like the server (default data/fable-online.sqlite).
 */
import { Database } from '../src/db/database.js';
import { STAGE_REGIONS } from '../src/world/regions.js';
import { hashPassword } from '../src/accounts/accounts.js';

const db = new Database(process.env.FO_DB || 'data/fable-online.sqlite');
const [cmd, name, ...rest] = process.argv.slice(2);

interface Row {
  id: number;
  name: string;
  phase: string;
  region: string;
  x: number;
  y: number;
  z: number;
  last_played_at: number | null;
}

const find = (n: string | undefined): Row => {
  const h = n ? db.get<Row>('SELECT * FROM heroes WHERE name = ?', n) : undefined;
  if (!h) {
    console.error(`No hero called "${n ?? ''}". Try: npm run admin -- list`);
    process.exit(1);
  }
  return h;
};

switch (cmd) {
  case 'list': {
    const rows = db.all<Row>('SELECT * FROM heroes ORDER BY id');
    if (!rows.length) console.log('No heroes yet.');
    for (const h of rows)
      console.log(
        `${String(h.id).padStart(4)}  ${h.name.padEnd(16)} ${h.phase.padEnd(15)} ${h.region} (${h.x.toFixed(1)}, ${h.y.toFixed(1)}, ${h.z.toFixed(1)})` +
          (h.last_played_at ? `  last played ${new Date(h.last_played_at).toISOString().slice(0, 16).replace('T', ' ')}` : ''),
      );
    break;
  }
  case 'graduate': {
    const h = find(name);
    db.run("UPDATE heroes SET phase = 'world', region = ?, x = 0, y = 0, z = 0 WHERE id = ?", STAGE_REGIONS.world, h.id);
    console.log(`${h.name} is now in the shared world (${STAGE_REGIONS.world}).`);
    break;
  }
  case 'place': {
    const h = find(name);
    const region = rest[0];
    if (!region) {
      console.error('Which region? For example: npm run admin -- place Mob BowerstoneSouth');
      process.exit(1);
    }
    const [x, y, z] = [rest[1], rest[2], rest[3]].map((v) => Number(v ?? 0));
    if (![x, y, z].every(Number.isFinite)) {
      console.error('x y z must be numbers');
      process.exit(1);
    }
    db.run('UPDATE heroes SET region = ?, x = ?, y = ?, z = ? WHERE id = ?', region, x, y, z, h.id);
    console.log(`${h.name} will join in ${region} at (${x}, ${y}, ${z}).`);
    break;
  }
  case 'password': {
    const acct = name ? db.get<{ id: number; username: string }>('SELECT id, username FROM accounts WHERE username = ?', name) : undefined;
    if (!acct) {
      const all = db.all<{ username: string }>('SELECT username FROM accounts ORDER BY id').map((a) => a.username);
      console.error(`No account called "${name ?? ''}". Accounts: ${all.join(', ') || 'none yet'}`);
      process.exit(1);
    }
    const pw = rest[0] ?? '';
    if (pw.length < 8 || pw.length > 128) {
      console.error('The new password must be 8-128 characters.');
      process.exit(1);
    }
    db.run('UPDATE accounts SET password_hash = ? WHERE id = ?', hashPassword(pw), acct.id);
    db.run('DELETE FROM sessions WHERE account_id = ?', acct.id);
    console.log(`${acct.username} has a new password. Log in with it in the launcher.`);
    break;
  }
  case 'role': {
    const acct = name ? db.get<{ id: number; username: string }>('SELECT id, username FROM accounts WHERE username = ?', name) : undefined;
    const role = rest[0] ?? '';
    if (!acct || !['player', 'gm', 'admin'].includes(role)) {
      console.error('Usage: npm run admin -- role <Account> <player|gm|admin>');
      process.exit(1);
    }
    db.run('UPDATE accounts SET role = ? WHERE id = ?', role, acct.id);
    console.log(`${acct.username} is now ${role}. Dev tools work for it straight away.`);
    break;
  }
  default:
    console.log('Usage: npm run admin -- list | graduate <Name> | place <Name> <Region> [x y z] | password <Account> <New> | role <Account> <player|gm|admin>');
}
db.close();
