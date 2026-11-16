import crypto from 'node:crypto';
import fs from 'node:fs';
import type { DatabaseInstance } from '../db.js';
import type { FabSettings } from './settings.js';
import type { FabStore } from './store.js';
import { checkModZip, checkScreenshot, checkText, type UploadMeta } from './checks.js';

// Upload sessions and publishing: the rules, with no HTTP (routes.ts maps them to answers).
const DAY_MS = 24 * 3600 * 1000;
const SHA = /^[0-9a-f]{64}$/;

// modcore's Side words (modcore.h): client, host (server is read as host), everything else everyone.
export const sideWord = (v: string | undefined): 'everyone' | 'host' | 'client' => {
  const low = (v ?? '').trim().toLowerCase();
  return low === 'client' ? 'client' : low === 'host' || low === 'server' ? 'host' : 'everyone';
};
export const MAX_SCREENS = 6;
const GONE = 'fabricator: upload or account gone';

export interface UploadRow {
  id: string; account: string; mod_id: number | null; meta: string; size: number; sha256: string;
  chunk_bytes: number; chunks_done: number; created_at: number; expires_at: number;
}

// One request at a time works on a session (chunk, screenshot, finish, cancel): the server is a single process.
const busy = new Set<string>();

export const chunkCount = (row: { size: number; chunk_bytes: number }): number => Math.ceil(row.size / row.chunk_bytes);

export function getUpload(db: DatabaseInstance, id: string): UploadRow | undefined {
  return db.prepare('SELECT * FROM fab_uploads WHERE id = ?').get(id) as UploadRow | undefined;
}

function creator(db: DatabaseInstance, account: string): { publishing: number; bytes_used: number } {
  return (db.prepare('SELECT publishing, bytes_used FROM fab_creators WHERE account = ?').get(account) as any)
    ?? { publishing: 1, bytes_used: 0 };
}

// Null when the account may start an upload of `size` bytes, else the reason shown to the creator.
export function canPublish(db: DatabaseInstance, s: FabSettings, account: string, size: number, now: number,
  disk: { total: number; free: number }): string | null {
  const user = db.prepare('SELECT created_at FROM users WHERE uuid = ?').get(account) as { created_at: number } | undefined;
  if (!user || now - user.created_at < s.minAccountAgeDays * DAY_MS) return 'account too new';
  const c = creator(db, account);
  if (!c.publishing) return 'publishing stopped';
  if (size > s.maxFileBytes) return 'file too big';
  // open sessions count: their files arrive later (chunks, then an assembled copy on the same disk)
  const openMine = (db.prepare('SELECT COALESCE(SUM(size), 0) AS b FROM fab_uploads WHERE account = ? AND expires_at >= ?')
    .get(account, now) as { b: number }).b;
  if (c.bytes_used + openMine + size > s.creatorQuotaBytes) return 'quota';
  const since = now - DAY_MS;
  const mine = (db.prepare(`SELECT COUNT(*) AS n FROM fab_versions v JOIN fab_mods m ON m.id = v.mod_id
    WHERE m.owner = ? AND v.created_at > ?`).get(account, since) as { n: number }).n;
  const open = (db.prepare('SELECT COUNT(*) AS n FROM fab_uploads WHERE account = ? AND expires_at >= ?')
    .get(account, now) as { n: number }).n;
  if (mine + open >= s.creatorUploadsPerDay) return 'daily limit';
  const all = db.prepare('SELECT COUNT(*) AS n, COALESCE(SUM(size), 0) AS bytes FROM fab_versions WHERE created_at > ?')
    .get(since) as { n: number; bytes: number };
  if (all.n >= s.globalUploadsPerDay || all.bytes + size > s.globalUploadBytesPerDay) return 'server daily limit';
  const openAll = (db.prepare('SELECT COALESCE(SUM(size), 0) AS b FROM fab_uploads WHERE expires_at >= ?').get(now) as { b: number }).b;
  if (disk.total <= 0 || (disk.total - disk.free + 2 * (openAll + size)) / disk.total > s.diskFullFraction) return 'server full';
  return null;
}

