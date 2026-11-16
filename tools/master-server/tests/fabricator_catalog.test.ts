import { describe, it, expect, beforeEach } from 'vitest';
import { initDb, DatabaseInstance } from '../src/db.js';
import { listMods, modPage, pageIdsSql } from '../src/fabricator/catalog.js';

const DAY = 86_400_000;
const NOW = 100 * DAY;

describe('fabricator catalog', () => {
  let db: DatabaseInstance;
  let n = 0;
  const user = (name: string, disc = '0001'): string => {
    const uuid = `u-${++n}`;
    db.prepare(`INSERT INTO users (uuid, display_name, discriminator, xuid, token_hash, created_at, last_seen)
                VALUES (?, ?, ?, ?, 'h', 0, 0)`).run(uuid, name, disc, String(1000 + n));
    return uuid;
  };
  const mod = (owner: string, name: string, o: Record<string, any> = {}): number => {
    const d = { game: 'gears3', type: 'gameplay', short: '', long: '', tags: [] as string[], state: 'live', created: 1, requires: [] as number[], ...o };
    const id = Number(db.prepare(`INSERT INTO fab_mods (owner, slug, name, game, type, short_desc, long_desc, tags, requires, state, created_at, updated_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`).run(owner, name.toLowerCase(), name, d.game, d.type, d.short, d.long,
      JSON.stringify(d.tags), JSON.stringify(d.requires), d.state, d.created, d.created).lastInsertRowid);
    const u = db.prepare('SELECT display_name, discriminator FROM users WHERE uuid = ?').get(owner) as { display_name: string; discriminator: string };
    db.prepare('INSERT INTO fab_mods_fts (rowid, name, short_desc, long_desc, tags, author) VALUES (?, ?, ?, ?, ?, ?)')
      .run(id, name, d.short, d.long, d.tags.join(' '), `${u.display_name}#${u.discriminator}`);
    db.prepare("INSERT INTO fab_versions (mod_id, version, notes, size, sha256, file_key, state, created_at) VALUES (?, '1.0', '', 100, ?, 'k', 'live', ?)")
      .run(id, 'a'.repeat(64), o.updated ?? d.created);
    return id;
  };
  const dl = (id: number, count: number, at: number) => {
    for (let i = 0; i < count; i++) db.prepare('INSERT INTO fab_downloads (account, mod_id, at) VALUES (?, ?, ?)').run(`d-${id}-${at}-${i}`, id, at);
  };
  const rate = (id: number, stars: number, count: number, tag = '') => {
    for (let i = 0; i < count; i++) db.prepare('INSERT INTO fab_ratings (account, mod_id, stars, updated_at) VALUES (?, ?, ?, 0)').run(`r-${id}-${stars}${tag}-${i}`, id, stars);
  };
  const q = (o: Record<string, any> = {}) => ({ sort: 'name' as const, page: 1, perPage: 24, ...o });
  const names = (o: Record<string, any> = {}) => listMods(db, q(o), NOW).mods.map((m) => m.name);

  beforeEach(() => { db = initDb(':memory:'); });

  it('search finds by a description word and by author name', () => {
    const a = user('Marcus'); const b = user('Dom');
    mod(a, 'Alpha', { long: 'Keeps your money between waves' });
    mod(b, 'Beta', { short: 'unrelated' });
    expect(names({ search: 'money' })).toEqual(['Alpha']);
    expect(names({ search: 'dom' })).toEqual(['Beta']);
    expect(names({ search: 'mon' })).toEqual(['Alpha']); // prefix on the last term
  });

  it('hostile search text never throws', () => {
    const a = user('Marcus'); mod(a, 'Alpha');
    for (const s of ['"', '***', 'NEAR(', 'a OR', '-"x', "'; DROP", '   ']) expect(() => listMods(db, q({ search: s }), NOW)).not.toThrow();
    expect(names({ search: '!!!' })).toEqual(['Alpha']);
  });

  it('filters by game and type', () => {
    const a = user('Marcus');
    mod(a, 'One', { game: 'gears1', type: 'audio' }); mod(a, 'Two', { game: 'gears3', type: 'audio' }); mod(a, 'Three', { game: 'gears3', type: 'campaign' });
    expect(names({ game: 'gears3' })).toEqual(['Three', 'Two']);
    expect(names({ type: 'audio' })).toEqual(['One', 'Two']);
    expect(names({ game: 'gears3', type: 'audio' })).toEqual(['Two']);
  });

  it('sort orders', () => {
    const a = user('Marcus');
    const m1 = mod(a, 'bravo', { created: 10, updated: 50 });
    const m2 = mod(a, 'Alpha', { created: 40, updated: 20 });
    const m3 = mod(a, 'Charlie', { created: 30, updated: 90 });
    const m4 = mod(a, 'Delta', { created: 20, updated: 30 });
    expect(names({ sort: 'name' })).toEqual(['Alpha', 'bravo', 'Charlie', 'Delta']);
    expect(names({ sort: 'new' })).toEqual(['Alpha', 'Charlie', 'Delta', 'bravo']);
    expect(names({ sort: 'updated' })).toEqual(['Charlie', 'bravo', 'Delta', 'Alpha']);
    dl(m1, 5, NOW - 60 * DAY); dl(m2, 2, NOW - DAY); dl(m3, 1, NOW - DAY); dl(m4, 1, NOW - 40 * DAY);
    // last 30 days: Alpha 2, Charlie 1; then by total: bravo 5, Delta 1
    expect(names({ sort: 'popular' })).toEqual(['Alpha', 'Charlie', 'bravo', 'Delta']);
    rate(m3, 5, 1); rate(m4, 4, 3);
    expect(names({ sort: 'rating' }).slice(0, 2)).toEqual(['Charlie', 'Delta']);
  });

  it('one 5-star ranks below 20 ratings averaging 4.6', () => {
    const a = user('Marcus');
    const one = mod(a, 'One'); const many = mod(a, 'Many');
    rate(one, 5, 1); rate(many, 5, 16); rate(many, 3, 4); // sum 92 over 20 = 4.6
    const r = listMods(db, q({ sort: 'rating' }), NOW).mods;
    expect(r.map((m) => m.name)).toEqual(['Many', 'One']);
    // sorted Bayesian, shown as the plain mean (one decimal)
    expect(r[0].rating).toBe(4.6);
    expect(r[0].ratings).toBe(20);
    expect(r[1].rating).toBe(5);
    expect(modPage(db, one, NOW)).toMatchObject({ rating: 5, ratings: 1 });
    const third = mod(a, 'Third');
    rate(third, 5, 1, 't'); rate(third, 4, 2, 't');   // 13 / 3 = 4.333...
    expect(listMods(db, q({ search: 'third' }), NOW).mods[0].rating).toBe(4.3);
  });

  it('never lists taken-down or removed mods', () => {
    const a = user('Marcus');
    mod(a, 'Live'); mod(a, 'Down', { state: 'taken_down' }); mod(a, 'Gone', { state: 'removed' });
    const r = listMods(db, q(), NOW);
    expect(r.mods.map((m) => m.name)).toEqual(['Live']);
    expect(r.total).toBe(1);
    expect(names({ search: 'down' })).toEqual([]);
  });

  it('pages', () => {
    const a = user('Marcus');
    for (const nm of ['A1', 'B2', 'C3', 'D4', 'E5']) mod(a, nm);
    const r = listMods(db, q({ perPage: 2, page: 2 }), NOW);
    expect(r.mods.map((m) => m.name)).toEqual(['C3', 'D4']);
    expect(r.total).toBe(5);
    expect(listMods(db, q({ perPage: 500 }), NOW).mods.length).toBe(5);
  });

  it('card fields and no uuid', () => {
    const a = user('Marcus', '0042');
    const id = mod(a, 'Alpha', { short: 'short' });
    db.prepare('INSERT INTO fab_screens VALUES (?, 0, ?, 4, 4)').run(id, 'b'.repeat(64) + '.png');
    const c = listMods(db, q(), NOW).mods[0];
    expect(c).toMatchObject({ id, name: 'Alpha', author: 'Marcus#0042', version: '1.0', size: 100, downloads: 0, ratings: 0,
      screen: `/api/v1/fabricator/images/${'b'.repeat(64)}.png` });
    expect(JSON.stringify(c)).not.toContain(a);
  });

  it('mod page', () => {
    const a = user('Marcus'); const b = user('Dom');
    const dep = mod(b, 'Dep');
    const id = mod(a, 'Main', { long: 'long text', tags: ['x', 'y'], requires: [dep, 9999] });
    for (let i = 0; i < 8; i++) mod(a, 'Other' + i);
    mod(a, 'Hidden', { state: 'taken_down' });
    db.prepare("INSERT INTO fab_versions (mod_id, version, notes, size, sha256, file_key, state, created_at) VALUES (?, '2.0', 'new', 200, ?, 'k2', 'live', 5)").run(id, 'c'.repeat(64));
    db.prepare('INSERT INTO fab_screens VALUES (?, 1, ?, 4, 4)').run(id, 'e'.repeat(64) + '.png');
    db.prepare('INSERT INTO fab_screens VALUES (?, 0, ?, 4, 4)').run(id, 'd'.repeat(64) + '.png');
    const p = modPage(db, id) as any;
    expect(p.long_desc).toBe('long text');
    expect(p.tags).toEqual(['x', 'y']);
    expect(p.requires).toEqual([{ id: dep, name: 'Dep' }]);
    expect(p.screens).toEqual([`/api/v1/fabricator/images/${'d'.repeat(64)}.png`, `/api/v1/fabricator/images/${'e'.repeat(64)}.png`]);
    expect(p.versions.map((v: any) => v.version)).toEqual(['2.0', '1.0']);
    expect(p.version).toBe('2.0');
    expect(p.others.length).toBe(6);
    expect(p.others.every((o: any) => o.id !== id && o.name !== 'Hidden' && o.author === 'Marcus#0001')).toBe(true);
    expect(JSON.stringify(p)).not.toContain(a);
  });

  it('mod page for taken down and removed', () => {
    const a = user('Marcus');
    const t = mod(a, 'Down', { state: 'taken_down' }); const r = mod(a, 'Gone', { state: 'removed' });
    expect(modPage(db, t)).toEqual({ id: t, name: 'Down', state: 'taken_down' });
    expect(modPage(db, r)).toBeNull();
    expect(modPage(db, 12345)).toBeNull();
  });
  it('shows only live versions', () => {
    const a = user('Marcus');
    const id = mod(a, 'Main');
    db.prepare("INSERT INTO fab_versions (mod_id, version, notes, size, sha256, file_key, state, created_at) VALUES (?, '2.0', '', 999, ?, 'k2', 'files_deleted', 50)").run(id, 'c'.repeat(64));
    const p = modPage(db, id) as any;
    expect(p.versions.map((v: any) => v.version)).toEqual(['1.0']);
    expect(p.version).toBe('1.0');
    expect(listMods(db, q({ sort: 'updated' }), NOW).mods[0]).toMatchObject({ version: '1.0', size: 100 });
  });

  it('listing query paths do no per-mod work before paging', () => {
    const plan = (sort: any) => db.prepare('EXPLAIN QUERY PLAN ' + pageIdsSql(sort, "m.state = 'live'"))
      .all({ since: 0, limit: 10, offset: 0 }).map((r: any) => r.detail).join(' | ');
    for (const sort of ['name', 'new']) {
      const p = plan(sort);
      expect(p).not.toMatch(/CORRELATED|fab_downloads|fab_ratings|fab_versions/);
    }
    expect(plan('updated')).not.toMatch(/CORRELATED|fab_downloads|fab_ratings/);
    const pop = plan('popular');
    expect(pop).not.toMatch(/CORRELATED|SCAN fab_downloads(?! USING)/);
    expect(pop).toContain('idx_fab_downloads_mod');
    const rat = plan('rating');
    expect(rat).not.toMatch(/CORRELATED|SCAN fab_ratings(?! USING)/);
    expect(rat).toContain('idx_fab_ratings_mod');
  });
});
