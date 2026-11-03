# Launcher API (v1)

HTTP + JSON between `fable_online_launcher.exe` and the gateway. All paths are
under `/launcher/v1`. Bodies are UTF-8 JSON. Errors are any non-2xx status with
`{"error": "<code>", "message": "<text a player can read>"}`.

The launcher stores only the refresh token, encrypted with Windows DPAPI.
Passwords never touch disk.

"Region" in this API means a server realm or shard (for example `albion-1`),
not an in-game area. The server picks the in-game area from the hero's save.

## GET /status

No auth. Polled every 30 s while the launcher is open.

```json
{
  "online": true,
  "motd": "Welcome to Albion",
  "protocol": 1,
  "regions": [
    { "id": "eu-1", "name": "Albion EU 1", "status": "online",
      "players": 132, "capacity": 1000, "ping_host": "eu1.example.net", "ping_port": 27015 }
  ]
}
```

`status` is one of `online`, `busy`, `full`, `maintenance`, `offline`.
The launcher only lets players join `online` or `busy` regions.

## GET /news

No auth. Newest first, at most 20.

```json
{ "items": [ { "id": "n1", "title": "Patch 0.1", "date": "2026-11-03", "body": "Plain text.", "url": "https://..." } ] }
```

## POST /login

```json
{ "username": "hero", "password": "..." }
```
→ `200`
```json
{ "access_token": "...", "refresh_token": "...", "expires_in": 900,
  "account": { "id": "a1", "name": "hero" } }
```
`401 {"error":"bad_credentials"}`, `403 {"error":"banned","message":"..."}`,
`429 {"error":"rate_limited"}`.

## POST /register

Same body as `/login`, same `200` response (the new account is logged in).
`409 {"error":"name_taken"}`, `400 {"error":"weak_password","message":"..."}`.

## POST /refresh

```json
{ "refresh_token": "..." }
```
→ same shape as `/login`. Refresh tokens rotate: the old one stops working.
`401` means log in again.

## POST /logout  (Bearer)

Revokes the refresh token. `204`.

## GET /manifest?channel=live

No auth, so patching works before login. Describes every file the client
needs next to the launcher (the mod DLL, overlay assets, and the launcher
itself). Paths are relative to the launcher's folder and use `/`.

```json
{
  "version": "0.1.0",
  "protocol": 1,
  "base_url": "https://cdn.example.net/live/0.1.0/",
  "files": [
    { "path": "fable_online.dll", "size": 2097152, "sha256": "hex..." },
    { "path": "fable_online_launcher.exe", "size": 693504, "sha256": "hex..." }
  ]
}
```

Each file downloads from `base_url + path` (or a per-file `url`, if present).
The launcher checks size and SHA-256 before it swaps a file in. If
`fable_online_launcher.exe` changed, the launcher renames itself to
`fable_online_launcher.old`, writes the new copy and restarts.

## POST /join  (Bearer)

```json
{ "region": "eu-1", "client_version": "0.1.0", "hero_id": 7 }
```
`hero_id` is optional; without it the account's most recently played hero is
used. Hero select is `GET/POST /heroes` (owned by the server, see `server/`).
→ `200`
```json
{ "ticket": "one-time, short-lived", "host": "eu1.example.net", "port": 27015 }
```
`409 {"error":"outdated"}` if `client_version` is older than the live manifest,
`503 {"error":"region_full"}`.

The launcher passes the ticket to Fable.exe in the environment
(`FABLE_ONLINE_TICKET`, `FABLE_ONLINE_HOST`, `FABLE_ONLINE_PORT`), and the mod
sends it as `Hello.token` (see `messages.proto`). The game server redeems the
ticket with the gateway once; it is never reused.
