import type { DatabaseInstance } from '../db.js';
import type { FabStore } from './store.js';
import { dropFileIfUnused } from './publish.js';

// Moderation for the admin panel (through the private API). `who` is the admin panel's signed-in name, never an
// account uuid; players show as `display_name#discriminator` tags.
// A take-down keeps the mod's search row: listings, pages and downloads already require state 'live', so restore
// only has to flip the state back.
const DAY_MS = 24 * 3600 * 1000;
const MONTH_MS = 30 * DAY_MS;
const TAG = (alias: string) => `COALESCE(${alias}.display_name || '#' || ${alias}.discriminator, 'unknown')`;
// Whether the mod's owner may publish (no fab_creators row: yes).
const PUBLISHING = (owner: string) => `COALESCE((SELECT publishing FROM fab_creators WHERE account = ${owner}), 1)`;

export interface QueueEntry {
  mod: { id: number; name: string; game: string; author: string; state: string; publishing: boolean };
  reports: { id: number; reason: string; text: string; reporter: string; created_at: number }[];
}

// Open reports grouped by mod, the mod with the newest report first and each mod's reports newest first.
export function reportsQueue(db: DatabaseInstance): QueueEntry[] {
  const rows = db.prepare(`SELECT r.id, r.mod_id, r.reason, r.text, r.created_at, ${TAG('ru')} AS reporter,
      m.name, m.game, m.state, ${TAG('ou')} AS author, ${PUBLISHING('m.owner')} AS publishing
    FROM fab_reports r JOIN fab_mods m ON m.id = r.mod_id
    LEFT JOIN users ru ON ru.uuid = r.reporter LEFT JOIN users ou ON ou.uuid = m.owner
    WHERE r.handled_by IS NULL AND m.state != 'removed' ORDER BY r.created_at DESC, r.id DESC`).all() as any[];
  const byMod = new Map<number, QueueEntry>();
  for (const r of rows) {
    let e = byMod.get(r.mod_id);
    if (!e) {
      e = { mod: { id: r.mod_id, name: r.name, game: r.game, author: r.author, state: r.state, publishing: r.publishing === 1 }, reports: [] };
      byMod.set(r.mod_id, e);
    }
    e.reports.push({ id: r.id, reason: r.reason, text: r.text, reporter: r.reporter, created_at: r.created_at });
  }
  return [...byMod.values()];
}

function logAction(db: DatabaseInstance, who: string, action: string, modId: number | null, detail: string, now: number): void {
  db.prepare('INSERT INTO fab_admin_log (who, action, mod_id, detail, at) VALUES (?, ?, ?, ?, ?)').run(who, action, modId, detail, now);
}

function handleReports(db: DatabaseInstance, modId: number, who: string, outcome: string): void {
  db.prepare('UPDATE fab_reports SET handled_by = ?, outcome = ? WHERE mod_id = ? AND handled_by IS NULL').run(who, outcome, modId);
}

const hasLiveVersion = (db: DatabaseInstance, modId: number): boolean =>
  !!db.prepare("SELECT 1 FROM fab_versions WHERE mod_id = ? AND state = 'live' LIMIT 1").get(modId);

// Moves a mod from one state to another, logged; false when it is not in `from`.
function flip(db: DatabaseInstance, modId: number, from: string, to: string, action: string, who: string, now: number): boolean {
  const r = db.prepare('UPDATE fab_mods SET state = ? WHERE id = ? AND state = ?').run(to, modId, from);
  if (r.changes === 0) return false;
  logAction(db, who, action, modId, '', now);
  handleReports(db, modId, who, to === 'live' ? 'restored' : to);
  return true;
}

export const takeDown = (db: DatabaseInstance, modId: number, who: string, now: number): boolean =>
  db.transaction(() => flip(db, modId, 'live', 'taken_down', 'takedown', who, now))();

// 'no files' when the mod has no live version left (its files were deleted): it stays taken down.
export const restore = (db: DatabaseInstance, modId: number, who: string, now: number): boolean | 'no files' =>
  db.transaction(() => {
    if (db.prepare("SELECT 1 FROM fab_mods WHERE id = ? AND state = 'taken_down'").get(modId) && !hasLiveVersion(db, modId)) return 'no files' as const;
    return flip(db, modId, 'taken_down', 'live', 'restore', who, now);
  })();

