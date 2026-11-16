import type { DatabaseInstance } from '../db.js';

// The store's read side: the listing, search and a mod's page. Only 'live' mods are ever listed;
// owners appear as `display_name#discriminator`, never as a uuid.

export interface ModCard {
  id: number; name: string; author: string; game: string; type: string; short_desc: string;
  screen: string; rating: number; ratings: number; downloads: number; updated_at: number; version: string; size: number;
}
export type SortKey = 'popular' | 'rating' | 'new' | 'updated' | 'name';
export interface ListQuery { search?: string; game?: string; type?: string; sort: SortKey; page: number; perPage: number }
export interface ModVersionInfo { id: number; version: string; notes: string; created_at: number; size: number; sha256: string }
export type ModPage = ModCard & {
  long_desc: string; tags: string[]; side: string; requires: { id: number; name: string }[];
  screens: string[]; versions: ModVersionInfo[]; others: ModCard[];
};
export type TakenDownPage = { id: number; name: string; state: 'taken_down' };

export const MAX_PER_PAGE = 48;
const DAY_MS = 86_400_000;
const PRIOR_WEIGHT = 5;  // C
const PRIOR_MEAN = 3.5;  // m
const IMAGE_PATH = '/api/v1/fabricator/images/';

// Turn free text into a safe FTS5 MATCH expression: every word is a quoted term, the last also matches
// as a prefix. Returns null when nothing searchable is left.
export function ftsQuery(text: string): string | null {
  const words = (text.normalize('NFKC').match(/[\p{L}\p{N}]+/gu) ?? []).slice(0, 12);
  if (words.length === 0) return null;
  return words.map((w, i) => `"${w}"${i === words.length - 1 ? '*' : ''}`).join(' ');
}

const SELECT = `
  SELECT m.id, m.name, m.created_at, m.game, m.type, m.short_desc, m.updated_at,
    COALESCE(u.display_name || '#' || u.discriminator, 'unknown') AS author,
    (SELECT image_key FROM fab_screens WHERE mod_id = m.id ORDER BY pos LIMIT 1) AS image_key,
    (SELECT COUNT(*) FROM fab_ratings WHERE mod_id = m.id) AS ratings,
    (SELECT COALESCE(SUM(stars), 0) FROM fab_ratings WHERE mod_id = m.id) AS stars,
    (SELECT COUNT(*) FROM fab_downloads WHERE mod_id = m.id) AS downloads,
    (SELECT COUNT(*) FROM fab_downloads WHERE mod_id = m.id AND at > :since) AS recent,
    COALESCE((SELECT version FROM fab_versions WHERE mod_id = m.id AND state = 'live' ORDER BY created_at DESC, id DESC LIMIT 1), '') AS version,
    COALESCE((SELECT size FROM fab_versions WHERE mod_id = m.id AND state = 'live' ORDER BY created_at DESC, id DESC LIMIT 1), 0) AS size,
    COALESCE((SELECT MAX(created_at) FROM fab_versions WHERE mod_id = m.id AND state = 'live'), m.updated_at) AS latest
  FROM fab_mods m LEFT JOIN users u ON u.uuid = m.owner`;

interface CardRow {
  id: number; name: string; game: string; type: string; short_desc: string; updated_at: number; author: string;
  image_key: string | null; ratings: number; stars: number; downloads: number; recent: number; version: string; size: number; latest: number;
}

// The Bayesian score (PRIOR_WEIGHT, PRIOR_MEAN) only sorts (PAGE_IDS.rating); cards and pages show the plain mean.
const mean = (r: { ratings: number; stars: number }): number => (r.ratings > 0 ? Math.round((r.stars / r.ratings) * 10) / 10 : 0);

function card(r: CardRow): ModCard {
  return {
    id: r.id, name: r.name, author: r.author, game: r.game, type: r.type, short_desc: r.short_desc,
    screen: r.image_key ? IMAGE_PATH + r.image_key : '',
    rating: mean(r), ratings: r.ratings, downloads: r.downloads,
    updated_at: r.updated_at, version: r.version, size: r.size,
  };
}

// Phase one of a listing: the page's mod ids in order. Sorts that need counts get them from one GROUP BY
// pass over the (covering) index; name/new/updated never touch the download or rating tables.
const DL_AGG = `LEFT JOIN (SELECT mod_id, COUNT(*) AS downloads, COALESCE(SUM(at > :since), 0) AS recent
  FROM fab_downloads GROUP BY mod_id) a ON a.mod_id = m.id`;
