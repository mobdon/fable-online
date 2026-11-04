import { createApp } from './app.js';
import { listen } from './net/server.js';
import { startMetricsServer } from './analytics/http.js';

const app = createApp();
const audit = app.economy.audit();
if (!audit.ok) {
  console.error('Economy audit FAILED at boot, refusing to start:\n  ' + audit.problems.join('\n  '));
  process.exit(1);
}
app.director.start();
const tradeSweep = setInterval(() => app.trades.sweep(), 30_000);
const server = await listen(app);
// Chat and metrics are side services: a busy port is reported, and the world still starts.
const side = async <T>(what: string, start: () => Promise<T>): Promise<T | null> => {
  try {
    return await start();
  } catch (e) {
    console.error(`${what} could not start: ${(e as Error).message}`);
    return null;
  }
};
const chatServer = app.chat ? await side(`Realm chat on :${app.cfg.chatPort}`, () => app.chat!.listen()) : null;
const metricsServer = app.cfg.metricsPort
  ? await side(`Metrics on :${app.cfg.metricsPort}`, () =>
      startMetricsServer(app.analytics.metrics.registry, { port: app.cfg.metricsPort, host: app.cfg.metricsHost }),
    )
  : null;
console.log(
  `Fable Online server "${app.cfg.realmId}" on :${app.cfg.port}  ` +
    `(${app.regions.size} regions, tick ${app.cfg.tickRate} Hz, ` +
    `${app.cfg.maxChannels} channels x ${app.cfg.channelHardCap} = ${app.director.capacity} player slots)`,
);
console.log(
  `Realm chat ${chatServer ? `on :${app.cfg.chatPort} (token login from /join)` : 'off'}; ` +
    `metrics ${metricsServer ? `on http://${app.cfg.metricsHost}:${app.cfg.metricsPort}/metrics` : 'off'}`,
);

let stopping = false;
const shutdown = () => {
  if (stopping) return;
  stopping = true;
  console.log('Shutting down: saving every hero...');
  clearInterval(tradeSweep);
  server.close();
  metricsServer?.close();
  app.close();
  process.exit(0);
};
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