// Gives up every live version's file (quota back to the owner); a file another live version still uses stays.
// A live mod is also taken down (nothing is left to download). False when there is no such mod or it is removed.
export function deleteFiles(db: DatabaseInstance, store: FabStore, modId: number, who: string, now: number): boolean {
  const keys = db.transaction(() => {
    const m = db.prepare("SELECT owner FROM fab_mods WHERE id = ? AND state != 'removed'").get(modId) as { owner: string } | undefined;
    if (!m) return null;
    db.prepare("UPDATE fab_mods SET state = 'taken_down' WHERE id = ? AND state = 'live'").run(modId);
    const vs = db.prepare("SELECT id, size, file_key FROM fab_versions WHERE mod_id = ? AND state = 'live'").all(modId) as { id: number; size: number; file_key: string }[];
    for (const v of vs) {
      db.prepare("UPDATE fab_versions SET state = 'files_deleted' WHERE id = ?").run(v.id);
      db.prepare('UPDATE fab_creators SET bytes_used = MAX(0, bytes_used - ?) WHERE account = ?').run(v.size, m.owner);
    }
    logAction(db, who, 'delete-files', modId, `${vs.length} versions`, now);
    handleReports(db, modId, who, 'files_deleted');
    return vs.map((v) => v.file_key);
  })();
  if (!keys) return false;
  for (const k of new Set(keys)) dropFileIfUnused(db, store, k);
  return true;
}

// Allows or stops publishing for the account with this tag; false when no account has it.
export function setPublishing(db: DatabaseInstance, tag: string, allowed: boolean, who: string, now: number): boolean {
  const at = typeof tag === 'string' ? tag.lastIndexOf('#') : -1;
  if (at <= 0) return false;
  const u = db.prepare('SELECT uuid FROM users WHERE display_name = ? AND discriminator = ?').get(tag.slice(0, at), tag.slice(at + 1)) as { uuid: string } | undefined;
  if (!u) return false;
  db.transaction(() => {
    db.prepare(`INSERT INTO fab_creators (account, publishing, bytes_used) VALUES (?, ?, 0)
      ON CONFLICT(account) DO UPDATE SET publishing = excluded.publishing`).run(u.uuid, allowed ? 1 : 0);
    logAction(db, who, 'publishing', null, `${tag} ${allowed ? 'allowed' : 'stopped'}`, now);
  })();
  return true;
}

// The UTC day (YYYY-MM-DD) of a time.
const dayOf = (t: number): string => new Date(t).toISOString().slice(0, 10);

// Adds bytes a download address really sent to its UTC day (the server's egress, for the panel).
export function addTraffic(db: DatabaseInstance, now: number, bytes: number): void {
  if (!(bytes > 0)) return;
  db.prepare('INSERT INTO fab_traffic (day, bytes) VALUES (?, ?) ON CONFLICT(day) DO UPDATE SET bytes = bytes + excluded.bytes')
    .run(dayOf(now), Math.floor(bytes));
}

export async function fabStats(db: DatabaseInstance, store: FabStore, now: number): Promise<{
  disk: { total: number; free: number }; month_bytes: number; month_bytes_out: number; mods: number; creators: number; open_reports: number;
}> {
  const disk = await store.disk();
  // uploads: the versions published in the last 30 days; downloads: what the download addresses sent (today and the 29 days before)
  const month_bytes = (db.prepare('SELECT COALESCE(SUM(size), 0) AS b FROM fab_versions WHERE created_at > ?').get(now - MONTH_MS) as { b: number }).b;
  const month_bytes_out = (db.prepare('SELECT COALESCE(SUM(bytes), 0) AS b FROM fab_traffic WHERE day >= ?').get(dayOf(now - 29 * DAY_MS)) as { b: number }).b;
  const live = db.prepare("SELECT COUNT(*) AS mods, COUNT(DISTINCT owner) AS creators FROM fab_mods WHERE state = 'live'").get() as { mods: number; creators: number };
  const open_reports = (db.prepare(`SELECT COUNT(*) AS n FROM fab_reports r JOIN fab_mods m ON m.id = r.mod_id
    WHERE r.handled_by IS NULL AND m.state != 'removed'`).get() as { n: number }).n;
  return { disk, month_bytes, month_bytes_out, mods: live.mods, creators: live.creators, open_reports };
}

