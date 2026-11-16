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

const bayes = (r: { ratings: number; stars: number }): number => (PRIOR_WEIGHT * PRIOR_MEAN + r.stars) / (PRIOR_WEIGHT + r.ratings);

function card(r: CardRow): ModCard {
  return {
    id: r.id, name: r.name, author: r.author, game: r.game, type: r.type, short_desc: r.short_desc,
    screen: r.image_key ? IMAGE_PATH + r.image_key : '',
    rating: Math.round(bayes(r) * 100) / 100, ratings: r.ratings, downloads: r.downloads,
    updated_at: r.updated_at, version: r.version, size: r.size,
  };
}

const ORDER: Record<SortKey, string> = {
  popular: 'recent DESC, downloads DESC, name COLLATE NOCASE, id',
  rating: `((${PRIOR_WEIGHT} * ${PRIOR_MEAN} + stars) * 1.0 / (${PRIOR_WEIGHT} + ratings)) DESC, ratings DESC, name COLLATE NOCASE, id`,
  new: 'created_at DESC, id DESC',
  updated: 'latest DESC, id DESC',
  name: 'name COLLATE NOCASE, id',
};

export function listMods(db: DatabaseInstance, q: ListQuery, now: number): { mods: ModCard[]; total: number } {
  const where = ["m.state = 'live'"];
  const args: Record<string, string | number> = { since: now - 30 * DAY_MS };
  const match = q.search ? ftsQuery(q.search) : null;
  if (match) { where.push('m.id IN (SELECT rowid FROM fab_mods_fts WHERE fab_mods_fts MATCH :match)'); args.match = match; }
  if (q.game) { where.push('m.game = :game'); args.game = q.game; }
  if (q.type) { where.push('m.type = :type'); args.type = q.type; }
  const w = where.join(' AND ');
  const perPage = Math.min(MAX_PER_PAGE, Math.max(1, Math.floor(q.perPage) || 24));
  const page = Math.max(1, Math.floor(q.page) || 1);
  const order = ORDER[q.sort] ?? ORDER.popular;
  const total = (db.prepare(`SELECT COUNT(*) AS n FROM fab_mods m WHERE ${w}`).get(
    Object.fromEntries(Object.entries(args).filter(([k]) => k !== 'since'))) as { n: number }).n;
  const rows = db.prepare(`SELECT * FROM (${SELECT} WHERE ${w}) ORDER BY ${order} LIMIT :limit OFFSET :offset`)
    .all({ ...args, limit: perPage, offset: (page - 1) * perPage }) as CardRow[];
  return { mods: rows.map(card), total };
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