// The caller's own live mod, or undefined.
export function ownLiveMod(db: DatabaseInstance, account: string, modId: number): { id: number; game: string } | undefined {
  return db.prepare("SELECT id, game FROM fab_mods WHERE id = ? AND owner = ? AND state = 'live'").get(modId, account) as any;
}

// Names are unique per game ignoring case (every state holds its name); the mod being updated keeps its own.
function nameProblem(db: DatabaseInstance, account: string, game: string, name: string, exceptMod: number | null): string | null {
  const row = db.prepare('SELECT id, owner FROM fab_mods WHERE game = ? AND lower(name) = lower(?) AND id <> ?')
    .get(game, name, exceptMod ?? -1) as { id: number; owner: string } | undefined;
  if (!row) return null;
  return row.owner === account
    ? 'name taken: you already have a mod with this name (upload a new version of it)'
    : 'name taken: another creator uses this name for this game';
}

export function normalizeMeta(m: any): UploadMeta {
  const str = (v: unknown) => (typeof v === 'string' ? v : '');
  const tags = Array.isArray(m?.tags) ? m.tags.filter((t: unknown) => typeof t === 'string').map((t: string) => t.trim()).filter(Boolean) : [];
  const out: UploadMeta = {
    name: str(m?.name).trim(), game: str(m?.game), type: str(m?.type), version: str(m?.version).trim(),
    short_desc: str(m?.short_desc), long_desc: str(m?.long_desc), tags: [...new Set<string>(tags)], notes: str(m?.notes),
  };
  if (typeof m?.side === 'string') out.side = m.side.toLowerCase();
  if (Array.isArray(m?.requires)) out.requires = m.requires.filter((x: unknown) => Number.isInteger(x)).slice(0, 32);
  return out;
}

// What is wrong with a new session's request (400); ownership and limits are answered separately (403).
export function uploadProblems(db: DatabaseInstance, account: string, meta: UploadMeta, modId: number | null,
  size: unknown, sha256: unknown): string[] {
  const p = checkText(meta);
  if (!Number.isSafeInteger(size) || (size as number) < 1) p.push('size must be a positive whole number of bytes');
  if (typeof sha256 !== 'string' || !SHA.test(sha256)) p.push('sha256 must be 64 lowercase hex characters');
  const mod = modId !== null ? ownLiveMod(db, account, modId) : undefined;
  if (mod && meta.game !== mod.game) p.push(`game: a new version must be for the mod's game (${mod.game})`);
  const taken = p.length === 0 ? nameProblem(db, account, mod?.game ?? meta.game, meta.name, modId) : null;
  if (taken) p.push(taken);
  return p;
}

export function startUpload(db: DatabaseInstance, account: string, meta: UploadMeta, modId: number | null,
  size: number, sha256: string, now: number, s: FabSettings): { id: string; chunkBytes: number; chunks: number } {
  if (modId !== null && !ownLiveMod(db, account, modId)) throw new Error('not your mod');
  const chunkBytes = Math.max(1, Math.floor(s.chunkBytes));
  const id = crypto.randomBytes(18).toString('base64url');
  db.prepare(`INSERT INTO fab_uploads (id, account, mod_id, meta, size, sha256, chunk_bytes, chunks_done, created_at, expires_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?)`)
    .run(id, account, modId, JSON.stringify(normalizeMeta(meta)), size, sha256, chunkBytes, now, now + s.sessionTtlMs);
  return { id, chunkBytes, chunks: Math.ceil(size / chunkBytes) };
}

export type ChunkResult = 'ok' | 'repeat' | 'order' | 'hash' | 'size' | 'busy' | 'gone';

