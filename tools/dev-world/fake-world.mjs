#!/usr/bin/env node
// A stand-in region server for testing entity sync in the mod without accounts, tickets
// or a database: it accepts any hello and sends frames with a couple of entities moving.
//
//   node tools/dev-world/fake-world.mjs [--port 7777] [--creatures 3]
//
// Then launch the game with FABLE_ONLINE_HOST=127.0.0.1 and FABLE_ONLINE_PORT=7777.
// It speaks the same frames as the real server (shared/protocol/messages.proto), so the
// mod cannot tell the difference. It has no authority over anything: do not confuse it
// with server/, which is the real one.
import crypto from 'node:crypto';
import http from 'node:http';

const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
};
const port = Number(arg('port', 7777));
const creatures = Number(arg('creatures', 2));
const tickRate = Number(arg('tick-rate', 20));

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

/** Minimal server-side WebSocket: text frames, no extensions, no fragmentation. */
function accept(req, socket) {
  const key = req.headers['sec-websocket-key'];
  const hash = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${hash}\r\n\r\n`,
  );
  socket.setNoDelay(true);
}

function sendText(socket, text) {
  const payload = Buffer.from(text, 'utf8');
  const n = payload.length;
  let head;
  if (n < 126) head = Buffer.from([0x81, n]);
  else if (n <= 0xffff) {
    head = Buffer.alloc(4);
    head[0] = 0x81;
    head[1] = 126;
    head.writeUInt16BE(n, 2);
  } else {
    head = Buffer.alloc(10);
    head[0] = 0x81;
    head[1] = 127;
    head.writeBigUInt64BE(BigInt(n), 2);
  }
  socket.write(Buffer.concat([head, payload]));
}

/** Pulls whole frames out of a growing buffer; returns the text payloads. */
function readFrames(state) {
  const out = [];
  for (;;) {
    const b = state.buf;
    if (b.length < 2) return out;
    const opcode = b[0] & 0x0f;
    const masked = (b[1] & 0x80) !== 0;
    let len = b[1] & 0x7f;
    let at = 2;
    if (len === 126) {
      if (b.length < 4) return out;
      len = b.readUInt16BE(2);
      at = 4;
    } else if (len === 127) {
      if (b.length < 10) return out;
      len = Number(b.readBigUInt64BE(2));
      at = 10;
    }
    const maskAt = at;
    if (masked) at += 4;
    if (b.length < at + len) return out;
    let payload = b.subarray(at, at + len);
    if (masked) {
      const mask = b.subarray(maskAt, maskAt + 4);
      payload = Buffer.from(payload);
      for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
    }
    state.buf = b.subarray(at + len);
    if (opcode === 0x8) out.push({ close: true });
    else if (opcode === 0x1) out.push({ text: payload.toString('utf8') });
  }
}

const round = (n) => Math.round(n * 100) / 100;
let nextId = 2;
const clients = new Set();

const server = http.createServer((_req, res) => {
  res.writeHead(200, { 'content-type': 'text/plain' });
  res.end('fake world server: connect a WebSocket to /play\n');
});

server.on('upgrade', (req, socket) => {
  if (!req.headers['sec-websocket-key']) return socket.destroy();
  accept(req, socket);
  const client = { socket, id: nextId++, state: { buf: Buffer.alloc(0) }, origin: null, reported: 0 };
  clients.add(client);
  console.log(`client ${client.id} connected`);

  socket.on('data', (chunk) => {
    client.state.buf = Buffer.concat([client.state.buf, chunk]);
    for (const frame of readFrames(client.state)) {
      if (frame.close) return socket.end();
      let msg;
      try {
        msg = JSON.parse(frame.text);
      } catch {
        continue;
      }
      if (msg.t === 'hello') {
        sendText(socket, JSON.stringify({ t: 'welcome', player_id: client.id, name: `Tester${client.id}`, phase: 'world', tick_rate: tickRate }));
        sendText(socket, JSON.stringify({ t: 'enter_region', region: 'GuildWoods', x: 0, y: 0, z: 0 }));
      } else if (msg.t === 'hero_pos') {
        // The entities are placed around wherever the player's own hero is, so they
        // appear next to them whatever region the test save is in.
        client.origin = { x: msg.x, y: msg.y, z: msg.z };
        if (client.reported++ % 100 === 0) console.log(`client ${client.id} at ${round(msg.x)}, ${round(msg.y)}, ${round(msg.z)}`);
      } else if (msg.t === 'ping') {
        sendText(socket, JSON.stringify({ t: 'pong', n: msg.n ?? 0 }));
      }
    }
  });

  const bye = () => {
    if (clients.delete(client)) console.log(`client ${client.id} gone`);
  };
  socket.on('close', bye);
  socket.on('error', bye);
});

let tick = 0;
const started = Date.now();
setInterval(() => {
  tick++;
  const t = (Date.now() - started) / 1000;
  for (const client of clients) {
    const o = client.origin ?? { x: 0, y: 0, z: 0 };
    const entities = [
      {
        id: 101,
        kind: 'hero',
        name: 'Walker',
        renown: 1200,
        x: round(o.x + 4 * Math.cos(t * 0.6)),
        y: round(o.y + 4 * Math.sin(t * 0.6)),
        z: round(o.z),
        yaw: round(t * 0.6 + Math.PI / 2),
        anim: 2,
        health: 100,
      },
    ];
    for (let i = 0; i < creatures; i++) {
      entities.push({
        id: 1_000_001 + i,
        kind: 'creature',
        def: 'CREATURE_BANDIT_GRUNT_LEVEL1',
        x: round(o.x - 6 - i * 2),
        y: round(o.y + 4 * Math.sin(t * 0.4 + i)),
        z: round(o.z),
        yaw: round(Math.sin(t * 0.4 + i) > 0 ? 0 : Math.PI),
        anim: 1,
        health: 100,
      });
    }
    sendText(client.socket, JSON.stringify({ t: 'tick', tick, last_input_seq: 0, entities }));
  }
}, 1000 / tickRate);

server.listen(port, () => console.log(`fake world server on ws://127.0.0.1:${port}/play (${creatures} creatures)`));
