import Database from 'better-sqlite3';
import fs from 'node:fs';
import path from 'node:path';

export type Db = ReturnType<typeof Database>;

export function openDb(dbPath: string): Db {
  if (dbPath !== ':memory:') {
    fs.mkdirSync(path.dirname(path.resolve(dbPath)), { recursive: true });
    // Files created below (db, -wal, -shm) must be owner-only from the start.
    process.umask(0o077);
  }
  const db = new Database(dbPath);
  if (dbPath !== ':memory:') {
    db.pragma('journal_mode = WAL');
    ownerOnly(dbPath);
  }
  db.exec(`
    CREATE TABLE IF NOT EXISTS users (
      name TEXT PRIMARY KEY,
      pw_hash TEXT NOT NULL,
      totp_secret TEXT NOT NULL,
      must_change INTEGER NOT NULL DEFAULT 1,
      failed INTEGER NOT NULL DEFAULT 0,
      locked_until INTEGER NOT NULL DEFAULT 0,
      last_totp_step INTEGER NOT NULL DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS sessions (
      id TEXT PRIMARY KEY,
      name TEXT NOT NULL,
      csrf TEXT NOT NULL,
      created INTEGER NOT NULL,
      seen INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS audit (
      id INTEGER PRIMARY KEY,
      time INTEGER NOT NULL,
      name TEXT NOT NULL,
      action TEXT NOT NULL,
      detail TEXT NOT NULL DEFAULT ''
    );
  `);
  if (dbPath !== ':memory:') ownerOnly(dbPath);
  return db;
}

// An existing file keeps its old mode, and the -wal/-shm files only exist once WAL is on.
function ownerOnly(dbPath: string): void {
  for (const f of [dbPath, dbPath + '-wal', dbPath + '-shm']) {
    try { fs.chmodSync(f, 0o600); } catch { /* missing file, or Windows */ }
  }
}