// Chunks go in order: only n === chunks_done is taken; the last one sent again with the same hash is ignored.
export async function putChunk(db: DatabaseInstance, store: FabStore, id: string, n: number, data: Buffer,
  sha256: string): Promise<ChunkResult> {
  if (busy.has(id)) return 'busy';
  busy.add(id);
  try {
    const row = getUpload(db, id);
    if (!row) return 'gone';
    const hash = crypto.createHash('sha256').update(data).digest('hex');
    if (hash !== sha256) return 'hash';
    if (n === row.chunks_done - 1) {
      const kept = await fs.promises.readFile(store.chunkPath(id, n)).catch(() => null);
      return kept && crypto.createHash('sha256').update(kept).digest('hex') === sha256 ? 'repeat' : 'order';
    }
    const chunks = chunkCount(row);
    if (n !== row.chunks_done || n >= chunks) return 'order';
    const want = n < chunks - 1 ? row.chunk_bytes : row.size - row.chunk_bytes * (chunks - 1);
    if (data.length !== want) return 'size';
    await store.writeChunk(id, n, data);
    db.prepare('UPDATE fab_uploads SET chunks_done = ? WHERE id = ? AND chunks_done = ?').run(n + 1, id, n);
    return 'ok';
  } finally {
    busy.delete(id);
  }
}

function dropImageIfUnused(db: DatabaseInstance, store: FabStore, key: string): void {
  const used = db.prepare('SELECT 1 FROM fab_screens WHERE image_key = ? UNION ALL SELECT 1 FROM fab_upload_screens WHERE image_key = ? LIMIT 1')
    .get(key, key);
  if (!used) store.remove(key);
}

export function dropFileIfUnused(db: DatabaseInstance, store: FabStore, key: string): void {
  if (!db.prepare("SELECT 1 FROM fab_versions WHERE file_key = ? AND state = 'live' LIMIT 1").get(key)) store.remove(key);
}

// Null when the screenshot is kept at `pos` (0-5), else the problem.
export async function putScreen(db: DatabaseInstance, store: FabStore, id: string, pos: number, bytes: Buffer): Promise<string | null> {
  if (!Number.isInteger(pos) || pos < 0 || pos >= MAX_SCREENS) return `screenshot position must be 0 to ${MAX_SCREENS - 1}`;
  const shot = await checkScreenshot(bytes);
  if (!shot.ok) return shot.problem;
  if (!getUpload(db, id)) return 'upload not found';
  const key = store.saveImage(shot.png);
  const old = db.prepare('SELECT image_key FROM fab_upload_screens WHERE upload_id = ? AND pos = ?').get(id, pos) as { image_key: string } | undefined;
  db.prepare(`INSERT INTO fab_upload_screens (upload_id, pos, image_key, width, height) VALUES (?, ?, ?, ?, ?)
    ON CONFLICT(upload_id, pos) DO UPDATE SET image_key = excluded.image_key, width = excluded.width, height = excluded.height`)
    .run(id, pos, key, shot.width, shot.height);
  if (old && old.image_key !== key) dropImageIfUnused(db, store, old.image_key);
  return null;
}

// Ends a session: its row, its chunks, its staged file and screenshots nothing else uses. False while it is busy.
export function cancelUpload(db: DatabaseInstance, store: FabStore, id: string): boolean {
  if (busy.has(id)) return false;
  const keys = (db.prepare('SELECT image_key FROM fab_upload_screens WHERE upload_id = ?').all(id) as { image_key: string }[]).map((r) => r.image_key);
  db.prepare('DELETE FROM fab_upload_screens WHERE upload_id = ?').run(id);
  db.prepare('DELETE FROM fab_uploads WHERE id = ?').run(id);
  store.dropUpload(id);
  for (const k of new Set(keys)) dropImageIfUnused(db, store, k);
  return true;
}

export function expireUploads(db: DatabaseInstance, store: FabStore, now: number): number {
  const ids = (db.prepare('SELECT id FROM fab_uploads WHERE expires_at < ?').all(now) as { id: string }[]).map((r) => r.id);
  let n = 0;
  for (const id of ids) {
    if (cancelUpload(db, store, id)) n++;
  }
  return n;
}

