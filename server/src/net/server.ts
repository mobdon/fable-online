import http from 'node:http';
import { WebSocketServer } from 'ws';
import type { App } from '../app.js';
import { createHttpHandler } from './http.js';
import { GameSocket } from './game-socket.js';

/** HTTP API and the game WebSocket (/play) on one port. */
export function listen(app: App, port = app.cfg.port): Promise<http.Server> {
  const server = http.createServer(createHttpHandler(app));
  const wss = new WebSocketServer({ noServer: true, maxPayload: 4096, perMessageDeflate: false });
  server.on('upgrade', (req, socket, head) => {
    if (new URL(req.url ?? '/', 'http://x').pathname !== '/play') return socket.destroy();
    wss.handleUpgrade(req, socket, head, (ws) => new GameSocket(app, ws));
  });
  server.on('close', () => wss.close());
  return new Promise((resolve) => server.listen(port, () => resolve(server)));
}
