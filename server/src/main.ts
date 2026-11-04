import { createApp } from './app.js';
import { listen } from './net/server.js';

const app = createApp();
const audit = app.economy.audit();
if (!audit.ok) {
  console.error('Economy audit FAILED at boot, refusing to start:\n  ' + audit.problems.join('\n  '));
  process.exit(1);
}
app.director.start();
const tradeSweep = setInterval(() => app.trades.sweep(), 30_000);
const partySweep = setInterval(() => app.parties.sweep(), 5_000);
const server = await listen(app);
console.log(
  `Fable Online server "${app.cfg.realmId}" on :${app.cfg.port}  ` +
    `(${app.regions.size} regions, tick ${app.cfg.tickRate} Hz, ` +
    `${app.cfg.maxChannels} channels x ${app.cfg.channelHardCap} = ${app.director.capacity} player slots)`,
);

let stopping = false;
const shutdown = () => {
  if (stopping) return;
  stopping = true;
  console.log('Shutting down: saving every hero...');
  clearInterval(tradeSweep);
  clearInterval(partySweep);
  server.close();
  app.close();
  process.exit(0);
};
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
