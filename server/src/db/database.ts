import { DatabaseSync } from 'node:sqlite';
import fs from 'node:fs';
import path from 'node:path';
import { MIGRATIONS } from './migrations.js';

/**
 * Thin wrapper over node:sqlite. Everything is synchronous, so a `tx()` body runs to
 * completion with nothing else interleaving: that is what makes the gold ledger and item
 * moves atomic. Production swaps this for Postgres with SERIALIZABLE transactions behind
 * the same repository classes.
 */
export class Database {
  readonly raw: DatabaseSync;
  private depth = 0;

  constructor(file: string) {
    if (file !== ':memory:') fs.mkdirSync(path.dirname(path.resolve(file)), { recursive: true });
    this.raw = new DatabaseSync(file);
    this.raw.exec('PRAGMA journal_mode = WAL; PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 5000;');
    this.migrate();
  }

  private migrate(): void {
    this.raw.exec('CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)');
    const row = this.raw.prepare('SELECT MAX(version) AS v FROM schema_version').get() as { v: number | null };
    const current = row.v ?? 0;
    for (const [i, sql] of MIGRATIONS.entries()) {
      const version = i + 1;
      if (version <= current) continue;
      this.tx(() => {
        this.raw.exec(sql);
        this.raw.prepare('INSERT INTO schema_version (version) VALUES (?)').run(version);
      });
    }
  }

  /** Run fn in a transaction. Nested calls become savepoints. Any throw rolls back. */
  tx<T>(fn: () => T): T {
    const sp = `sp${this.depth}`;
    this.raw.exec(this.depth === 0 ? 'BEGIN IMMEDIATE' : `SAVEPOINT ${sp}`);
    this.depth++;
    try {
      const out = fn();
      this.depth--;
      this.raw.exec(this.depth === 0 ? 'COMMIT' : `RELEASE ${sp}`);
      return out;
    } catch (err) {
      this.depth--;
      this.raw.exec(this.depth === 0 ? 'ROLLBACK' : `ROLLBACK TO ${sp}; RELEASE ${sp}`);
      throw err;
    }
  }

  get<T>(sql: string, ...params: SqlParam[]): T | undefined {
    return this.raw.prepare(sql).get(...params) as T | undefined;
  }
  all<T>(sql: string, ...params: SqlParam[]): T[] {
    return this.raw.prepare(sql).all(...params) as T[];
  }
  run(sql: string, ...params: SqlParam[]): { changes: number; lastInsertRowid: number } {
    const r = this.raw.prepare(sql).run(...params);
    return { changes: Number(r.changes), lastInsertRowid: Number(r.lastInsertRowid) };
  }

  close(): void {
    this.raw.close();
  }
}

export type SqlParam = string | number | bigint | null | Uint8Array;
