# World time (day and night)

Checked 2026-11-03 against the Steam Fable.exe (no ASLR, so addresses are fixed).

## Finding the world and its clock

`CWorld`'s primary vtable is `0x012390F0`; `0x012390DC` (listed in rtti.json as CIDrawWorld's
table) sits at world+4. The constructor `0x004A67D0` writes both. There is no need to scan the
heap: the game object at `[0x013B8A1C]` holds the world at +0x24 (getter `0x00686A70`, also
`0x004C7AA0` and `0x0073A680`: `mov eax,[0x13B8A1C] ; mov eax,[eax+0x24] ; ret`). The scripted
time change at `0x0057C81B` uses exactly this path:

```
0x0057C81B  call 0x00686A70          ; world
0x0057C820  mov ebx, [eax+0x1C]      ; time manager
0x0057C83A  call 0x006BB810          ; SetTime(0.41667)  (10:00)
```

So the clock is `[[[0x013B8A1C]+0x24]+0x1C]`, checked with `[clock+0] == world` and
`[world] == 0x012390F0`.

## CGameTimeManager

The world creates it in `0x004A6E30` (CWorld vtable `0x012390F0` slot 9):

```
0x004A6F27  push 0x3C ; call operator new
0x004A6F35  mov ecx, [esi+8] ; push esi ; push ecx ; call 0x006BBC30   ; CGameTimeManager(arg1 = [world+8], arg2 = world)
0x004A6F48  mov [esi+0x1C], eax                                     ; world+0x1C = the time manager
```

The constructor (`0x006BBC30`, 0x3C bytes) reads the starting hour and the day length from a
def (`[[[world+0x10]+0xD8]+0xB4]` hours, `+0xB8` day length in seconds) and registers two
console commands with the console at `[0x013CAA40]`: `SetTimeOfDay` (handler `0x006BB860`) and
`SetDaySpeed` (handler `0x006BB940`).

| Offset | What | How we know |
| --- | --- | --- |
| +0x00 | owning `CWorld*` | ctor stores its second argument, the world, here |
| +0x04 | `[world+8]` | ctor's first argument |
| +0x08 | time of day, float, fraction of a day (0 = midnight, 0.5 = noon) | `SetTimeOfDay h` stores `h * (1/24)` (`0x0122DC88`), clamps to 0..1 |
| +0x0C | last update's time | update `0x006BB990` copies +0x08 here first |
| +0x10 | day speed, fraction of a day per game second (`1 / day length`) | ctor: `1.0 / [def+0xB8]`; update adds `dt * [+0x10]` |
| +0x14 | day count | update increments it when the time wraps past 1.0 |
| +0x18 | byte: day-only mode | update then only advances inside 09:00..17:00 or 05:30..19:30 (+0x20 picks which) |
| +0x1C | float, time copy | follows +0x08 outside day-only mode |
| +0x21 | byte: easing to a target | set by `0x006BB720(target, seconds)`; target +0x24, seconds left +0x28 |
| +0x30 | non-zero: clock stopped | update skips the advance (scripts and cutscenes) |

Per-frame update: `CWorld` update calls `0x006BB990(dt)` at `0x004A5DE4` with
`dt = 1.0 / [0x01375550]` (the world update rate), then passes the new time to the environment
(`[world+0x18]`, `0x006B3FF0`), which blends the sky and lighting from it.

Other setters: `0x006BB810` SetTime(f) (clamped 0..1), `0x006BB7D0` adds to the speed.

## How the mod uses it

`client-mod/src/game/world_time.cpp` reads the clock through the path above (Read/WriteProcessMemory,
so a stale pointer fails safely). `client-mod/src/sync.cpp` keeps it on the server's `sky`
frames (`client-mod/src/world/sky.cpp`) four times a second:

- a gap over 0.02 of a day (about 29 game minutes: a join, a load, a staff time change) is
  snapped by writing +0x08, +0x0C and +0x1C;
- a smaller gap is closed by setting the speed at +0x10 a little faster or slower for about
  8 seconds, so the light never jumps;
- while +0x21 or +0x30 is set (Fable's own scripts and cutscenes) the mod waits, then catches up.

Offline, the speed goes back to Fable's own. `FABLE_ONLINE_SYNC_CLOCK=0` turns it off.

## Weather (rain and snow)

Fable has no realm-wide weather state and no console command for it. Each area's engine theme
(`ENGINE_THEME` / `ENVIRONMENT_THEME_DAY` defs in game.bin) carries its own rain, snow and mist
amounts, which `CEnvironment` (vtable `0x0125D53C`, world+0x18, per frame `0x006B3FF0`) blends.
The renderer has an override, which is what the mod uses.

`CEngineWeatherRenderer` (vtable `0x012A2434`, 0x12FC bytes, ctor `0x00B52250`) lives at
`[0x01436E44]` (set at `0x00B2697D` when the engine builds its components).

| Offset | What |
| --- | --- |
| +0x5C8 | float, rain being drawn now (0..1) |
| +0x5CC | float, snow being drawn now |
| +0x5D0 | byte, rain particles running |
| +0x12E8 | float, rain override amount |
| +0x12EC | byte, rain override on |
| +0x12F0 | float, snow override amount |
| +0x12F4 | byte, snow override on |
| +0x12F8 | float, most it draws (1.0) |

Setters: `0x00B50EE0` rain, `0x00B50F40` snow (thiscall, one float: negative turns the override
off, otherwise capped at +0x12F8). They are `CEngine` vtable `0x012A0F3C` slots 80 and 81
(wrappers `0x00B24010` / `0x00B24020` that load `[0x01436E44]`), which is how the game's own
scripts would reach them.

The renderer's update (slot 7, `0x00B52B30`) reads the theme's amounts from the current theme
(`+0x16C` rain, `+0x168` snow, `+0x170` a third amount, probably mist, copied to +0x188 with a
flag at +0x5D2). With an override on, it uses the override instead and moves the drawn amount
toward it by 0.01 a frame (`0x00B52870`), so changes fade. `0x00B51EA0` starts or stops the
particles (amount * 128 particles; below 1/128 is off). The override is reset when an area loads
(`0x00B527DF`), so the mod sets it again after loads.

`client-mod/src/game/weather.cpp` writes the override fields directly (after checking the
vtable). Rain, storm, snow and blizzard from the server's `sky` frame are drawn; clear, cloudy,
fog and interiors give the area its own weather back. `FABLE_ONLINE_SYNC_WEATHER=0` turns it off.

### Retail bug: snow follows rain

The snow fade `0x00B52900` starts from `[ecx+0x5C8]` (the rain amount) where the rain fade
`0x00B52870` reads its own. So whenever rain is drawn, snow is set to nearly the same amount
every frame, whatever the snow override says (retail areas never have both, so it never
showed). With rain overridden to 0.8 the game drew heavy snow and rain together. The mod
rewrites the displacement byte at `0x00B52903` (`C8` to `CC`) at start-up
(`game::weather::fix_snow_fade`), after checking the six bytes `8B 81 C8 05 00 00`.

## Still open

- Fog and cloud: no override found for the third theme amount (+0x170) yet.
- Whether rain is masked indoors when an interior is part of an outdoor region (the server
  sends `indoor` only for regions whose name says so).
- None of this has run in game yet.