const RATE_AGG = `LEFT JOIN (SELECT mod_id, COUNT(*) AS ratings, SUM(stars) AS stars FROM fab_ratings GROUP BY mod_id) a ON a.mod_id = m.id`;
const LATEST_AGG = `LEFT JOIN (SELECT mod_id, MAX(created_at) AS latest FROM fab_versions WHERE state = 'live' GROUP BY mod_id) a ON a.mod_id = m.id`;
const NAME = 'm.name COLLATE NOCASE, m.id';
const PAGE_IDS: Record<SortKey, { join: string; order: string }> = {
  popular: { join: DL_AGG, order: `COALESCE(a.recent, 0) DESC, COALESCE(a.downloads, 0) DESC, ${NAME}` },
  rating: {
    join: RATE_AGG,
    order: `((${PRIOR_WEIGHT} * ${PRIOR_MEAN} + COALESCE(a.stars, 0)) * 1.0 / (${PRIOR_WEIGHT} + COALESCE(a.ratings, 0))) DESC, COALESCE(a.ratings, 0) DESC, ${NAME}`,
  },
  new: { join: '', order: 'm.created_at DESC, m.id DESC' },
  updated: { join: LATEST_AGG, order: 'COALESCE(a.latest, m.updated_at) DESC, m.id DESC' },
  name: { join: '', order: NAME },
};

export function pageIdsSql(sort: SortKey, where: string): string {
  const p = PAGE_IDS[sort] ?? PAGE_IDS.popular;
  return `SELECT m.id FROM fab_mods m ${p.join} WHERE ${where} ORDER BY ${p.order} LIMIT :limit OFFSET :offset`;
}

export function listMods(db: DatabaseInstance, q: ListQuery, now: number): { mods: ModCard[]; total: number } {
  const where = ["m.state = 'live'"];
  const args: Record<string, string | number> = {};
  const match = q.search ? ftsQuery(q.search) : null;
  if (match) { where.push('m.id IN (SELECT rowid FROM fab_mods_fts WHERE fab_mods_fts MATCH :match)'); args.match = match; }
  if (q.game) { where.push('m.game = :game'); args.game = q.game; }
  if (q.type) { where.push('m.type = :type'); args.type = q.type; }
  const w = where.join(' AND ');
  const perPage = Math.min(MAX_PER_PAGE, Math.max(1, Math.floor(q.perPage) || 24));
  const page = Math.max(1, Math.floor(q.page) || 1);
  const total = (db.prepare(`SELECT COUNT(*) AS n FROM fab_mods m WHERE ${w}`).get(args) as { n: number }).n;
  const ids = (db.prepare(pageIdsSql(q.sort, w)).all({ ...args, since: now - 30 * DAY_MS, limit: perPage, offset: (page - 1) * perPage }) as { id: number }[])
    .map((r) => r.id);
  if (ids.length === 0) return { mods: [], total };
  // Phase two: card fields for just those ids (integers from the query above), put back in order.
  const rows = db.prepare(`${SELECT} WHERE m.id IN (${ids.join(',')})`).all({ since: now - 30 * DAY_MS }) as CardRow[];
  const byId = new Map(rows.map((r) => [r.id, r]));
  return { mods: ids.map((id) => card(byId.get(id) as CardRow)), total };
}

const parseList = (s: string): unknown[] => { try { const v = JSON.parse(s); return Array.isArray(v) ? v : []; } catch { return []; } };

export function modPage(db: DatabaseInstance, id: number, now: number = Date.now()): ModPage | TakenDownPage | null {
  const m = db.prepare('SELECT id, owner, name, state, long_desc, tags, side, requires FROM fab_mods WHERE id = ?').get(id) as
    { id: number; owner: string; name: string; state: string; long_desc: string; tags: string; side: string; requires: string } | undefined;
  if (!m || m.state === 'removed') return null;
  if (m.state === 'taken_down') return { id: m.id, name: m.name, state: 'taken_down' };
  const since = now - 30 * DAY_MS;
  const base = db.prepare(`${SELECT} WHERE m.id = :id`).get({ id, since }) as CardRow;
  const reqIds = parseList(m.requires).filter((x): x is number => Number.isSafeInteger(x));
  const requires = reqIds.flatMap((rid) => {
    const r = db.prepare("SELECT id, name FROM fab_mods WHERE id = ? AND state = 'live'").get(rid) as { id: number; name: string } | undefined;
    return r ? [r] : [];
  });
  const screens = (db.prepare('SELECT image_key FROM fab_screens WHERE mod_id = ? ORDER BY pos').all(id) as { image_key: string }[])
    .map((s) => IMAGE_PATH + s.image_key);
  const versions = db.prepare("SELECT id, version, notes, created_at, size, sha256 FROM fab_versions WHERE mod_id = ? AND state = 'live' ORDER BY created_at DESC, id DESC")
    .all(id) as ModVersionInfo[];
  const others = (db.prepare(`${SELECT} WHERE m.owner = :owner AND m.id != :id AND m.state = 'live' ORDER BY m.updated_at DESC, m.id DESC LIMIT 6`)
    .all({ owner: m.owner, id, since }) as CardRow[]).map(card);
  return { ...card(base), long_desc: m.long_desc, tags: parseList(m.tags).map(String), side: m.side, requires, screens, versions, others };
}
