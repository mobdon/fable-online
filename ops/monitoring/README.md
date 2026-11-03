# Monitoring: Prometheus + Grafana

Dashboards and alerts for the Fable Online server. The metrics come from
[`server/analytics`](../../server/analytics).

## Try it with fake data

```
cd server/analytics && npm install && npm run demo      # leave running
cd ops/monitoring && cp .env.example .env               # set a Grafana password
docker compose up -d
```

Open http://localhost:3000 and sign in. Dashboards are in the **Fable Online** folder:

| Dashboard | Shows |
| --- | --- |
| Overview | Heroes online per region, logins, session length, Guild graduations, Cullis jumps, chat |
| Economy | Watchdog anomalies, ledger vs DB gold supply, gold faucets and sinks per source, trade, items, Resurrection Phials |
| PvP & Arena balance | Arena win rate per school (Strength / Skill / Will), kill matrix, time to kill, damage share and counter rate per ability, queue times |
| Server health | Region Fable.exe up/down and restarts, tick time vs the 33 ms budget, latency, network, DB, CPU and memory |

Prometheus is on http://localhost:9090 (alerts under *Alerts*). Both ports are bound to
127.0.0.1 only.

## Changing things

- **Dashboards** are generated: edit `grafana/build-dashboards.mjs`, then run
  `node grafana/build-dashboards.mjs`. Grafana picks up the JSON within 30 s.
- **Alert rules** are in `prometheus/alerts.yml` (economy, PvP balance, health).
  Check with `docker run --rm --entrypoint promtool -v $PWD/prometheus:/p prom/prometheus:v3.5.0 check config /p/prometheus.yml`.
- **Scrape targets**: add each server process to `prometheus/prometheus.yml`.

Sending alerts to Discord or email needs Alertmanager; that is left for when the
server is deployed.
