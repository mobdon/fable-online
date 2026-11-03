# Script interface

Fable's cutscene script runner (a long string-compare chain at about 0x00CBE000..0x00CD2000) turns text
commands such as GiveHero and GiveGold into calls on one object: `CGameScriptInterface`, at
`[0x0143E8F8]` (vtable 0x01260F0C, 918 slots). The mod calls the same slots.

Strings are `CCharString`: 4 bytes, built with `0x0099EBF0(this, const char*, int len = -1)` and freed
with `0x0099EAE0(this)`. All calls are `__thiscall`.

| Slot | Command | Signature |
| --- | --- | --- |
| +0x1E4 | GiveHero | `(const CCharString& def, int, bool)`, once per item |
| +0x1E8 | GiveHero, next step (default) | `(const CCharString& def)` |
| +0x1F8 | GiveGold | `(int amount)` |
| +0x23C | GiveHero, next step (other flag) | `(const CCharString& def)` |
| +0x2F4 | RemoveHeroClothes | `()` |
| +0x2F8 | HeroWear | `(const CCharString& def)` |
| +0x404 | hero health | `() -> float` |
| +0x408 | hero max health | `() -> float` |
| +0x41C | GiveHeroHealth | `(float delta, bool, bool)`; the script passes `(max - hp, true, false)` |

Other commands in the same runner (CreateNear, ObjectCreate, TeleportThing, TeleportToHSP, SetHeroWeapon)
use slots +0x118, +0x120, +0x800 and +0x20, which are the next to map for spawning a stand-in hero.

The dev kit (`client-mod/src/devkit.cpp`, on with `FABLE_ONLINE_DEV=1`) calls these from the game
window's message handler, which runs on the game's main thread.
