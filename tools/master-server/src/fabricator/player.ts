import { randomBytes } from 'node:crypto';
import type { DatabaseInstance } from '../db.js';
import type { FabSettings } from './settings.js';

// A player's side of the store: the library, download addresses, ratings and reports.
// Download addresses are 32 random bytes (hex) kept only in memory: a restart forgets them and clients ask again.

export const FILES_PATH = '/api/v1/fabricator/files/';
const IMAGE_PATH = '/api/v1/fabricator/images/';
const HOUR_MS = 3_600_000;
export const REPORT_REASONS = new Set(['malware', 'stolen', 'broken', 'offensive', 'other']);
export const REPORT_TEXT_MAX = 500;

export interface DownloadGrant { key: string; modId: number; account: string; expires: number }

// The live download addresses and, per account, when each was issued (for the per-hour limit).
export class DownloadTokens {
  private readonly grants = new Map<string, DownloadGrant>();
  private readonly issued = new Map<string, number[]>();

  // Drops expired addresses and issue times older than an hour.
  sweep(now: number): void {
    for (const [t, g] of this.grants) if (g.expires <= now) this.grants.delete(t);
    for (const [a, times] of this.issued) {
      const kept = times.filter((at) => at > now - HOUR_MS);
      if (kept.length === 0) this.issued.delete(a); else this.issued.set(a, kept);
    }
  }

  issuedInLastHour(account: string, now: number): number {
    return (this.issued.get(account) ?? []).filter((at) => at > now - HOUR_MS).length;
  }

  issue(grant: Omit<DownloadGrant, 'expires'>, now: number, ttlMs: number): string {
    const token = randomBytes(32).toString('hex');
    this.grants.set(token, { ...grant, expires: now + ttlMs });
    const times = this.issued.get(grant.account) ?? [];
    times.push(now);
    this.issued.set(grant.account, times);
    return token;
  }

  // The grant for a token that has not expired, or null.
  get(token: string, now: number): DownloadGrant | null {
    const g = this.grants.get(token);
    if (!g) return null;
    if (g.expires <= now) { this.grants.delete(token); return null; }
    return g;
  }
}

const modState = (db: DatabaseInstance, modId: number): string | undefined =>
  (db.prepare('SELECT state FROM fab_mods WHERE id = ?').get(modId) as { state: string } | undefined)?.state;

export function modIsLive(db: DatabaseInstance, modId: number): boolean {
  return modState(db, modId) === 'live';
}

// Adds a live mod to the account's library (already there is fine). False when the mod is not live.
export function addToLibrary(db: DatabaseInstance, account: string, modId: number, now: number): boolean {
  if (!modIsLive(db, modId)) return false;
  db.prepare('INSERT OR IGNORE INTO fab_library (account, mod_id, added_at) VALUES (?, ?, ?)').run(account, modId, now);
  return true;
}

export function removeFromLibrary(db: DatabaseInstance, account: string, modId: number): void {
  db.prepare('DELETE FROM fab_library WHERE account = ? AND mod_id = ?').run(account, modId);
}

export interface LibraryEntry {
  id: number; name: string; game: string; type: string; state: string;
  version: string; version_id: number; sha256: string; size: number; screen: string;
}

const LATEST = `(SELECT id FROM fab_versions WHERE mod_id = m.id AND state = 'live' ORDER BY created_at DESC, id DESC LIMIT 1)`;

// Every mod in the library, newest first, taken-down and removed ones included with their state.
// A mod with no live version shows version_id 0.
export function library(db: DatabaseInstance, account: string): LibraryEntry[] {
  const rows = db.prepare(`
    SELECT m.id, m.name, m.game, m.type, m.state,
      COALESCE(v.version, '') AS version, COALESCE(v.id, 0) AS version_id, COALESCE(v.sha256, '') AS sha256, COALESCE(v.size, 0) AS size,
      (SELECT image_key FROM fab_screens WHERE mod_id = m.id ORDER BY pos LIMIT 1) AS image_key
    FROM fab_library l JOIN fab_mods m ON m.id = l.mod_id
    LEFT JOIN fab_versions v ON v.id = ${LATEST}
    WHERE l.account = ? ORDER BY l.added_at DESC, m.id DESC`).all(account) as (Omit<LibraryEntry, 'screen'> & { image_key: string | null })[];
  return rows.map(({ image_key, ...r }) => ({ ...r, screen: image_key ? IMAGE_PATH + image_key : '' }));
}

