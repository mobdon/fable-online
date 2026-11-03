// Load test for server-run creatures on a real region (needs shared/defs/nav from the extractor).
//   npm run ai-bench -- [Region] [creatures] [heroes]
import path from 'node:path';
import { NavStore, seededRng } from '../src/world/nav/navmesh.js';
import { NavSim } from '../src/world/nav-sim.js';

const [region = 'Greatwood', nC = '200', nH = '48'] = process.argv.slice(2);
const nav = new NavStore(path.resolve(import.meta.dirname, '../../shared/defs/nav')).get(region);
if (!nav) {
  console.error(`no navmesh for ${region}: run tools/extractor/navmesh.mjs first`);
  process.exit(1);
}
const sim = new NavSim(nav, undefined, 42);
const rng = seededRng(9);
const big = nav.cells.reduce((a, b) => (b.half > a.half ? b : a)); // spawn on the main ground
const pick = () => nav.randomPointNear(big, 60, rng) ?? big;
const kinds = ['CREATURE_BANDIT_GRUNT', 'CREATURE_HOBBE', 'CREATURE_WOLF', 'CREATURE_BALVERINE', 'CREATURE_BANDIT_ARCHER'];
for (let i = 0; i < Number(nC); i++) sim.spawn(kinds[i % kinds.length]!, { ...pick(), z: 0 });
for (let i = 1; i <= Number(nH); i++) sim.addHero(i, { ...pick(), z: 0 });
const TICK = 1000 / 15;
const times: number[] = [];
for (let t = 0; t < 900; t++) {
  for (let i = 1; i <= Number(nH); i++)
    sim.applyInput(i, { seq: t, moveX: 0, moveY: rng() < 0.7 ? 1 : 0, buttons: 0, cameraYaw: (i + t / 60) % (Math.PI * 2) });
  const s = performance.now();
  sim.step(TICK);
  times.push(performance.now() - s);
}
times.sort((a, b) => a - b);
const states = new Map<string, number>();
for (const c of sim.creatures.values()) states.set(sim.ai.state(c.id)!, (states.get(sim.ai.state(c.id)!) ?? 0) + 1);
console.log(`${region}: ${nav.cells.length} cells, ${nC} creatures, ${nH} heroes, 60 s at 15 Hz`);
console.log(`tick p50 ${times[450]!.toFixed(2)} ms, p99 ${times[891]!.toFixed(2)} ms, max ${times[899]!.toFixed(2)} ms`);
console.log('creature states:', Object.fromEntries(states));
