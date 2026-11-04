import { test } from 'node:test';
import assert from 'node:assert/strict';
// @ts-expect-error plain .mjs extractor
import { parseWld, regionFrames } from '../../tools/extractor/things.mjs';
// @ts-expect-error plain .mjs extractor
import { groupByRegion } from '../../tools/extractor/spawns.mjs';

// Three levels, in .wld order C, A, B; region R contains A, B, C with no level named R,
// and region A2 contains a level named A2. Shapes match FinalAlbion.wld.
const WLD = `
NewMap 1; LevelName "FinalAlbion\\\\C.lev"; LevelScriptName "C"; MapUID 11; MapX 1000; MapY 2000; EndMap;
NewMap 2; LevelName "FinalAlbion\\\\A.lev"; LevelScriptName "A"; MapUID 12; MapX 1032; MapY 2064; EndMap;
NewMap 3; LevelName "FinalAlbion\\\\B.lev"; LevelScriptName "B"; MapUID 13; MapX 1096; MapY 2000; EndMap;
NewMap 4; LevelName "FinalAlbion\\\\A2.lev"; LevelScriptName "A2"; MapUID 14; MapX 500; MapY 500; EndMap;
NewMap 5; LevelName "FinalAlbion\\\\A2_Leadout.lev"; LevelScriptName "A2_Leadout"; MapUID 15; MapX 532; MapY 500; EndMap;
NewRegion 1; RegionName "R"; ContainsMap "FinalAlbion\\\\A.lev"; ContainsMap "FinalAlbion\\\\B.lev"; ContainsMap "FinalAlbion\\\\C.lev"; EndRegion;
NewRegion 2; RegionName "A2"; ContainsMap "FinalAlbion\\\\A2_Leadout.lev"; ContainsMap "FinalAlbion\\\\A2.lev"; EndRegion;
`;

test('region frames: anchor is the level named like the region, else the first in .wld order (as RegionTable.origin)', () => {
  const { levelInfo, regionAnchor } = regionFrames(parseWld(WLD));
  assert.equal(regionAnchor.R, 'C', 'first in .wld order, not first in ContainsMap order');
  assert.equal(regionAnchor.A2, 'A2');
  assert.deepEqual([levelInfo.A.dx, levelInfo.A.dy], [32, 64]);
  assert.deepEqual([levelInfo.B.dx, levelInfo.B.dy], [96, 0]);
  assert.deepEqual([levelInfo.A2_Leadout.dx, levelInfo.A2_Leadout.dy], [32, 0]);
});

test('spawns are grouped by region and moved into its frame; uids carry the level', () => {
  const { levelInfo } = regionFrames(parseWld(WLD));
  const c = (uid: string, x: number, y: number) => ({ uid, def: 'CREATURE_X', x, y, z: 1, yaw: 0, script: null });
  const regions = groupByRegion(
    [
      { script: 'A', creatures: [c('7', 10, 10)], generators: [] },
      { script: 'C', creatures: [c('7', 5, 5)], generators: [] },
      { script: 'B', creatures: [], generators: [{ ...c('9', 1, 2), families: [], radius: 5, triggerRadius: 0, activeLimit: 2, totalLimit: -1 }] },
      { script: 'Loose', creatures: [c('1', 3, 3)], generators: [] },
    ],
    levelInfo,
  );
  assert.deepEqual(Object.keys(regions).sort(), ['Loose', 'R']);
  assert.deepEqual(
    regions.R.creatures.map((x: any) => [x.uid, x.x, x.y]),
    [
      ['A:7', 42, 74],
      ['C:7', 5, 5],
    ],
  );
  assert.deepEqual([regions.R.generators[0].uid, regions.R.generators[0].x, regions.R.generators[0].y], ['B:9', 97, 2]);
  assert.deepEqual([regions.Loose.creatures[0].x, regions.Loose.creatures[0].y], [3, 3], 'no region: own frame');
});
