import type { DatabaseInstance } from '../db.js';

// The Fabricator's tables. Account columns (owner, account, reporter) are plain TEXT with no foreign
// key to users: deleting an account marks its mods removed but keeps the rows.
// fab_mods_fts is an ordinary FTS5 table whose rowid is fab_mods.id; the write functions keep it.
export function initFabricatorSchema(db: DatabaseInstance): void {
  db.exec(`
    CREATE TABLE IF NOT EXISTS fab_mods (
      id INTEGER PRIMARY KEY,
      owner TEXT NOT NULL,
      slug TEXT NOT NULL,
      name TEXT NOT NULL,
      game TEXT NOT NULL,
      type TEXT NOT NULL,
      short_desc TEXT NOT NULL DEFAULT '',
      long_desc TEXT NOT NULL DEFAULT '',
      tags TEXT NOT NULL DEFAULT '[]',
      side TEXT NOT NULL DEFAULT 'both',
      requires TEXT NOT NULL DEFAULT '[]',
      state TEXT NOT NULL DEFAULT 'live' CHECK(state IN ('live', 'taken_down', 'removed')),
      created_at INTEGER NOT NULL,
      updated_at INTEGER NOT NULL
    );
    CREATE UNIQUE INDEX IF NOT EXISTS idx_fab_mods_game_name ON fab_mods(game, lower(name));
    CREATE INDEX IF NOT EXISTS idx_fab_mods_owner ON fab_mods(owner);

    CREATE TABLE IF NOT EXISTS fab_versions (
      id INTEGER PRIMARY KEY,
      mod_id INTEGER NOT NULL REFERENCES fab_mods(id),
      version TEXT NOT NULL,
      notes TEXT NOT NULL DEFAULT '',
      size INTEGER NOT NULL,
      sha256 TEXT NOT NULL,
      file_key TEXT NOT NULL,
      state TEXT NOT NULL DEFAULT 'live' CHECK(state IN ('live', 'files_deleted')),
      created_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_fab_versions_mod ON fab_versions(mod_id);

    CREATE TABLE IF NOT EXISTS fab_screens (
      mod_id INTEGER NOT NULL REFERENCES fab_mods(id),
      pos INTEGER NOT NULL,
      image_key TEXT NOT NULL,
      width INTEGER NOT NULL,
      height INTEGER NOT NULL,
      PRIMARY KEY (mod_id, pos)
    );

    CREATE TABLE IF NOT EXISTS fab_library (
      account TEXT NOT NULL,
      mod_id INTEGER NOT NULL REFERENCES fab_mods(id),
      added_at INTEGER NOT NULL,
      PRIMARY KEY (account, mod_id)
    );

    CREATE TABLE IF NOT EXISTS fab_downloads (
      account TEXT NOT NULL,
      mod_id INTEGER NOT NULL REFERENCES fab_mods(id),
      at INTEGER NOT NULL,
      PRIMARY KEY (account, mod_id)
    );

    CREATE TABLE IF NOT EXISTS fab_ratings (
      account TEXT NOT NULL,
      mod_id INTEGER NOT NULL REFERENCES fab_mods(id),
      stars INTEGER NOT NULL CHECK(stars BETWEEN 1 AND 5),
      updated_at INTEGER NOT NULL,
      PRIMARY KEY (account, mod_id)
    );

    CREATE TABLE IF NOT EXISTS fab_reports (
      id INTEGER PRIMARY KEY,
      mod_id INTEGER NOT NULL REFERENCES fab_mods(id),
      reporter TEXT NOT NULL,
      reason TEXT NOT NULL,
      text TEXT NOT NULL DEFAULT '',
      created_at INTEGER NOT NULL,
      handled_by TEXT,
      outcome TEXT,
      UNIQUE(mod_id, reporter)
    );

    CREATE TABLE IF NOT EXISTS fab_creators (
      account TEXT PRIMARY KEY,
      publishing INTEGER NOT NULL DEFAULT 1,
      bytes_used INTEGER NOT NULL DEFAULT 0
    );

    CREATE TABLE IF NOT EXISTS fab_uploads (
      id TEXT PRIMARY KEY,
      account TEXT NOT NULL,
      mod_id INTEGER,
      meta TEXT NOT NULL,
      size INTEGER NOT NULL,
      sha256 TEXT NOT NULL,
      chunk_bytes INTEGER NOT NULL,
      chunks_done INTEGER NOT NULL DEFAULT 0,
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_fab_uploads_account ON fab_uploads(account);

    -- screenshots sent during an upload session; they become fab_screens when it is published
    CREATE TABLE IF NOT EXISTS fab_upload_screens (
      upload_id TEXT NOT NULL REFERENCES fab_uploads(id) ON DELETE CASCADE,
      pos INTEGER NOT NULL,
      image_key TEXT NOT NULL,
      width INTEGER NOT NULL,
      height INTEGER NOT NULL,
      PRIMARY KEY (upload_id, pos)
    );

    CREATE TABLE IF NOT EXISTS fab_admin_log (
      id INTEGER PRIMARY KEY,
      who TEXT NOT NULL,
      action TEXT NOT NULL,
      mod_id INTEGER,
      detail TEXT NOT NULL DEFAULT '',
      at INTEGER NOT NULL
    );

    CREATE VIRTUAL TABLE IF NOT EXISTS fab_mods_fts USING fts5(name, short_desc, long_desc, tags, author);
  `);
}
