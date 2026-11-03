# Online scoreboard and clan protocol (Fable.exe)

Decoded from the Steam Fable.exe by disassembly. The request builders and the cipher were run under an emulator (Unicorn) to capture their exact output, and the server's replies were fed back through the exe's own reply parsers (login, clan list and scoreboard all return success).

## Transport

- `POST <host>:<port>/index.php`, HTTP/1.1, `Connection: close`, `User-Agent: Lionhead Studios HTTP Library Version 1.0`.
- Host: CCharString at `[0x013B8AF0]`, set at `0x00562F8B` (`push 0x0124DB14` = "fableclangw.lionhead.com"). Path at `[0x013B8AF4]` = "/index.php".
- Port: profile `+0x120` (`ScoreboardServerPort`), read at `0x0055F830`. An optional proxy uses profile `+0x118` (IP) and `+0x11C` (port).
- Headers: `X-LH-Username: <profile +0x110>`. The login request also sends `X-LH-Login: Login`.
- The reply must be HTTP 200 with a body (the poll at `0x0055FB80` checks for 200).
- Send function: `0x0055F710(body vector, key CCharString*, isLogin)`.

## Cipher (`0x0055E2C0`, encrypt `0x0055E040`, decrypt `0x0055E180`)

XTEA, 32 cycles, 64-bit blocks, little endian, plus:

- Key schedule from a text key of length n: `buf[i] = ((i+1) * key[n-i]) & 0xFF` for i = 0..n-1 (`key[n]` is the NUL, so `buf[0] = 0`). Then `k = [sx(buf[3]), sx(buf[2]), sx(buf[1]), sx(buf[0])]`, sign-extended.
- After each full block: `k0 += sx(buf[p]); k1 -= sx(buf[p+1]); k2 += sx(buf[p+2]); k3 -= sx(buf[p+3])`, with p starting at 4 and wrapping to 0 at n.
- Output: `u32 plaintext length`, then the blocks. A trailing partial block is zero padded.
- Keys: login request and reply use `4AF92F27-1892-42f1-AA23-AF405325D8E6` (`0x0124DCC0`, global `[0x013B8AF8]`). Every later request and reply uses the retail "CD key" `THIS-ISMY-VALI-DCDK-EYOH-YESI-TISM` (`0x0124C58C`).
- Implementation: `server/src/scoreboard/lhcrypt.ts`, with test vectors produced by the exe.

## Messages

Every plaintext request is CR LF separated:

```
Fable1.5
Version 1
<ACTION>
<username>
<md5 hex | session id>
<params...>
```

Replies are CR LF separated too and are split before parsing, so no line may contain CR LF (this includes binary lines). Every reply except the login reply must start with `SID:OK`, or the game shows TEXT_GUI_SCOREBOARD_ERROR_GENERAL_NETWORK. Text lines go through `0x0055ECB0`, which turns `''` into `'`.

| Action | Builder | Params | Reply after `SID:OK` |
| --- | --- | --- | --- |
| `LOG_ON_TO_REG_SERVER` | 0x00560D00 | md5 = MD5(lowercase(username + password)), then a binary line holding the CD key encrypted | No SID line. The lines are: anything, status, session. Status `0` means OK. `-3`, `-4` or `-5` shows LOGIN_FAILURE, and other values show GENERAL_NETWORK. The session is the session string encrypted with the CD key, ending in its own NUL, and is stored at scoreboard `+0x34` |
| `DOWNLOAD_CLAN_LIST` | 0x005615C0 | none | Per clan: name, id, description (`NO_DESCRIPTION` = none) |
| `DOWNLOAD_CLAN_LOGOS` | 0x00561980 | count, then per clan: id and the logo version on disk (`0x0055E850`) | Per logo: clan id, version, byte length, raw JPEG. Saved as `ClanData\<id>\logo.<version>.jpg` and shown in `PC_CLAN_IMAGE` |
| `DOWNLOAD_MEMBER_LIST` | 0x00561DC0 | clan name (scoreboard `+0x3C`) | Per member: name, id, description |
| `DOWNLOAD_MEMBER_LOGOS` | 0x005621B0 | the same shape as clan logos | The same shape. Saved under `MemberData\` |
| `DOWNLOAD_SCOREBOARD` | 0x00563480 | clan name (scoreboard `+0x3C`) | Per row: activity 0-38, score (float), player name, one more line (unused) |
| `UPLOAD_SCORES` | 0x00561450 / 0x0055F480 | Per activity: id, `LIB` (low is best) or `HIB`, score as `%.6f` | nothing |

The 39 activities are the `TEXT_GUI_SCOREBOARD_ACTIVITY_NAME_*` entries (murders, decapitations, wealth, Arena, minigames and others).

## Our side

- Mod: `client-mod/src/scoreboard.cpp` patches the host push and the port load (`mov eax, <port>; nop`) before the game runs. The setting comes from `[scoreboard] host/port` in fable_online.ini or from `FABLE_ONLINE_SCOREBOARD=host:port`.
- Server: `server/src/scoreboard/` on the main HTTP port (`POST /index.php`). Players log in with their account name and a scoreboard code (`POST /launcher/v1/scoreboard-code`), never their real password. Everyone joins the realm clan "Chapters of the Lost" (id 1). Admins manage clans under `/admin/v1/clans*`. `GET /admin/v1/scoreboard/recent` shows the last requests that were decoded.