// ---- The panel's mods view ----

export const MOD_STATES = ['live', 'taken_down', 'removed'] as const;
export interface AdminModRow {
  id: number; name: string; game: string; author: string; state: string; versions: number; open_reports: number;
  publishing: boolean; created_at: number;
}
export interface AdminModDetail {
  id: number; name: string; game: string; type: string; author: string; state: string; publishing: boolean;
  created_at: number; updated_at: number; screens: number;
  versions: { id: number; version: string; size: number; sha256: string; state: string; created_at: number }[];
  reports: { id: number; reason: string; text: string; reporter: string; created_at: number; handled_by: string | null; outcome: string | null }[];
}
const ADMIN_LIST_MAX = 200;

// Mods in any state, newest first: filtered by state and by a piece of the name (or the id), at most 200.
export function adminMods(db: DatabaseInstance, f: { state?: string; q?: string }): AdminModRow[] {
  const where: string[] = [];
  const args: Record<string, string | number> = {};
  if (f.state) { where.push('m.state = :state'); args.state = f.state; }
  const q = (f.q ?? '').trim().slice(0, 100);
  if (q) {
    // LIKE wildcards in the text are plain characters
    args.q = '%' + q.toLowerCase().replace(/[\\%_]/g, (c) => '\\' + c) + '%';
    if (/^\d{1,9}$/.test(q)) { where.push("(lower(m.name) LIKE :q ESCAPE '\\' OR m.id = :id)"); args.id = Number(q); }
    else where.push("lower(m.name) LIKE :q ESCAPE '\\'");
  }
  const rows = db.prepare(`SELECT m.id, m.name, m.game, m.state, m.created_at, ${TAG('u')} AS author, ${PUBLISHING('m.owner')} AS publishing,
      (SELECT COUNT(*) FROM fab_versions WHERE mod_id = m.id) AS versions,
      (SELECT COUNT(*) FROM fab_reports WHERE mod_id = m.id AND handled_by IS NULL) AS open_reports
    FROM fab_mods m LEFT JOIN users u ON u.uuid = m.owner
    ${where.length ? 'WHERE ' + where.join(' AND ') : ''}
    ORDER BY m.created_at DESC, m.id DESC LIMIT ${ADMIN_LIST_MAX}`).all(args) as any[];
  return rows.map((r) => ({
    id: r.id, name: r.name, game: r.game, author: r.author, state: r.state, versions: r.versions, open_reports: r.open_reports,
    publishing: r.publishing === 1, created_at: r.created_at,
  }));
}

// One mod for the panel: its versions and their files, how many screenshots, every report (handled ones too).
export function adminModDetail(db: DatabaseInstance, id: number): AdminModDetail | null {
  const m = db.prepare(`SELECT m.id, m.name, m.game, m.type, m.state, m.created_at, m.updated_at, ${TAG('u')} AS author,
      ${PUBLISHING('m.owner')} AS publishing, (SELECT COUNT(*) FROM fab_screens WHERE mod_id = m.id) AS screens
    FROM fab_mods m LEFT JOIN users u ON u.uuid = m.owner WHERE m.id = ?`).get(id) as any;
  if (!m) return null;
  const versions = db.prepare('SELECT id, version, size, sha256, state, created_at FROM fab_versions WHERE mod_id = ? ORDER BY created_at DESC, id DESC')
    .all(id) as AdminModDetail['versions'];
  const reports = db.prepare(`SELECT r.id, r.reason, r.text, ${TAG('ru')} AS reporter, r.created_at, r.handled_by, r.outcome
    FROM fab_reports r LEFT JOIN users ru ON ru.uuid = r.reporter WHERE r.mod_id = ? ORDER BY r.created_at DESC, r.id DESC`).all(id) as AdminModDetail['reports'];
  return {
    id: m.id, name: m.name, game: m.game, type: m.type, author: m.author, state: m.state, publishing: m.publishing === 1,
    created_at: m.created_at, updated_at: m.updated_at, screens: m.screens, versions, reports,
  };
}
