// Tiny HTTP server for Prometheus to scrape. Bind it to a private interface:
// /metrics must never be reachable from the internet.

import { createServer, type Server } from "node:http";
import type { Registry } from "prom-client";

export function startMetricsServer(registry: Registry, opts: { port?: number; host?: string } = {}): Promise<Server> {
  const server = createServer(async (req, res) => {
    if (req.url === "/metrics") {
      try {
        const body = await registry.metrics();
        res.writeHead(200, { "content-type": registry.contentType });
        res.end(body);
      } catch (err) {
        res.writeHead(500).end(String(err));
      }
    } else if (req.url === "/healthz") {
      res.writeHead(200, { "content-type": "text/plain" }).end("ok");
    } else {
      res.writeHead(404).end();
    }
  });
  return new Promise((resolve) => server.listen(opts.port ?? 9464, opts.host ?? "127.0.0.1", () => resolve(server)));
}
