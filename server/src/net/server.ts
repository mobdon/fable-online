import http from 'node:http';
import { WebSocketServer, type WebSocket } from 'ws';
import type { App } from '../app.js';
import { createHttpHandler } from './http.js';
import { GameSocket } from './game-socket.js';

/**
 * Every this often each game socket gets a WebSocket ping. One that has not answered the
 * previous ping is dead (a crashed game, a pulled cable, a laptop lid) and is dropped, so
 * its hero leaves the world and its resume window starts, instead of standing there for
 * the minutes TCP can take to notice.
 */
export const HEARTBEAT_MS = 10_000;

/** HTTP API and the game WebSocket (/play) on one port. */
export function listen(app: App, port = app.cfg.port, heartbeatMs = HEARTBEAT_MS): Promise<http.Server> {
  const server = http.createServer(createHttpHandler(app));
  const wss = new WebSocketServer({ noServer: true, maxPayload: 4096, perMessageDeflate: false });
  const alive = new WeakMap<WebSocket, boolean>();
  server.on('upgrade', (req, socket, head) => {
    if (new URL(req.url ?? '/', 'http://x').pathname !== '/play') return socket.destroy();
    wss.handleUpgrade(req, socket, head, (ws) => {
      alive.set(ws, true);
      ws.on('pong', () => alive.set(ws, true));
      // Any frame from the client counts as a sign of life too.
      ws.on('message', () => alive.set(ws, true));
      new GameSocket(app, ws);
    });
  });
  const beat = setInterval(() => {
    for (const ws of wss.clients) {
      if (!alive.get(ws)) {
        ws.terminate();
        continue;
      }
      alive.set(ws, false);
      ws.ping();
    }
    app.resume.sweep();
  }, heartbeatMs);
  beat.unref();
  server.on('close', () => {
    clearInterval(beat);
    wss.close();
  });
  return new Promise((resolve) => server.listen(port, () => resolve(server)));
}
