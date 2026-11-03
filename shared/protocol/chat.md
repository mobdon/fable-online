# Chat and name tag protocol (version 1)

The chat service (`server/chat/`) and the mod (`client-mod/src/net/`, `client-mod/src/ui/`)
talk over TCP, one JSON object per line (UTF-8, `\n` at the end). Every object has a `t` (type).
Default port **7781**. TLS will sit in front of it (gateway or stunnel) before public servers.

This is separate from the game protocol in `messages.proto` on purpose: chat is low-rate,
text-only, and has to keep working while a region server restarts.

## Login

| Mode | How | When |
| --- | --- | --- |
| Dev | `{"t":"hello","v":1,"name":"Mob"}` | `CHAT_TOKEN_SECRET` is not set on the server |
| Production | `{"t":"hello","v":1,"token":"<gateway token>"}` | the gateway signs a token at login (HMAC-SHA256, see `server/chat/src/auth.ts`); the launcher passes it to the mod as `FABLE_ONLINE_TOKEN` |

Reply: `{"t":"welcome","v":1,"id":7,"name":"Mob","renown":0,"motd":"...","channels":["say",...]}`,
or an `err` and the connection closes (`auth_failed`, `bad_name`, `name_taken`, `forbidden` (banned), `version`).

Hero names: 3 to 16 characters, letters, digits, spaces, `'` and `-`, starting with a letter. Unique, ignoring case.

## Client to server

| `t` | Fields | Notes |
| --- | --- | --- |
| `say` | `ch`, `text` | `ch` is one of the channels below. `text` up to 200 characters |
| `whisper` | `to`, `text` | to a hero name, any region |
| `pos` | `region`, `x`, `y`, `z` | **dev only**. Ignored in production, where region servers own positions |
| `ignore` | `name`, `on` | per session for now; saved with the account once persistence exists |
| `report` | `name`, `reason` | goes to the analytics event log for moderators |
| `mod` | `action` (`mute`, `unmute`, `kick`, `ban`), `name`, `minutes?`, `reason?` | moderators and admins only |
| `ping` | `n` | server answers `pong` with the same `n`. The mod pings every 30 s; 90 s of silence drops the connection |

## Channels

| Channel | Who hears it | Cooldown |
| --- | --- | --- |
| `say` | heroes within 30 units, same region | none |
| `shout` | everyone in the region | 3 s |
| `world` | everyone online | 10 s |
| `trade` | everyone online | 30 s |
| `party` | your party (party system: later) | none |
| `guild` | your guild (from the login token) | none |
| `whisper` | one hero | none |

Flood control on top of that: a burst of 5 messages, refilling one every 2 s; the same text
twice in 30 s is refused; 5 refusals in a minute is an automatic 5 minute mute.
Text is cleaned (control characters, bidi overrides and zero-width characters removed) and
run through the word filter (`CHAT_BANNED_WORDS`). Ignored players' messages are not
delivered, and the sender isn't told, so an ignore can't be detected.

## Server to client

| `t` | Fields |
| --- | --- |
| `msg` | `ch`, `from`, `fromId`, `text`, `ts` (ms), `to` (whispers only; your own whispers come back with `from` = you) |
| `sys` | `text`, `level` (`info` or `warn`) |
| `err` | `code`, `text` (`rate_limited`, `muted`, `duplicate`, `too_long`, `no_such_player`, `not_in_party`, `not_in_guild`, `forbidden`, `bad_message`, ...) |
| `tags` | name tags, below |
| `pong` | `n` |

## Name tags

Fable has no levels, so a tag shows **name, guild, title and renown**. The server sends each
client the heroes within 60 units in its region, 5 times a second, as changes only:

```json
{"t":"tags","region":12,
 "add":[{"id":4,"name":"Maze","renown":12500,"title":"Arrow Master","guild":"OAK","pvp":false,"x":10.5,"y":3,"z":0}],
 "upd":[],  "move":[{"id":9,"x":1,"y":2,"z":0}],  "del":[3]}
```

- `add`: a hero came into range (full tag). `upd`: renown, title, guild or PvP flag changed (full tag).
  `move`: position only. `del`: out of range or logged off.
- A message with a different `region` than the last one means "you changed region": drop all tags.
- `pvp: true` (flagged for open-world PvP after the Oakvale Guild mission) draws the name in red.
- Renown colours (client side, `client-mod/src/ui/nametags.cpp`): under 1,000 white, 1,000+ green,
  5,000+ blue, 20,000+ purple, 50,000+ gold. First pass; tune once real renown numbers are in.

In production the region server is the only source of positions, renown, title and PvP
flag: it calls `ChatHub.updateHero()` (phase 5). Until then dev servers trust the mod's `pos`.
