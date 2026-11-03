// Starts the chat service. Settings come from the environment:
//   CHAT_PORT=7781  CHAT_HOST=0.0.0.0
//   CHAT_TOKEN_SECRET=...   set in production (shared with the gateway); unset = dev mode, log in by name
//   CHAT_TRUST_CLIENT_POS=1 dev only: name tags follow positions the mod reports
//   CHAT_BANNED_WORDS=a,b,c CHAT_MOTD="..."  CHAT_MODERATION_FILE=moderation.json
//   CHAT_TICK_MS=200        how often name tags are sent
import { ChatHub } from "./hub.ts";
import { listen } from "./server.ts";

const env = process.env;
const secret = env.CHAT_TOKEN_SECRET || null;
const hub = new ChatHub(
  {
    tokenSecret: secret,
    trustClientPositions: env.CHAT_TRUST_CLIENT_POS ? env.CHAT_TRUST_CLIENT_POS === "1" : !secret,
    bannedWords: (env.CHAT_BANNED_WORDS ?? "").split(",").map((w) => w.trim()).filter(Boolean),
    motd: env.CHAT_MOTD || undefined,
    moderationFile: env.CHAT_MODERATION_FILE ?? "moderation.json",
  },
  (event, data) => console.log(JSON.stringify({ ts: new Date().toISOString(), event, ...data })),
);
const port = Number(env.CHAT_PORT ?? 7781);
const server = await listen(hub, port, env.CHAT_HOST ?? "0.0.0.0");
const tick = setInterval(() => hub.tick(), Number(env.CHAT_TICK_MS ?? 200));
console.log(`chat listening on ${port} (${secret ? "token login" : "DEV MODE: login by name, no password"})`);

const stop = () => { clearInterval(tick); server.close(); process.exit(0); };
process.on("SIGINT", stop);
process.on("SIGTERM", stop);
