import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { initDb } from '../src/db.js';
import { loadFabSettings } from '../src/fabricator/settings.js';

describe('fabricator schema and settings', () => {
  let dir: string;
  beforeEach(() => { dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fab-')); });
  afterEach(() => { fs.rmSync(dir, { recursive: true, force: true }); });

  it('creates every fab table and the search index', () => {
    const db = initDb(':memory:');
    const names = (db.prepare("SELECT name FROM sqlite_master WHERE name LIKE 'fab_%'").all() as { name: string }[]).map((r) => r.name);
    for (const t of ['fab_mods', 'fab_versions', 'fab_screens', 'fab_library', 'fab_downloads', 'fab_ratings',
      'fab_reports', 'fab_creators', 'fab_uploads', 'fab_admin_log', 'fab_mods_fts']) {
      expect(names).toContain(t);
    }
  });

  it('keeps mod names unique per game, ignoring case', () => {
    const db = initDb(':memory:');
    const ins = db.prepare(`INSERT INTO fab_mods (owner, slug, name, game, type, short_desc, long_desc, tags, side, requires, state, created_at, updated_at)
      VALUES ('a', ?, ?, ?, 'other', '', '', '[]', 'everyone', '[]', 'live', 0, 0)`);
    ins.run('x', 'Cool Mod', 'gears3');
    expect(() => ins.run('y', 'cool mod', 'gears3')).toThrow();
    expect(() => ins.run('z', 'cool mod', 'gears2')).not.toThrow();
  });

  it("a mod's side defaults to modcore's everyone", () => {
    const db = initDb(':memory:');
    db.prepare(`INSERT INTO fab_mods (owner, slug, name, game, type, created_at, updated_at) VALUES ('a', 's', 'Side Mod', 'gears3', 'other', 0, 0)`).run();
    expect(db.prepare('SELECT side FROM fab_mods').get()).toEqual({ side: 'everyone' });
  });

  it('search index accepts rows keyed by mod id', () => {
    const db = initDb(':memory:');
    db.prepare("INSERT INTO fab_mods_fts(rowid, name, short_desc, long_desc, tags, author) VALUES (7, 'Lancer Skin', 'x', 'y', 'z', 'bob')").run();
    const r = db.prepare("SELECT rowid FROM fab_mods_fts WHERE fab_mods_fts MATCH 'lancer'").all() as { rowid: number }[];
    expect(r.map((x) => x.rowid)).toEqual([7]);
    db.prepare('DELETE FROM fab_mods_fts WHERE rowid = 7').run();
    expect(db.prepare("SELECT rowid FROM fab_mods_fts WHERE fab_mods_fts MATCH 'lancer'").all()).toEqual([]);
  });

  it('loads defaults', () => {
    const s = loadFabSettings(dir, {});
    expect(s.maxFileBytes).toBe(4 * 2 ** 30);
    expect(s.minAccountAgeDays).toBe(3);
    expect(s.chunkBytes).toBe(16 * 2 ** 20);
    expect(s.diskFullFraction).toBe(0.85);
  });

  it('reads fabricator.json then env, ignoring bad entries', () => {
    fs.writeFileSync(path.join(dir, 'fabricator.json'), JSON.stringify({ creatorQuotaBytes: 1000, keepVersions: 'x', bogus: 5 }));
    const s = loadFabSettings(dir, { FAB_KEEP_VERSIONS: '5', FAB_MAX_FILES: 'abc' });
    expect(s.creatorQuotaBytes).toBe(1000);
    expect(s.keepVersions).toBe(5);
    expect(s.maxFiles).toBe(50000);
    expect((s as any).bogus).toBeUndefined();
  });

  it('survives a malformed fabricator.json', () => {
    fs.writeFileSync(path.join(dir, 'fabricator.json'), '{nope');
    expect(loadFabSettings(dir, {}).keepVersions).toBe(3);
  });
});
