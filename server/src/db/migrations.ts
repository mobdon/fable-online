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
  /* 2: clans and the in-game online scoreboard */ `
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
];
