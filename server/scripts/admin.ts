/**
 * Small admin jobs on the server's database, for test realms. Run them while the hero is
 * offline: an online hero's position and region are rewritten by the server when they leave.
 *
 *   npm run admin -- list                         every hero, where they are and their life stage
 *   npm run admin -- graduate <Name>              move a hero straight to the shared world
 *   npm run admin -- place <Name> <Region> [x y z]  put a hero in a region (0 0 0 = the next report anchors)
 *
 * Uses FO_DB like the server (default data/fable-online.sqlite).
 */
import { Database } from '../src/db/database.js';
import { STAGE_REGIONS } from '../src/world/regions.js';

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
  default:
    console.log('Usage: npm run admin -- list | graduate <Name> | place <Name> <Region> [x y z]');
}
db.close();
