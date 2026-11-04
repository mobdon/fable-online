# Host a test server on your own PC

The Chapters of the Lost server runs on your PC for the first online tests. It needs no
copy of Fable running on the server side: it is a Node.js program that reads Fable's data
files once, at setup, from your own install. Later the same server moves to a Hetzner
machine unchanged (see the plan's hosting section).

You need Node.js 22 LTS (https://nodejs.org) and your Steam copy of Fable.

## One time

1. `1-setup-server.bat` installs the server and reads the regions, region exits, doors,
   monsters, shops and quests from your Fable folder. Give it the folder if Fable is not
   in the default Steam location: `1-setup-server.bat "D:\Games\Fable The Lost Chapters"`.
   Everything it writes stays on this PC and is never committed.

## Every test

1. `2-start-server.bat` and leave its window open. Ctrl+C stops it; heroes are saved.
2. `3-play.bat`, then in the launcher click Register (first time) or Log in, and Enter Albion.
   Your hero is named after your account (`mob` plays as `Mob`). Load the save from after
   the Guild: the server puts new heroes at the Guild too.
3. `4-start-walkers.bat Mob` (your hero's name) for three bot players who walk around you.
   They show up as stand-in heroes. `4-start-walkers.bat Mob 6` for six.

`admin.bat list` shows every hero and where the server has them.
`admin.bat place Mob <Region>` moves an offline hero to another region, if your save is
somewhere other than the Guild.

## With friends

Each friend needs their own Steam Fable and the same test build.

The easy, safe way is **Tailscale** (free, https://tailscale.com): you and your friends
install it and sign in, and everyone gets an address like `100.101.102.103` that only your
group can reach. No router settings, and your PC is not exposed to the internet.

1. On your PC: `2-start-server.bat 100.101.102.103` (your own Tailscale address).
2. Each friend: `3-play.bat 100.101.102.103`.

On the same home network, use your PC's LAN address (like `192.168.1.20`) instead.
`open-firewall.bat` (Run as administrator) lets Windows Firewall through on ports 7777 (game) and 7781 (Realm chat).

The launcher only talks plain http to this PC, a home network or a Tailscale address. A
public server will use https.

## What the server checks

Until Fable's controls are mapped, each player's mod reports where their Fable put the
hero, ten times a second while moving. The server only accepts moves the hero could have
made: running speed with about a second of slack, sensible climbs and falls, nothing while
dead. Anything else is refused, the player's Fable is put back where the server has the
hero, and the attempt is recorded. The first report after logging in may place the hero
anywhere in the region (your Fable loads your own save); reconnecting after a drop does not
get that, so dropping the connection is no way to teleport.

If your connection drops, the mod reconnects on its own for 5 minutes. After that, or after
a server restart, close Fable and start again from the launcher.
