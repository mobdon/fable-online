/**
 * A town's ground: where its guards keep the peace and come running to a fight. Built from
 * the village outlines in town-data.json (see PlacedVillage.zone); positions are the region
 * frame, like everything else the sim holds.
 */
export interface TownZone {
  /** The village's UID in the level, for logs and tests. */
  village: string;
  /** Convex outline, counter-clockwise. */
  points: { x: number; y: number }[];
}

/** True if p is inside the outline, or within `margin` metres of it. */
export function inZone(p: { x: number; y: number }, z: TownZone, margin = 0): boolean {
  const pts = z.points;
  if (pts.length < 3) return false;
  let inside = false;
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
    const a = pts[i]!;
    const b = pts[j]!;
    if (a.y > p.y !== b.y > p.y && p.x < ((b.x - a.x) * (p.y - a.y)) / (b.y - a.y) + a.x) inside = !inside;
  }
  if (inside || margin <= 0) return inside;
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) if (segDist(p, pts[j]!, pts[i]!) <= margin) return true;
  return false;
}

export const inAnyZone = (p: { x: number; y: number }, zones: readonly TownZone[], margin = 0): boolean => zones.some((z) => inZone(p, z, margin));

function segDist(p: { x: number; y: number }, a: { x: number; y: number }, b: { x: number; y: number }): number {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const l2 = dx * dx + dy * dy;
  const t = l2 === 0 ? 0 : Math.max(0, Math.min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2));
  return Math.hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}
