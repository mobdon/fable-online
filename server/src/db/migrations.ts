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
];
