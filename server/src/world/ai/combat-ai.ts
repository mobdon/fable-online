import type { AiContext, ChannelAi } from '../combat-sim.js';
import type { Creature as SharedCreature } from '../entities.js';
import type { NavStore } from '../nav/navmesh.js';
import { AiDirector } from './brain.js';
import type { AttackKind, Combatant, Creature } from './types.js';

/**
 * Runs the monster AI inside a CombatSim channel. Combat owns the creatures, the timers
 * (cooldown, stun) and whether a swing lands; the AI moves creatures and decides when to
 * swing. The region's navmesh is used when one was extracted, otherwise creatures move in
 * straight lines.
 */
export function monsterAi(nav?: NavStore): (ctx: AiContext) => ChannelAi {
  return (ctx) => {
    const mesh = nav?.get(ctx.region.name);
    const creatures = new Map<number, Creature>();
    const director = new AiDirector(
      {
        nav: mesh,
        creatures,
        targets: () => ctx.heroes() as Iterable<Combatant>,
        combat: {
          // Combat has no separate heavy swing: Fable's equivalent is the charged flourish.
          attack: (a, t, kind: AttackKind) => void ctx.combat.attack(a, t, kind === 'heavy' ? 'flourish' : kind),
          setBlocking: (id, on) => ctx.combat.setBlocking(id, on),
        },
      },
      hash(ctx.channelId),
    );
    return {
      onSpawn(shared: SharedCreature) {
        const c = shared as unknown as Creature;
        // game.bin stats live on the creature itself; the AI profile reads them from .stats.
        c.stats = {
          walkSpeed: shared.walkSpeed,
          runSpeed: shared.runSpeed,
          sightRange: shared.sightRange,
          attackRange: shared.attackRange,
          flees: shared.flees,
        };
        if (mesh && !mesh.walkable(c.pos.x, c.pos.y)) {
          const p = mesh.snap(c.pos, 8);
          if (p) c.pos = p;
        }
        creatures.set(c.id, c);
        director.onSpawn(c);
      },
      onDamaged: (id, by, n) => director.onDamaged(id, by, n),
      onDeath(id) {
        director.onDeath(id);
      },
      step(dtMs) {
        // Respawns come back as new creature objects; forget brains whose creature is gone.
        const live = new Set<number>();
        for (const c of ctx.creatures()) live.add(c.id);
        for (const id of creatures.keys()) {
          if (!live.has(id)) {
            creatures.delete(id);
            director.remove(id);
          }
        }
        director.step(dtMs);
      },
      dispose() {
        for (const id of creatures.keys()) director.remove(id);
        creatures.clear();
      },
    };
  };
}

function hash(s: string): number {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}
