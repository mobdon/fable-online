import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import http from 'node:http';
import type { AddressInfo } from 'node:net';
import jpeg from 'jpeg-js';
import { testApp } from './helpers.js';
import { listen } from '../src/net/server.js';
import { CDKEY, LOGIN_KEY, lhDecrypt, lhEncrypt } from '../src/scoreboard/lhcrypt.js';
import { joinLines, splitLines } from '../src/scoreboard/scoreboard.js';
import { hasCrLf, makeLogo } from '../src/scoreboard/logo.js';

// Produced by Fable.exe's own routine (0x55E2C0) run under an emulator.
const GOLDEN: [string, string, string][] = [
  [LOGIN_KEY, '5349443a4f4b0d0a300d0a', '0b000000b114a9dc73911c2a5154f778b20b96a7'],
  [
    'abc',
    '4661626c65312e350d0a56657273696f6e20310d0a55504c4f41445f53434f5245530d0a6d6f620d0a51370d0a',
    '2d00000068349cf1e939db8440ea84f05db29fda5807280363a112f2ea95924d99c6faa34c7750c67566371f02c9f791fb8ee144',
  ],
  [
    CDKEY,
    Buffer.from(Array.from({ length: 199 }, (_, i) => i + 1)).toString('hex'),
    'c700000050935d6d0acaddd1d5eed245bd77be4ff16cf9f1d1932dbb90b9fc3ce07352debf016bccc7888fef5c4f8c3612eeabf6f41d5c0cc735d0f670704c2db00a3253da4e0830d2566d856c8974f4997179a0e8d638d73bd7d3276c66b70297c59e4512172ef148235ab452b3d25759bd53716d9bc0fb542cd44867d4480bbd7ac9251891fd278bd92281cd3365f83ce26faaa9c142da63ad04a148bf836c0f3c525d567a09f0f33f5cc6b3634c8212019976c6786327e2a7b944d98ce0ed7e23944fc2e61b11b0f5a237',
  ],
];

test('lhcrypt matches the game byte for byte and round-trips', () => {
  for (const [key, plain, cipher] of GOLDEN) {
    const p = Buffer.from(plain, 'hex');
    assert.equal(lhEncrypt(p, key).toString('hex'), cipher);
    assert.deepEqual(lhDecrypt(Buffer.from(cipher, 'hex'), key), p);
  }
});

function testJpeg(w: number, h: number): Buffer {
  const data = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      data[i] = (x * 7) & 255;
      data[i + 1] = (y * 5) & 255;
      data[i + 2] = ((x ^ y) * 3) & 255;
      data[i + 3] = 255;
    }
  return Buffer.from(jpeg.encode({ width: w, height: h, data }, 90).data);
}

test('logos come out 64x64 with no CR LF pair', () => {
  for (const [w, h] of [[300, 200], [64, 64], [17, 90]] as const) {
    const out = makeLogo(testJpeg(w, h));
    assert.equal(hasCrLf(out), false);
    const back = jpeg.decode(out);
    assert.equal(back.width, 64);
    assert.equal(back.height, 64);
  }
});

/** Send a request shaped exactly like Fable.exe's (see tools/re-notes/07-scoreboard-protocol.md). */
function fable(port: number, user: string, lines: (string | Buffer)[], login = false): Promise<{ status: number; lines: string[] }> {
  const key = login ? LOGIN_KEY : CDKEY;
  const body = lhEncrypt(joinLines(['Fable1.5', 'Version 1', ...lines]), key);
  return new Promise((resolve, reject) => {
    const req = http.request(
      {
        host: '127.0.0.1',
        port,
        method: 'POST',
        path: '/index.php',
        headers: {
          'User-Agent': 'Lionhead Studios HTTP Library Version 1.0',
          'X-LH-Username': user,
          ...(login ? { 'X-LH-Login': 'Login' } : {}),
          'Content-Length': body.length,
          Connection: 'close',
        },
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on('data', (c) => chunks.push(c));
        res.on('end', () => {
          const raw = Buffer.concat(chunks);
          resolve({ status: res.statusCode ?? 0, lines: res.statusCode === 200 ? splitLines(lhDecrypt(raw, key)) : [] });
        });
      },
    );
    req.on('error', reject);
    req.end(body);
  });
}

