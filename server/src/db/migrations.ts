// Append-only. Never edit a migration that has shipped; add a new one.
export const MIGRATIONS: string[] = [
  /* 1: accounts, sessions, heroes, gold ledger, items */ `
  CREATE TABLE accounts (
    id            INTEGER PRIMARY KEY,
    username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_hash TEXT NOT NULL,
    role          TEXT NOT NULL DEFAULT 'player' CHECK (role IN ('player','gm','admin')),
    banned_until  INTEGER,
    created_at    INTEGER NOT NULL,
    last_login_at INTEGER
  );

  -- Opaque bearer tokens. Only the SHA-256 of a token is stored.
  CREATE TABLE sessions (
    token_hash  TEXT PRIMARY KEY,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    kind        TEXT NOT NULL CHECK (kind IN ('access','refresh','join')),
    hero_id     INTEGER,            -- join tickets only
    realm       TEXT,               -- join tickets only
    expires_at  INTEGER NOT NULL,
    created_at  INTEGER NOT NULL
  );
  CREATE INDEX sessions_account ON sessions(account_id);

  CREATE TABLE heroes (
    id           INTEGER PRIMARY KEY,
    account_id   INTEGER NOT NULL REFERENCES accounts(id),
    name         TEXT NOT NULL UNIQUE COLLATE NOCASE,
    -- childhood and guild_training are solo private instances; world is the shared MMO.
    phase        TEXT NOT NULL DEFAULT 'childhood' CHECK (phase IN ('childhood','guild_training','world')),
    pvp_unlocked INTEGER NOT NULL DEFAULT 0,
    renown       INTEGER NOT NULL DEFAULT 0 CHECK (renown >= 0),
    region       TEXT NOT NULL,
    x REAL NOT NULL DEFAULT 0, y REAL NOT NULL DEFAULT 0, z REAL NOT NULL DEFAULT 0,
    created_at   INTEGER NOT NULL,
    last_played_at INTEGER
  );
  CREATE INDEX heroes_account ON heroes(account_id);

  -- Double-entry gold. Every hero has one wallet. System wallets (mint/sink) may go negative;
  -- hero wallets never can. Invariant: SUM(balance) over ALL wallets is always exactly 0.
  CREATE TABLE wallets (
    id       TEXT PRIMARY KEY,            -- 'hero:<id>' or 'system:<name>'
    kind     TEXT NOT NULL CHECK (kind IN ('hero','system')),
    balance  INTEGER NOT NULL DEFAULT 0,
    CHECK (kind = 'system' OR balance >= 0)
  );

  -- One row per economic action. idempotency_key makes retries and double-clicks harmless.
  CREATE TABLE ledger_tx (
    id              INTEGER PRIMARY KEY,
    idempotency_key TEXT UNIQUE,
    kind            TEXT NOT NULL,
    actor           TEXT NOT NULL,       -- 'hero:<id>', 'system', 'gm:<account>'
    meta            TEXT,                -- JSON
    created_at      INTEGER NOT NULL
  );

  CREATE TABLE ledger_entries (
    id        INTEGER PRIMARY KEY,
    tx_id     INTEGER NOT NULL REFERENCES ledger_tx(id),
    from_wallet TEXT NOT NULL REFERENCES wallets(id),
    to_wallet   TEXT NOT NULL REFERENCES wallets(id),
    amount    INTEGER NOT NULL CHECK (amount > 0),
    reason    TEXT NOT NULL
  );
  CREATE INDEX ledger_entries_from ON ledger_entries(from_wallet);
  CREATE INDEX ledger_entries_to ON ledger_entries(to_wallet);

  -- Every item instance has a unique, never-reused ID. Stackables are one row per stack.
  CREATE TABLE items (
    id          TEXT PRIMARY KEY,
    def_id      TEXT NOT NULL,
    quantity    INTEGER NOT NULL DEFAULT 1 CHECK (quantity > 0),
    owner_hero  INTEGER REFERENCES heroes(id),
    location    TEXT NOT NULL CHECK (location IN ('inventory','equipped','bank','escrow','destroyed')),
    created_tx  INTEGER NOT NULL REFERENCES ledger_tx(id),
    destroyed_tx INTEGER REFERENCES ledger_tx(id),
    CHECK ((location = 'destroyed') = (destroyed_tx IS NOT NULL))
  );
  CREATE INDEX items_owner ON items(owner_hero, location);

  -- Full provenance: where every item came from and every hand it passed through.
  CREATE TABLE item_history (
    id        INTEGER PRIMARY KEY,
    item_id   TEXT NOT NULL REFERENCES items(id),
    tx_id     INTEGER NOT NULL REFERENCES ledger_tx(id),
    action    TEXT NOT NULL CHECK (action IN ('mint','move','transfer','split','merge','destroy')),
    from_hero INTEGER,
    to_hero   INTEGER,
    quantity  INTEGER NOT NULL
  );
  CREATE INDEX item_history_item ON item_history(item_id);
  `,
  /* 2: combat progress and quest reward claims */ `
  -- What a hero has earned in combat. Experience only ever comes from server-seen kills.
  CREATE TABLE hero_progress (
    hero_id    INTEGER PRIMARY KEY REFERENCES heroes(id),
    experience INTEGER NOT NULL DEFAULT 0 CHECK (experience >= 0),
    kills      INTEGER NOT NULL DEFAULT 0,
    deaths     INTEGER NOT NULL DEFAULT 0,
    pvp_kills  INTEGER NOT NULL DEFAULT 0
  );

  -- One row per quest reward paid. Quests run on the client, so the server pays each
  -- known quest at most once per hero (or once per interval for repeatable ones) and never
  -- more than that quest's cap.
  CREATE TABLE quest_claims (
    id         INTEGER PRIMARY KEY,
    hero_id    INTEGER NOT NULL REFERENCES heroes(id),
    quest      TEXT NOT NULL,
    gold       INTEGER NOT NULL,
    renown     INTEGER NOT NULL DEFAULT 0,
    tx_id      INTEGER REFERENCES ledger_tx(id),
    claimed_at INTEGER NOT NULL
  );
  CREATE INDEX quest_claims_hero ON quest_claims(hero_id, quest);
  `,
  /* 3: per-hero world flags (demon doors opened, ...) */ `
  CREATE TABLE hero_flags (
    hero_id  INTEGER NOT NULL REFERENCES heroes(id),
    flag     TEXT NOT NULL,
    set_at   INTEGER NOT NULL,
    PRIMARY KEY (hero_id, flag)
  );
  `,
  /* 4: law: crimes, wanted level clock, kick-outs, sentences, stolen marks, steal log */ `
  -- One row per crime. Unsettled rows are the hero's open record.
  CREATE TABLE law_crimes (
    id          INTEGER PRIMARY KEY,
    hero_id     INTEGER NOT NULL REFERENCES heroes(id),
    kind        TEXT NOT NULL,
    town        TEXT,                 -- jurisdiction id; NULL = realm-wide (crimes against heroes)
    fine        INTEGER NOT NULL CHECK (fine >= 0),
    level       INTEGER NOT NULL CHECK (level BETWEEN 1 AND 4),
    victim      TEXT,                 -- 'hero:<id>' or a creature def name
    online_at   INTEGER NOT NULL,     -- the hero's online clock when it happened
    created_at  INTEGER NOT NULL,
    settled     TEXT CHECK (settled IN ('paid','bribed','forgiven','served')),
    settled_at  INTEGER,
    settled_tx  INTEGER REFERENCES ledger_tx(id)
  );
  CREATE INDEX law_crimes_hero ON law_crimes(hero_id, settled);

  -- Per-hero law state. online_ms only advances while the hero is in the world, so petty
  -- forgiveness, kick-outs and sentences can't be waited out logged off.
  CREATE TABLE law_heroes (
    hero_id      INTEGER PRIMARY KEY REFERENCES heroes(id),
    online_ms    INTEGER NOT NULL DEFAULT 0,
    day          TEXT,                -- UTC date the daily counters below belong to
    bribes_today INTEGER NOT NULL DEFAULT 0,
    stolen_today INTEGER NOT NULL DEFAULT 0,
    fenced_today INTEGER NOT NULL DEFAULT 0,
    last_steal_at INTEGER
  );

  CREATE TABLE law_kickouts (
    hero_id     INTEGER NOT NULL REFERENCES heroes(id),
    town        TEXT NOT NULL,
    until_online_ms INTEGER NOT NULL,
    PRIMARY KEY (hero_id, town)
  );

  CREATE TABLE law_sentences (
    id          INTEGER PRIMARY KEY,
    hero_id     INTEGER NOT NULL REFERENCES heroes(id),
    prison      TEXT NOT NULL CHECK (prison IN ('jail','bargate')),
    total_ms    INTEGER NOT NULL CHECK (total_ms > 0),
    served_ms   INTEGER NOT NULL DEFAULT 0,
    fines       INTEGER NOT NULL DEFAULT 0,  -- fines this sentence settled (jail pay-out price)
    reason      TEXT NOT NULL,
    warden_used INTEGER NOT NULL DEFAULT 0,
    escape_tried INTEGER NOT NULL DEFAULT 0,
    created_at  INTEGER NOT NULL,
    released_at INTEGER,
    release     TEXT CHECK (release IN ('served','paid'))
  );
  CREATE INDEX law_sentences_hero ON law_sentences(hero_id, released_at);
  CREATE UNIQUE INDEX law_sentences_one_open ON law_sentences(hero_id) WHERE released_at IS NULL;

  -- Stolen items keep a mark until 'until' (ms): no normal traders, no trading to heroes.
  CREATE TABLE law_stolen (
    item_id   TEXT PRIMARY KEY REFERENCES items(id),
    hero_id   INTEGER NOT NULL REFERENCES heroes(id),
    value     INTEGER NOT NULL,
    until     INTEGER NOT NULL
  );

  CREATE TABLE law_steals (
    id        INTEGER PRIMARY KEY,
    hero_id   INTEGER NOT NULL REFERENCES heroes(id),
    shop      TEXT NOT NULL,
    def_id    TEXT NOT NULL,
    day       TEXT NOT NULL,
    value     INTEGER NOT NULL,
    spotted   INTEGER NOT NULL,
    created_at INTEGER NOT NULL
  );
  CREATE INDEX law_steals_hero ON law_steals(hero_id, day);
  `,
  /* 5: clans and the in-game online scoreboard */ `
  -- Clans as Fable's own clan screen shows them (name, description, logo).
  CREATE TABLE clans (
    id            INTEGER PRIMARY KEY,
    name          TEXT NOT NULL UNIQUE COLLATE NOCASE,
    description   TEXT,
    logo_version  INTEGER NOT NULL DEFAULT 0,  -- bumped on every new logo; 0 = no logo
    logo_jpeg     BLOB,                         -- 64x64 JPEG, guaranteed free of CR LF pairs
    created_at    INTEGER NOT NULL
  );

  CREATE TABLE clan_members (
    clan_id    INTEGER NOT NULL REFERENCES clans(id) ON DELETE CASCADE,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    joined_at  INTEGER NOT NULL,
    PRIMARY KEY (clan_id, account_id)
  );
  CREATE INDEX clan_members_account ON clan_members(account_id);

  -- The realm-wide clan every scoreboard user joins, so the records screen always has a board.
  INSERT INTO clans (id, name, description, created_at)
    VALUES (1, 'Chapters of the Lost', 'Every hero in the realm', 0);

  -- The game hashes MD5(lower(username + password)). We never store a real password that
  -- way: players type a random scoreboard code instead, and only its MD5 lives here.
  CREATE TABLE scoreboard_logins (
    account_id INTEGER PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    md5        TEXT NOT NULL,
    created_at INTEGER NOT NULL
  );

  -- Best score per account per scoreboard activity (0-38).
  CREATE TABLE scoreboard_scores (
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    activity    INTEGER NOT NULL CHECK (activity BETWEEN 0 AND 38),
    score       REAL NOT NULL,
    low_is_best INTEGER NOT NULL DEFAULT 0,
    updated_at  INTEGER NOT NULL,
    PRIMARY KEY (account_id, activity)
  );
  CREATE INDEX scoreboard_scores_activity ON scoreboard_scores(activity, score);
  `,
  /* 6: renown taken from the hero's own Fable */ `
  -- Set the first time the mod reports the hero's renown: that report is the one-off import.
  ALTER TABLE heroes ADD COLUMN renown_imported_at INTEGER;

  -- Renown added from reports after the import, per UTC day, for the daily cap.
  CREATE TABLE renown_days (
    hero_id INTEGER NOT NULL REFERENCES heroes(id),
    day     TEXT NOT NULL,
    gained  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (hero_id, day)
  );
  `,
  /* 7: hero experience pools, bought stat and ability levels */ `
  -- The server's copy of Fable's CTCHeroExperience and ability levels. The mod writes this
  -- into the local game; nothing a client reports raises it without a server check.
  CREATE TABLE hero_xp (
    hero_id     INTEGER PRIMARY KEY REFERENCES heroes(id),
    general     INTEGER NOT NULL DEFAULT 0 CHECK (general >= 0),
    strength    INTEGER NOT NULL DEFAULT 0 CHECK (strength >= 0),
    skill       INTEGER NOT NULL DEFAULT 0 CHECK (skill >= 0),
    will        INTEGER NOT NULL DEFAULT 0 CHECK (will >= 0),
    total       INTEGER NOT NULL DEFAULT 0 CHECK (total >= 0),
    stats       TEXT NOT NULL DEFAULT '[]',  -- JSON: level per TrainableStatLevels slot
    abilities   TEXT NOT NULL DEFAULT '[]',  -- JSON: level per ability slot, 0 = not owned
    imported    INTEGER NOT NULL DEFAULT 0,  -- the save's own experience was taken once
    local_day   TEXT,                        -- UTC day of local_today
    local_today INTEGER NOT NULL DEFAULT 0,  -- experience taken from the local game that day
    updated_at  INTEGER NOT NULL
  );

  -- Every change to a hero's experience, for support and the economy watchdog.
  CREATE TABLE hero_xp_log (
    id       INTEGER PRIMARY KEY,
    hero_id  INTEGER NOT NULL REFERENCES heroes(id),
    kind     TEXT NOT NULL,   -- kill, local, import, buy, refund, admin
    detail   TEXT,
    general  INTEGER NOT NULL DEFAULT 0,
    strength INTEGER NOT NULL DEFAULT 0,
    skill    INTEGER NOT NULL DEFAULT 0,
    will     INTEGER NOT NULL DEFAULT 0,
    at       INTEGER NOT NULL
  );
  CREATE INDEX hero_xp_log_hero ON hero_xp_log(hero_id, at);
  `,
];