const slugify = (name: string): string =>
  name.normalize('NFKD').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 60) || 'mod';

type FinishResult = { ok: true; modId: number; versionId: number } | { ok: false; problems: string[] };

export async function finishUpload(db: DatabaseInstance, store: FabStore, s: FabSettings, uploadId: string,
  account: string, now: number): Promise<FinishResult> {
  const row = getUpload(db, uploadId);
  if (!row || row.account !== account) return { ok: false, problems: ['upload not found'] };
  if (row.expires_at < now) return { ok: false, problems: ['upload expired'] };
  if (busy.has(uploadId)) return { ok: false, problems: ['upload busy: try again'] };
  busy.add(uploadId);
  try {
    return await finishLocked(db, store, s, row, now);
  } finally {
    busy.delete(uploadId);
  }
}

async function finishLocked(db: DatabaseInstance, store: FabStore, s: FabSettings, row: UploadRow, now: number): Promise<FinishResult> {
  const chunks = chunkCount(row);
  if (row.chunks_done < chunks) return { ok: false, problems: [`only ${row.chunks_done} of ${chunks} chunks are uploaded`] };
  const m = normalizeMeta(JSON.parse(row.meta));
  const problems = checkText(m);
  if (!creator(db, row.account).publishing) problems.push('publishing stopped');
  const mod = row.mod_id !== null ? ownLiveMod(db, row.account, row.mod_id) : undefined;
  if (row.mod_id !== null && !mod) problems.push('not your mod: a new version needs your own live mod');
  const game = mod?.game ?? m.game;   // a new version stays in its mod's game
  if (m.game !== game) problems.push(`game: a new version must be for the mod's game (${game})`);
  const c = creator(db, row.account);
  const openOther = (db.prepare('SELECT COALESCE(SUM(size), 0) AS b FROM fab_uploads WHERE account = ? AND expires_at >= ? AND id <> ?')
    .get(row.account, now, row.id) as { b: number }).b;
  if (c.bytes_used + openOther + row.size > s.creatorQuotaBytes) problems.push('quota: this upload would go over your storage quota');
  const disk = await store.disk();   // the chunks are already on disk; assembling adds one more copy
  if (disk.total <= 0 || (disk.total - disk.free + row.size) / disk.total > s.diskFullFraction) problems.push('server full: try again later');
  const shots = db.prepare('SELECT pos, image_key, width, height FROM fab_upload_screens WHERE upload_id = ? ORDER BY pos')
    .all(row.id) as { pos: number; image_key: string; width: number; height: number }[];
  const kept = row.mod_id !== null && shots.length === 0
    ? (db.prepare('SELECT COUNT(*) AS n FROM fab_screens WHERE mod_id = ?').get(row.mod_id) as { n: number }).n : 0;
  if (shots.length === 0 && kept === 0) problems.push('at least one screenshot is needed');
  const taken = m.name ? nameProblem(db, row.account, game, m.name, row.mod_id) : null;
  if (taken) problems.push(taken);

  if (problems.some((p) => p.startsWith('quota') || p.startsWith('server full'))) return { ok: false, problems };
  let file: string;
  try {
    const a = await store.assemble(row.id, chunks);
    file = a.file;
    if (a.size !== row.size || a.sha256 !== row.sha256) problems.push('the file\'s size or SHA-256 hash does not match what was announced');
  } catch {
    problems.push('the uploaded chunks are missing or damaged: start a new upload');
    return { ok: false, problems };
  }
  const zip = problems.some((p) => p.includes('hash')) ? null : await checkModZip(file, game, s);
  if (zip) problems.push(...zip.problems);
  if (problems.length > 0 || !zip) {
    await fs.promises.rm(file, { force: true, maxRetries: 5, retryDelay: 50 }).catch(() => {});
    return { ok: false, problems };
  }

  const key = store.commit(file, row.sha256);
  const dropFiles: string[] = [];
  const dropImages: string[] = [];
  let modId = row.mod_id;
  let versionId = 0;
  try {
    db.transaction(() => {
      // The account may have been deleted while the file was assembled and checked: write nothing then.
      if (!getUpload(db, row.id) || !db.prepare('SELECT 1 FROM users WHERE uuid = ?').get(row.account)) throw new Error(GONE);
      const self = modId ?? -1;
      const reqs = new Set<number>();
      for (const name of zip.modIni?.requires ?? []) {
        const r = db.prepare("SELECT id FROM fab_mods WHERE game = ? AND lower(name) = lower(?) AND state = 'live' AND id <> ?")
          .get(game, name, self) as { id: number } | undefined;
        if (r) reqs.add(r.id);
      }
      for (const id of m.requires ?? []) {
        if (id !== self && db.prepare("SELECT 1 FROM fab_mods WHERE id = ? AND game = ? AND state = 'live'").get(id, game)) reqs.add(id);
      }
      const side = sideWord(m.side ?? zip.modIni?.side);
      const fields = [slugify(m.name), m.name, m.type, m.short_desc, m.long_desc, JSON.stringify(m.tags), side, JSON.stringify([...reqs])];
      if (modId === null) {
        modId = Number(db.prepare(`INSERT INTO fab_mods (slug, name, type, short_desc, long_desc, tags, side, requires, owner, game, state, created_at, updated_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'live', ?, ?)`).run(...fields, row.account, game, now, now).lastInsertRowid);
      } else {
        db.prepare(`UPDATE fab_mods SET slug = ?, name = ?, type = ?, short_desc = ?, long_desc = ?, tags = ?, side = ?, requires = ?, updated_at = ?
          WHERE id = ?`).run(...fields, now, modId);
      }
      const u = db.prepare('SELECT display_name, discriminator FROM users WHERE uuid = ?').get(row.account) as { display_name: string; discriminator: string } | undefined;
      db.prepare('DELETE FROM fab_mods_fts WHERE rowid = ?').run(modId);
      db.prepare('INSERT INTO fab_mods_fts (rowid, name, short_desc, long_desc, tags, author) VALUES (?, ?, ?, ?, ?, ?)')
        .run(modId, m.name, m.short_desc, m.long_desc, m.tags.join(' '), u ? `${u.display_name}#${u.discriminator}` : '');
      versionId = Number(db.prepare(`INSERT INTO fab_versions (mod_id, version, notes, size, sha256, file_key, state, created_at)
        VALUES (?, ?, ?, ?, ?, ?, 'live', ?)`).run(modId, m.version, m.notes ?? '', row.size, row.sha256, key, now).lastInsertRowid);
      if (shots.length > 0) {
        for (const r of db.prepare('SELECT image_key FROM fab_screens WHERE mod_id = ?').all(modId) as { image_key: string }[]) dropImages.push(r.image_key);
        db.prepare('DELETE FROM fab_screens WHERE mod_id = ?').run(modId);
        shots.forEach((sh, i) => db.prepare('INSERT INTO fab_screens (mod_id, pos, image_key, width, height) VALUES (?, ?, ?, ?, ?)')
          .run(modId, i, sh.image_key, sh.width, sh.height));
      }
      db.prepare(`INSERT INTO fab_creators (account, publishing, bytes_used) VALUES (?, 1, ?)
        ON CONFLICT(account) DO UPDATE SET bytes_used = bytes_used + excluded.bytes_used`).run(row.account, row.size);
      const live = db.prepare("SELECT id, size, file_key FROM fab_versions WHERE mod_id = ? AND state = 'live' ORDER BY created_at DESC, id DESC")
        .all(modId) as { id: number; size: number; file_key: string }[];
      for (const old of live.slice(Math.max(1, s.keepVersions))) {
        db.prepare("UPDATE fab_versions SET state = 'files_deleted' WHERE id = ?").run(old.id);
        db.prepare('UPDATE fab_creators SET bytes_used = MAX(0, bytes_used - ?) WHERE account = ?').run(old.size, row.account);
        dropFiles.push(old.file_key);
      }
      db.prepare('DELETE FROM fab_upload_screens WHERE upload_id = ?').run(row.id);
      db.prepare('DELETE FROM fab_uploads WHERE id = ?').run(row.id);
    })();
  } catch (e) {
    dropFileIfUnused(db, store, key);
    if ((e as Error)?.message === GONE) return { ok: false, problems: ['upload not found'] };
    if ((e as any)?.code === 'SQLITE_CONSTRAINT_UNIQUE') return { ok: false, problems: ['name taken: another mod for this game has this name'] };
    throw e;
  }
  store.dropUpload(row.id);
  for (const k of new Set(dropFiles)) dropFileIfUnused(db, store, k);
  for (const k of new Set(dropImages)) dropImageIfUnused(db, store, k);
  return { ok: true, modId: modId as number, versionId };
}

