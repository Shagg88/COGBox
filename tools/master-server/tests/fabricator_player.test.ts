import { describe, it, expect, beforeEach } from 'vitest';
import { initDb, DatabaseInstance } from '../src/db.js';
import { FAB_DEFAULTS } from '../src/fabricator/settings.js';
import { addToLibrary, DownloadTokens, issueDownload, library, rate, report } from '../src/fabricator/player.js';

const NOW = 1_000_000_000;
const MIN = 60_000;

describe('fabricator player actions', () => {
  let db: DatabaseInstance;
  let tokens: DownloadTokens;
  const s = { ...FAB_DEFAULTS };

  const mod = (name: string, o: { state?: string; versions?: number; screen?: string } = {}): number => {
    const id = Number(db.prepare(`INSERT INTO fab_mods (owner, slug, name, game, type, state, created_at, updated_at)
      VALUES ('owner', ?, ?, 'gears3', 'gameplay', ?, 1, 1)`).run(name.toLowerCase(), name, o.state ?? 'live').lastInsertRowid);
    for (let v = 1; v <= (o.versions ?? 1); v++) {
      db.prepare(`INSERT INTO fab_versions (mod_id, version, size, sha256, file_key, state, created_at)
        VALUES (?, ?, ?, ?, ?, 'live', ?)`).run(id, `1.${v}`, 100 * v, String(v).repeat(64), `${String(v).repeat(64)}.zip`, v);
    }
    if (o.screen) db.prepare('INSERT INTO fab_screens (mod_id, pos, image_key, width, height) VALUES (?, 0, ?, 4, 4)').run(id, o.screen);
    return id;
  };
  const downloads = (id: number) => (db.prepare('SELECT COUNT(*) AS n FROM fab_downloads WHERE mod_id = ?').get(id) as { n: number }).n;

  beforeEach(() => { db = initDb(':memory:'); tokens = new DownloadTokens(); });

  it('counts one download per account per mod however many times issued', () => {
    const id = mod('Alpha');
    for (let i = 0; i < 5; i++) expect(issueDownload(db, s, 'acct-a', id, NOW + i, tokens)).toHaveProperty('url');
    expect(downloads(id)).toBe(1);
    issueDownload(db, s, 'acct-b', id, NOW, tokens);
    expect(downloads(id)).toBe(2);
  });

  it('issues the latest live version and adds the mod to the library', () => {
    const id = mod('Alpha', { versions: 3 });
    const r = issueDownload(db, s, 'acct-a', id, NOW, tokens);
    if ('error' in r) throw new Error(r.error);
    expect(r).toMatchObject({ sha256: '3'.repeat(64), size: 300 });
    expect(r.url).toMatch(/^\/api\/v1\/fabricator\/files\/[0-9a-f]{64}$/);
    const token = r.url.split('/').pop() as string;
    expect(tokens.get(token, NOW)).toMatchObject({ key: `${'3'.repeat(64)}.zip`, modId: id });
    expect(tokens.get(token, NOW + 16 * MIN)).toBeNull();
    expect(library(db, 'acct-a').map((m) => m.id)).toEqual([id]);
  });

  it('answers not_found and taken_down', () => {
    expect(issueDownload(db, s, 'acct-a', 999, NOW, tokens)).toEqual({ error: 'not_found' });
    expect(issueDownload(db, s, 'acct-a', mod('Gone', { state: 'taken_down' }), NOW, tokens)).toEqual({ error: 'taken_down' });
    expect(issueDownload(db, s, 'acct-a', mod('Removed', { state: 'removed' }), NOW, tokens)).toEqual({ error: 'taken_down' });
    expect(issueDownload(db, s, 'acct-a', mod('Empty', { versions: 0 }), NOW, tokens)).toEqual({ error: 'not_found' });
  });

  it('the 31st issueDownload in an hour is rate_limited', () => {
    const id = mod('Alpha');
    for (let i = 0; i < 30; i++) expect(issueDownload(db, s, 'acct-a', id, NOW + i * MIN, tokens)).toHaveProperty('url');
    expect(issueDownload(db, s, 'acct-a', id, NOW + 30 * MIN, tokens)).toEqual({ error: 'rate_limited' });
    expect(issueDownload(db, s, 'acct-b', id, NOW + 30 * MIN, tokens)).toHaveProperty('url');   // per account
    expect(issueDownload(db, s, 'acct-a', id, NOW + 60 * MIN + 1, tokens)).toHaveProperty('url'); // the first has aged out
  });

  it('rate refuses a mod not in the library and bad stars', () => {
    const id = mod('Alpha');
    expect(rate(db, 'acct-a', id, 4)).toBe('not_in_library');
    expect(addToLibrary(db, 'acct-a', id, NOW)).toBe(true);
    expect(rate(db, 'acct-a', id, 0)).toBe('bad_stars');
    expect(rate(db, 'acct-a', id, 6)).toBe('bad_stars');
    expect(rate(db, 'acct-a', id, 2.5)).toBe('bad_stars');
    expect(rate(db, 'acct-a', id, 4)).toBe('ok');
    expect(rate(db, 'acct-a', id, 5)).toBe('ok');
    expect(db.prepare('SELECT stars FROM fab_ratings WHERE account = ? AND mod_id = ?').all('acct-a', id)).toEqual([{ stars: 5 }]);
  });

  it('a second report from the same account is already; reasons are checked', () => {
    const id = mod('Alpha');
    expect(report(db, 'acct-a', id, 'spam', '', NOW)).toBe('bad_reason');
    expect(report(db, 'acct-a', id, 'broken', 'crashes on load', NOW)).toBe('ok');
    expect(report(db, 'acct-a', id, 'malware', '', NOW)).toBe('already');
    expect(report(db, 'acct-b', id, 'malware', '', NOW)).toBe('ok');
  });

  it('addToLibrary only takes live mods; the library keeps taken-down mods with their state', () => {
    const a = mod('Alpha', { versions: 2, screen: `${'c'.repeat(64)}.png` });
    const b = mod('Beta');
    expect(addToLibrary(db, 'acct-a', 999, NOW)).toBe(false);
    expect(addToLibrary(db, 'acct-a', mod('Gone', { state: 'taken_down' }), NOW)).toBe(false);
    expect(addToLibrary(db, 'acct-a', a, NOW)).toBe(true);
    expect(addToLibrary(db, 'acct-a', a, NOW)).toBe(true);
    expect(addToLibrary(db, 'acct-a', b, NOW + 1)).toBe(true);
    db.prepare("UPDATE fab_mods SET state = 'taken_down' WHERE id = ?").run(b);
    db.prepare("UPDATE fab_versions SET state = 'files_deleted' WHERE mod_id = ?").run(b);
    expect(library(db, 'acct-a')).toEqual([
      { id: b, name: 'Beta', game: 'gears3', type: 'gameplay', state: 'taken_down', version: '', version_id: 0, sha256: '', size: 0, screen: '' },
      {
        id: a, name: 'Alpha', game: 'gears3', type: 'gameplay', state: 'live', version: '1.2', version_id: expect.any(Number),
        sha256: '2'.repeat(64), size: 200, screen: `/api/v1/fabricator/images/${'c'.repeat(64)}.png`,
      },
    ]);
    expect(library(db, 'acct-b')).toEqual([]);
  });

  it("a creator cannot rate their own mod, even from their library", () => {
    const id = mod('Alpha');
    addToLibrary(db, 'owner', id, NOW);
    expect(rate(db, 'owner', id, 5)).toBe('own_mod');
    expect(db.prepare('SELECT COUNT(*) AS n FROM fab_ratings').get()).toEqual({ n: 0 });
  });
});