export type DownloadResult = { url: string; sha256: string; size: number; version_id: number } | { error: 'not_found' | 'taken_down' | 'rate_limited' };

// A download address for the mod's latest live version. Adds the mod to the library and counts one download
// per account and mod, however many addresses are issued.
export function issueDownload(db: DatabaseInstance, s: FabSettings, account: string, modId: number, now: number, tokens: DownloadTokens): DownloadResult {
  const state = modState(db, modId);
  if (state === undefined) return { error: 'not_found' };
  if (state !== 'live') return { error: 'taken_down' };
  const v = db.prepare(`SELECT id, sha256, size, file_key FROM fab_versions WHERE mod_id = ? AND state = 'live' ORDER BY created_at DESC, id DESC LIMIT 1`)
    .get(modId) as { id: number; sha256: string; size: number; file_key: string } | undefined;
  if (!v) return { error: 'not_found' };
  tokens.sweep(now);
  if (tokens.issuedInLastHour(account, now) >= s.downloadsPerHour) return { error: 'rate_limited' };
  db.transaction(() => {
    db.prepare('INSERT OR IGNORE INTO fab_library (account, mod_id, added_at) VALUES (?, ?, ?)').run(account, modId, now);
    db.prepare('INSERT OR IGNORE INTO fab_downloads (account, mod_id, at) VALUES (?, ?, ?)').run(account, modId, now);
  })();
  const token = tokens.issue({ key: v.file_key, modId, account }, now, s.downloadTtlMs);
  return { url: FILES_PATH + token, sha256: v.sha256, size: v.size, version_id: v.id };
}

// Stars 1-5 (a whole number) on a mod in the player's library; a new rating replaces the old one. A creator does not
// rate their own mod.
export function rate(db: DatabaseInstance, account: string, modId: number, stars: unknown, now: number = Date.now()): 'ok' | 'not_in_library' | 'bad_stars' | 'own_mod' {
  if (typeof stars !== 'number' || !Number.isInteger(stars) || stars < 1 || stars > 5) return 'bad_stars';
  if (db.prepare('SELECT 1 FROM fab_mods WHERE id = ? AND owner = ?').get(modId, account)) return 'own_mod';
  if (!db.prepare('SELECT 1 FROM fab_library WHERE account = ? AND mod_id = ?').get(account, modId)) return 'not_in_library';
  db.prepare(`INSERT INTO fab_ratings (account, mod_id, stars, updated_at) VALUES (?, ?, ?, ?)
    ON CONFLICT(account, mod_id) DO UPDATE SET stars = excluded.stars, updated_at = excluded.updated_at`).run(account, modId, stars, now);
  return 'ok';
}

// One report per account and mod. The caller checks that the mod exists; text is cut to 500 characters.
export function report(db: DatabaseInstance, account: string, modId: number, reason: unknown, text: unknown, now: number): 'ok' | 'already' | 'bad_reason' {
  if (typeof reason !== 'string' || !REPORT_REASONS.has(reason)) return 'bad_reason';
  const body = typeof text === 'string' ? text.slice(0, REPORT_TEXT_MAX) : '';
  const r = db.prepare('INSERT OR IGNORE INTO fab_reports (mod_id, reporter, reason, text, created_at) VALUES (?, ?, ?, ?, ?)')
    .run(modId, account, reason, body, now);
  return r.changes === 0 ? 'already' : 'ok';
}