async function api(base: string, method: string, path: string, body?: unknown, token?: string) {
  const res = await fetch(base + path, {
    method,
    headers: { 'content-type': 'application/json', ...(token ? { authorization: `Bearer ${token}` } : {}) },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return { status: res.status, body: res.status === 204 ? null : ((await res.json()) as any) };
}

test('Fable scoreboard: login, clans, logos, members, score upload and download', async () => {
  const app = testApp();
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  try {
    const admin = await api(base, 'POST', '/launcher/v1/register', { username: 'gm_mob', password: 'password123' });
    app.db.run("UPDATE accounts SET role = 'admin' WHERE username = 'gm_mob'");
    const tok = admin.body.access_token as string;
    const p1 = await api(base, 'POST', '/launcher/v1/register', { username: 'Sparrow', password: 'password123' });
    await api(base, 'POST', '/launcher/v1/register', { username: 'Thorne', password: 'password123' });

    // The code is what the player types as the password in Fable's scoreboard login box.
    const code = await api(base, 'POST', '/launcher/v1/scoreboard-code', undefined, p1.body.access_token);
    assert.equal(code.status, 200);
    assert.match(code.body.code, /^[A-Z2-9]{10}$/);
    const md5 = (s: string) => crypto.createHash('md5').update(s).digest('hex');

    // Wrong code: status -3 (TEXT_GUI_SCOREBOARD_ERROR_LOGIN_FAILURE).
    const bad = await fable(port, 'Sparrow', ['LOG_ON_TO_REG_SERVER', 'Sparrow', md5('sparrowwrong'), Buffer.from([0x22, 0, 0, 0, 1, 2])], true);
    assert.equal(bad.lines[1], '-3');

    // The game lower-cases name + password before hashing.
    const ok = await fable(port, 'Sparrow', ['LOG_ON_TO_REG_SERVER', 'Sparrow', md5(('Sparrow' + code.body.code).toLowerCase()), Buffer.from([0x22, 0, 0, 0])], true);
    assert.equal(ok.status, 200);
    assert.equal(ok.lines[1], '0');
    const sid = lhDecrypt(Buffer.from(ok.lines[2]!, 'latin1'), CDKEY).toString('latin1');
    assert.match(sid, /^[0-9a-f]{24}\0$/);
    const session = sid.slice(0, -1);

    // A clan with a logo, Sparrow in it.
    const clan = await api(base, 'POST', '/admin/v1/clans', { name: "Maze's Own", description: 'Guild mages' }, tok);
    assert.equal(clan.status, 200);
    assert.equal((await api(base, 'POST', '/admin/v1/clans/members', { clan_id: clan.body.id, username: 'Sparrow' }, tok)).status, 204);
    const logo = await api(base, 'POST', '/admin/v1/clans/logo', { clan_id: clan.body.id, jpeg_base64: testJpeg(128, 128).toString('base64') }, tok);
    assert.equal(logo.body.version, 1);

    const list = await fable(port, 'Sparrow', ['DOWNLOAD_CLAN_LIST', 'Sparrow', session]);
    assert.deepEqual(list.lines, ['SID:OK', 'Chapters of the Lost', '1', 'Every hero in the realm', "Maze''s Own", String(clan.body.id), 'Guild mages']);

    // Logo: id, version, length, JPEG bytes. Not sent again once the game has that version.
    const logos = await fable(port, 'Sparrow', ['DOWNLOAD_CLAN_LOGOS', 'Sparrow', session, '0']);
    assert.equal(logos.lines[0], 'SID:OK');
    assert.equal(logos.lines[1], String(clan.body.id));
    assert.equal(logos.lines[2], '1');
    const bytes = Buffer.from(logos.lines[4]!, 'latin1');
    assert.equal(bytes.length, Number(logos.lines[3]));
    assert.equal(jpeg.decode(bytes).width, 64);
    const again = await fable(port, 'Sparrow', ['DOWNLOAD_CLAN_LOGOS', 'Sparrow', session, '2', '1', '0', String(clan.body.id), '1']);
    assert.deepEqual(again.lines, ['SID:OK']);

    // Upload: activity, LIB/HIB, score. Only improvements count.
    await fable(port, 'Sparrow', ['UPLOAD_SCORES', 'Sparrow', session, '3', 'HIB', '120.000000', '7', 'LIB', '42.500000']);
    await fable(port, 'Sparrow', ['UPLOAD_SCORES', 'Sparrow', session, '3', 'HIB', '90.000000', '7', 'LIB', '40.000000', '99', 'HIB', '5']);
    const board = await fable(port, 'Sparrow', ['DOWNLOAD_SCOREBOARD', 'Sparrow', session, "Maze's Own"]);
    assert.deepEqual(board.lines, ['SID:OK', '3', '120', 'Sparrow', "Maze''s Own", '7', '40', 'Sparrow', "Maze''s Own"]);

    const members = await fable(port, 'Sparrow', ['DOWNLOAD_MEMBER_LIST', 'Sparrow', session, 'Chapters of the Lost']);
    assert.deepEqual(members.lines, ['SID:OK', 'Sparrow', String(p1.body.account.id), 'NO_DESCRIPTION']);

    // A made-up or another player's session is refused.
    const forged = await fable(port, 'Thorne', ['DOWNLOAD_CLAN_LIST', 'Thorne', session]);
    assert.notEqual(forged.lines[0], 'SID:OK');
  } finally {
    server.close();
    app.close();
  }
});