// ---- Creator pages, owner removal, deleted accounts ----

export interface CreatorMod {
  id: number; name: string; game: string; state: string;
  versions: { id: number; version: string; created_at: number; size: number; state: string }[];
  downloads: number; rating: number; ratings: number; reports_open: number;
}

// The account's mods in every state (taken-down and removed ones too, so the creator sees what happened), newest first.
export function creatorMods(db: DatabaseInstance, account: string): CreatorMod[] {
  const mods = db.prepare(`SELECT m.id, m.name, m.game, m.state,
      (SELECT COUNT(*) FROM fab_downloads WHERE mod_id = m.id) AS downloads,
      (SELECT COUNT(*) FROM fab_ratings WHERE mod_id = m.id) AS ratings,
      (SELECT COALESCE(SUM(stars), 0) FROM fab_ratings WHERE mod_id = m.id) AS stars,
      (SELECT COUNT(*) FROM fab_reports WHERE mod_id = m.id AND handled_by IS NULL) AS reports_open
    FROM fab_mods m WHERE m.owner = ? ORDER BY m.created_at DESC, m.id DESC`).all(account) as any[];
  const versions = db.prepare('SELECT id, version, created_at, size, state FROM fab_versions WHERE mod_id = ? ORDER BY created_at DESC, id DESC');
  return mods.map((m) => ({
    id: m.id, name: m.name, game: m.game, state: m.state,
    versions: versions.all(m.id) as CreatorMod['versions'],
    downloads: m.downloads, ratings: m.ratings,
    rating: m.ratings > 0 ? Math.round((m.stars / m.ratings) * 100) / 100 : 0,
    reports_open: m.reports_open,
  }));
}

