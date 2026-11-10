import Database from 'better-sqlite3';
import fs from 'node:fs';
import path from 'node:path';

export type DatabaseInstance = ReturnType<typeof Database>;

export function initDb(dbPath: string = process.env.DB_PATH || 'master.db'): DatabaseInstance {
  if (dbPath !== ':memory:') {
    const dir = path.dirname(path.resolve(dbPath));
    if (!fs.existsSync(dir)) {
      fs.mkdirSync(dir, { recursive: true });
    }
  }

  const db = new Database(dbPath);
  // Compiling a statement costs far more than running it, and the server only ever uses a few dozen.
  const compiled = new Map<string, ReturnType<typeof db.prepare>>();
  const compile = db.prepare.bind(db);
  (db as any).prepare = (sql: string) => {
    let s = compiled.get(sql);
    if (!s) compiled.set(sql, s = compile(sql));
    return s;
  };

  // Enable WAL mode for high concurrency
  if (dbPath !== ':memory:') {
    db.pragma('journal_mode = WAL');
  }
  db.pragma('synchronous = NORMAL');
  db.pragma('foreign_keys = ON');

  // Initialize Schema
  db.exec(`
    CREATE TABLE IF NOT EXISTS users (
      uuid TEXT PRIMARY KEY,
      display_name TEXT NOT NULL,
      discriminator TEXT NOT NULL,
      xuid TEXT NOT NULL UNIQUE,
      token_hash TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      last_seen INTEGER NOT NULL
    );

    CREATE UNIQUE INDEX IF NOT EXISTS idx_users_display_discriminator
      ON users(display_name, discriminator);

    CREATE TABLE IF NOT EXISTS friends (
      user_uuid TEXT NOT NULL,
      friend_uuid TEXT NOT NULL,
      status TEXT NOT NULL CHECK(status IN ('pending', 'accepted', 'blocked')),
      updated_at INTEGER NOT NULL,
      PRIMARY KEY (user_uuid, friend_uuid),
      FOREIGN KEY (user_uuid) REFERENCES users(uuid) ON DELETE CASCADE,
      FOREIGN KEY (friend_uuid) REFERENCES users(uuid) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS lobbies (
      lobby_id TEXT PRIMARY KEY,
      host_uuid TEXT NOT NULL,
      host_xuid TEXT NOT NULL,
      title_id TEXT NOT NULL,
      room_code TEXT NOT NULL UNIQUE,
      session_info TEXT NOT NULL,
      properties_json TEXT NOT NULL DEFAULT '[]',
      contexts_json TEXT NOT NULL DEFAULT '[]',
      qos_data TEXT NOT NULL DEFAULT '',
      region TEXT NOT NULL DEFAULT 'global',
      is_public INTEGER NOT NULL DEFAULT 1,
      max_players INTEGER NOT NULL DEFAULT 8,
      current_players INTEGER NOT NULL DEFAULT 1,
      metadata_json TEXT,
      created_at INTEGER NOT NULL,
      updated_at INTEGER NOT NULL
    );

    CREATE INDEX IF NOT EXISTS idx_lobbies_title_region
      ON lobbies(title_id, region);

    CREATE INDEX IF NOT EXISTS idx_lobbies_room_code
      ON lobbies(room_code);
  `);

  // Columns added after a database was first made.
  const lobbyColumns = (db.prepare('PRAGMA table_info(lobbies)').all() as { name: string }[]).map((c) => c.name);
  if (!lobbyColumns.includes('qos_data')) {
    db.exec("ALTER TABLE lobbies ADD COLUMN qos_data TEXT NOT NULL DEFAULT ''");
  }
  const userColumns = (db.prepare('PRAGMA table_info(users)').all() as { name: string }[]).map((c) => c.name);
  if (!userColumns.includes('icon')) {   // the profile picture's name (icons/<TitleID>/<n>.png), '' for the letter
    db.exec("ALTER TABLE users ADD COLUMN icon TEXT NOT NULL DEFAULT ''");
  }
  if (!lobbyColumns.includes('invite_only')) {
    db.exec('ALTER TABLE lobbies ADD COLUMN invite_only INTEGER NOT NULL DEFAULT 0');
  }

  return db;
}
