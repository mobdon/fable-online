# The connection: turning stand-ins into players

`docs/entity-sync.md` covers how the server's frames become stand-ins in each player's game.
This is the other direction, and the link itself: how a player's own hero gets into the
server, and how the connection survives real networks.

```
Fable (hero) ──read_pos/read_yaw──▶ PositionReporter ──hero_pos──▶ Channel.reportPosition
     ▲                                                                 │ MoveGuard
     └──────────────write_pos◀──────── pos_correct ◀──── refused ──────┘
```

## Position reports

Fable's own controls are not mapped yet, so the server cannot move heroes from stick input
alone. Instead the mod reports where its Fable put the hero (`hero_pos`: x, y, z, facing).
`server/src/world/movement.ts` decides whether to believe it (`FO_CLIENT_MOVE`):

| Mode | What happens | Use |
| --- | --- | --- |
| `checked` (default) | A report is accepted only if the hero could have got there from where the **server** has them. | Every server |
| `trusted` | Every report is accepted (`FO_TRUST_CLIENT_POS=1` still means this). | Single-machine debugging |
| `off` | Reports are ignored; heroes move only from `input`. | Once input is mapped |

The checks: distance is paid from a budget that refills at 9 m/s and holds 1.2 seconds of
it (a roll, a slope or a late packet passes; a speed hack or teleport does not), a climb of
at most 2.5 m plus 4 m/s, a fall of at most 30 m/s, nothing while dead or while the next
region loads, and coordinates inside the world. A refused report changes nothing on the
server. The client gets `pos_correct` with the server's position (at most twice a second),
the mod writes it into its own hero, and the next report starts from there. Refusals are
recorded (`move_rejected`, one event per run).

**Anchors.** The client's Fable loads its own save, which is not where the server last had
the hero, and the mod cannot make Fable load a given spot yet. So the first report after a
join may be anywhere in the region (`FO_JOIN_ANCHOR`, on by default; recorded as
`move_anchor`). A hero placed at the origin (new, or moved by the law) always gets one. A
region arrival allows 40 m between the server's entrance and Fable's. A resumed connection
gets none.

**Areas Fable loads by itself.** Walking out of a town, into a house or through a Cullis
Gate makes the client's Fable load another area, and the hero's coordinates jump to that
area's. Each report carries the area hash (`area`). When it changes, the guard lets the next
report through from anywhere, once (`area_change`), but no more than once every 3 seconds: a
real load takes longer than that. A `pos_correct` carries the area it was meant for, and the
mod skips one that arrives after its Fable has already moved on. Until the server follows
those exits itself, the hero stays in the same channel. Heroes in different areas of one
channel don't see each other's stand-ins.

**Walk and run.** A hero moved by reports has no stick input, so the channel sets the
stand-in's animation from the reported speed (walk from 0.4 m/s, run from 4 m/s). The mod
sends reports while the hero moves (at most ten a second) and one a second while it stands
still, so silence means idle.

## Staying connected

- **Heartbeats.** The server pings every game socket every 10 seconds and drops one that has
  not answered the last ping, so a crashed or frozen game leaves the world in under 20
  seconds instead of standing there until TCP gives up. The mod pings every 2 seconds and
  logs the round trip (`sync:` status lines, F7).
- **Resume.** Join tickets are one-time. Every `welcome` carries a `resume` token, good once,
  for 5 minutes after that connection ends (`FO_RESUME_MS`), and cancelled by the next
  welcome. The mod reconnects with it on its own. Tokens live in memory: a server restart
  sends everyone back to the launcher.
- **Giving up.** Close code 4000 (ticket or token refused) or 4003 (the hero logged in
  elsewhere) stops the mod reconnecting, so two copies never fight over one hero.

## Test realm switches

| Variable | Default | Test host |
| --- | --- | --- |
| `FO_NEW_HERO_PHASE` | `childhood` | `world`: new heroes start at the Guild, in the shared world |
| `FO_AUTO_HERO` | off | On: the first join of an account with no hero makes one named after the account |
| `FO_PUBLIC_HOST` | `127.0.0.1` | The address friends reach the server on (Tailscale or LAN) |

With no `manifest-live.json` in `server/content`, the server answers the launcher with an
empty manifest (nothing to patch), so a home server works with whatever build each player
has. The launcher accepts plain http only for loopback, home networks (10/8, 172.16/12,
192.168/16) and Tailscale (100.64/10).

## Tools

- `ops/local-host/` runs all of this on a Windows PC: setup, server, launcher, walkers.
- `npm run walkers -- --near <Hero>`: bots that log in like players and walk around a hero.
- `npm run admin -- list | graduate <Hero> | place <Hero> <Region> [x y z]`.
- `npm run link-test -- <link_test.exe> [wine]`: the mod's own connection code
  (`client-mod/tests/link_test.cpp`, built with `-DFO_BUILD_TESTS=ON`) against a real server:
  join, walk, a refused jump corrected, a dropped connection resumed, ping.

## Not done yet

- The server cannot tell which region the client's Fable has loaded (the mod only has a hash
  of the area anchor). An area change is let through and tracked by hash, but the server's
  region (and so its creatures, exits and the region chat channel) stays the one it had.
  Mapping hashes to region names is what moves the hero to the right channel.
- Moves are checked against speed, not walls: the navmesh (`FO_NAV_DIR`) is the next check.
