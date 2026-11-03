# World time (day and night)

Checked 2026-11-03 against the Steam Fable.exe (no ASLR, so addresses are fixed).

## CGameTimeManager

`CWorld` (vtable `0x012390DC`) creates it in the method at `0x004A6E30` (CWorld vtable slot 9):

```
0x004A6F27  push 0x3C ; call operator new
0x004A6F35  mov ecx, [esi+8] ; push esi ; push ecx ; call 0x006BBC30   ; CGameTimeManager(arg1 = [world+8], arg2 = world)
0x004A6F48  mov [esi+0x1C], eax                                     ; world+0x1C = the time manager
```

The constructor (`0x006BBC30`, 0x3C bytes) registers two console commands with the console at
`[0x013CAA40]`: `SetTimeOfDay` (handler `0x006BB860`) and `SetDaySpeed` (handler `0x006BB940`).

| Offset | What | How we know |
| --- | --- | --- |
| +0x00 | owning `CWorld*` | ctor stores its second argument, the world, here; used to check a pointer |
| +0x04 | `[world+8]` | ctor's first argument |
| +0x08 | time of day, float, fraction of a day (0 = midnight, 0.5 = noon) | `SetTimeOfDay h` stores `h * (1/24)` here (constant at `0x0122DC88` = 0.041667), then clamps to 0..1 |
| +0x0C | same value | `SetTimeOfDay` writes it too |
| +0x10 | day speed, `1 / day length` | `SetDaySpeed x` stores `1 / x` (0 when x <= the limit at `0x0129BA3C`) |
| +0x18, +0x20 | flags | when both are set, `SetTimeOfDay` clamps to 09:00..17:00 (`0x3EC00000` = 0.375, `0x3F355555` = 0.7083): a day-only mode some scripts use |

Strings next to the command names (`DayCount`, `TimeStep`, `Time`) are its saved fields.

## How the mod uses it

`client-mod/src/worldclock.cpp` (added with the entity sync work) finds the world by its vtable (the same heap scan as the probe),
checks `[[world+0x1C]+0] == world`, and writes the server's time into +0x08 and +0x0C four times
a second. The server's `sky` message carries `day_fraction` and `day_ms`, so the mod advances
the time on its own between messages.

Test without a server: `set FABLE_ONLINE_TIME=22` before starting the game puts it at 22:00 (the
day keeps turning at the 48-minute default). `fable_online.log` shows the time it found and the
time it writes.

## Still open

- Whether +0x10 drives the game's own clock per frame or per second. The mod doesn't touch it;
  it overwrites the time instead. If the sun stutters between writes, set +0x10 to 0 as well.
- Weather. Fable has no realm-wide weather state: rain, snow and fog come from each region's
  environment defs (`ENVIRONMENT_THEME_*`) and the weather renderer (`CEngineWeatherRenderer`,
  `CEngineWeatherSettings`, rain and snow shaders `PSHADER_WEATHER_RAIN` / `_SNOW`). The server
  already sends a weather state per region; mapping it onto those settings is the next step.
