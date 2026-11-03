# Server (phase 4+)

Empty on purpose until phase 2 tells us what the game exposes. Plan: TypeScript on Node 22,
`gateway/` (accounts, version check), `region/` (starts one server-mode Fable.exe per region
channel and relays its state), `persistence/` (Postgres). See the plan doc.

## chat/ (built)

Chat channels, whispers, moderation, flood control and name tags. Runs on its own so chat
survives region server restarts. See `chat/` and `../shared/protocol/chat.md`.
Hooks for later phases: `ChatHub.updateHero()` (region server: positions, renown, PvP flag),
`ChatHub.setParty()` (party system), the `EventSink` passed to `ChatHub` (analytics),
and `auth.ts` (gateway login tokens).