// Marks the mod removed in the database (versions' files given up, quota given back, screenshots and search entry
// dropped) and returns the file and image keys to delete once the transaction is over.
function removeModRows(db: DatabaseInstance, modId: number, account: string): { files: string[]; images: string[] } {
  const vs = db.prepare("SELECT id, size, file_key FROM fab_versions WHERE mod_id = ? AND state = 'live'").all(modId) as { id: number; size: number; file_key: string }[];
  for (const v of vs) {
    db.prepare("UPDATE fab_versions SET state = 'files_deleted' WHERE id = ?").run(v.id);
    db.prepare('UPDATE fab_creators SET bytes_used = MAX(0, bytes_used - ?) WHERE account = ?').run(v.size, account);
  }
  const images = (db.prepare('SELECT image_key FROM fab_screens WHERE mod_id = ?').all(modId) as { image_key: string }[]).map((r) => r.image_key);
  db.prepare('DELETE FROM fab_screens WHERE mod_id = ?').run(modId);
  db.prepare('DELETE FROM fab_mods_fts WHERE rowid = ?').run(modId);
  db.prepare("UPDATE fab_mods SET state = 'removed' WHERE id = ?").run(modId);
  // nothing is left to moderate: its open reports are closed
  db.prepare("UPDATE fab_reports SET handled_by = 'owner', outcome = 'removed' WHERE mod_id = ? AND handled_by IS NULL").run(modId);
  return { files: vs.map((v) => v.file_key), images };
}

