# Parties

Up to four heroes play together as a party. The world server owns parties
(`server/src/party/parties.ts`); the mod only sends requests and shows the result.

## Rules

- A party is made by an invite and an accept. The inviter becomes the leader.
- Only the leader invites, removes a member (`kick`) and hands on the lead (`lead`). When the
  leader leaves, the member who joined first and is online takes over.
- A party of one breaks up.
- Invites last 60 seconds. A hero already in a party has to leave it before accepting another.
- Heroes in childhood or Guild training (private instances) cannot be in a party.
- A disconnected member keeps their place for 5 minutes, shown as offline, and comes back in on
  their next login or resume.
- Parties live in memory: a server restart ends them.
- Size: `FO_PARTY_MAX` (2 to 4, default 4).

## Same channel

A region can run several shared channels. Placement (`Director.pickShared`) puts a hero into
the channel of that region holding most of their online party, past the soft cap and up to the
hard cap. That applies on login, on every region change and when the client's Fable loads a
region by itself.

When a hero joins a party while in the same region as it but in another channel, the server
moves them across (`Director.moveToChannel`). `regroup` does the same on request. The client
gets `enter_region` with `via: "party"` and `loading: false`: the region is already loaded, so
only the heroes and monsters around them change. The law's region rules still apply (a prisoner
cannot regroup).

## Colours

Each member has one of the four colours of Fable's cut co-op mode, in its slot order: red,
blue, yellow, green (`COOP_SPIRIT_PLAYER_ONE` to `_FOUR`, `MULTIPLAYER_<colour>` glows; see
`tools/re-notes/09-coop-spirit.md`). A member keeps their colour while in the party; a freed
colour goes to the next one to join.

## Protocol (game socket)

Client to server, one message for every request:

```json
{"t":"party","op":"invite","name":"Thunder"}
```

| `op` | Fields | |
| --- | --- | --- |
| `invite` | `name` | leader, or anyone not in a party (that makes a new party on accept) |
| `accept` | `name?` | the invite from `name`, or the newest |
| `decline` | `name?` | |
| `leave` | | |
| `kick` | `name` | leader only |
| `lead` | `name` | leader only |
| `regroup` | | move to the channel most of the party is in, same region |
| `say` | `text` | party chat, up to 200 characters |

A malformed name is refused with a message, never a disconnect. Requests are rate limited
(2 a second, 1 a second for chat).

Server to client:

| `t` | Fields |
| --- | --- |
| `party` | `id` (null: not in a party), `leader`, `max`, `members`: `id`, `name`, `colour`, `slot` (0-3), `leader`, `online`, `region`, `channel`. Sent on join, on every change and when a member changes region or channel |
| `party_invite` | `from` (hero id), `name`, `size`, `expires_ms` |
| `party_chat` | `from`, `name`, `text` |
| `party_note` | `text`: something happened (joined, left, offline, lead changed, invite ran out) |
| `party_error` | `op`, `error`, `message` |

Party chat goes through the world server, not the chat service, so it works whether or not a
chat server is running, and only party members ever receive it.

## In the mod

- Chat commands: `/invite Name`, `/accept [Name]`, `/decline [Name]`, `/leave`, `/regroup`,
  `/party kick Name`, `/party lead Name`. `/p text` (or the Party channel) is party chat. `/kick`
  alone stays the moderators' command.
- Party list: a pewter panel on the left under Fable's own bars, with each member's colour,
  name, leader mark and region (`client-mod/src/overlay/overlay.cpp`).
- Orbs: a glow in the member's colour over each party member's stand-in, drawn by the overlay.
- Spirits: `FABLE_ONLINE_PARTY_SPIRITS=1` also makes the retail `COOP_SPIRIT_PLAYER_*` creature
  over each member's stand-in (`client-mod/src/game/spirits.cpp`). Not run in game yet, so off
  by default; any failure turns it off for the session.

## Analytics

`party_invite`, `party_formed`, `party_join`, `party_leave` (with `reason`), `party_disband`,
`party_chat` (length only), `party_regroup`.