function dropKeys(db: DatabaseInstance, store: FabStore, files: Iterable<string>, images: Iterable<string>): void {
  for (const k of new Set(files)) dropFileIfUnused(db, store, k);
  for (const k of new Set(images)) dropImageIfUnused(db, store, k);
}

// The owner removes their own mod (live or taken down): false when it is not theirs or already removed.
export function removeOwnMod(db: DatabaseInstance, store: FabStore, account: string, modId: number): boolean {
  const gone = db.transaction(() => {
    if (!db.prepare("SELECT 1 FROM fab_mods WHERE id = ? AND owner = ? AND state != 'removed'").get(modId, account)) return null;
    return removeModRows(db, modId, account);
  })();
  if (!gone) return false;
  dropKeys(db, store, gone.files, gone.images);
  return true;
}

// The database part of deleting an account, for the account deletion's own transaction. Returns what to do once that
// transaction has committed (files are not transactional).
export function forgetFabricatorRows(db: DatabaseInstance, account: string): (store: FabStore) => void {
  const files: string[] = [];
  const images: string[] = [];
  const dropIds: string[] = [];
  for (const m of db.prepare("SELECT id FROM fab_mods WHERE owner = ? AND state != 'removed'").all(account) as { id: number }[]) {
    const g = removeModRows(db, m.id, account);
    files.push(...g.files);
    images.push(...g.images);
  }
  for (const u of db.prepare('SELECT id FROM fab_uploads WHERE account = ?').all(account) as { id: string }[]) {
    images.push(...(db.prepare('SELECT image_key FROM fab_upload_screens WHERE upload_id = ?').all(u.id) as { image_key: string }[]).map((r) => r.image_key));
    db.prepare('DELETE FROM fab_upload_screens WHERE upload_id = ?').run(u.id);
    db.prepare('DELETE FROM fab_uploads WHERE id = ?').run(u.id);
    dropIds.push(u.id);
  }
  db.prepare('DELETE FROM fab_ratings WHERE account = ?').run(account);
  db.prepare('DELETE FROM fab_reports WHERE reporter = ?').run(account);
  db.prepare('DELETE FROM fab_library WHERE account = ?').run(account);
  db.prepare('DELETE FROM fab_downloads WHERE account = ?').run(account);
  db.prepare('DELETE FROM fab_creators WHERE account = ?').run(account);
  return (store) => {
    for (const id of dropIds) store.dropUpload(id);
    dropKeys(db, store, files, images);
  };
}

// Everything the Fabricator holds for an account, on its own (the account route uses forgetFabricatorRows inside its transaction).
export function forgetFabricatorAccount(db: DatabaseInstance, store: FabStore, account: string): void {
  db.transaction(() => forgetFabricatorRows(db, account))()(store);
}

export function creatorLimits(db: DatabaseInstance, s: FabSettings, account: string, now: number, disk: { total: number; free: number }) {
  const since = now - DAY_MS;
  const done = (db.prepare(`SELECT COUNT(*) AS n FROM fab_versions v JOIN fab_mods m ON m.id = v.mod_id
    WHERE m.owner = ? AND v.created_at > ?`).get(account, since) as { n: number }).n;
  const open = (db.prepare('SELECT COUNT(*) AS n FROM fab_uploads WHERE account = ? AND expires_at >= ?').get(account, now) as { n: number }).n;
  return {
    can_publish: canPublish(db, s, account, 1, now, disk),
    bytes_used: creator(db, account).bytes_used,
    quota: s.creatorQuotaBytes,
    uploads_today: done + open,
    max_uploads_per_day: s.creatorUploadsPerDay,
    max_file_bytes: s.maxFileBytes,
    chunk_bytes: Math.max(1, Math.floor(s.chunkBytes)),
  };
}
