import { DatabaseInstance } from '../db.js';
import { MAX_TAG_LOOKUP } from './tags.js';

// Recent players (0.6.5): the COGBox players you were in a match with, so you can add them as friends from the launcher's
// or the game's Social page. The game reports the XUIDs on its scoreboard that have a COGBox account (the same players
// it shows tags for, GitHub #353); one row per pair, the newest game and time win.
export const RECENT_DAYS = 14;   // older meetings drop out of the list
export const RECENT_MAX = 30;    // the list's length

export interface RecentPlayer {
  uuid: string;
  display_name: string;
  discriminator: string;
  icon: string;
  title_id: string;
  seen_at: number;
  relation: 'friend' | 'sent' | 'incoming' | 'none';
}

export function recordRecent(db: DatabaseInstance, owner: string, titleId: unknown, xuids: unknown[], now: number): void {
  if (typeof titleId !== 'string' || !/^[0-9A-Fa-f]{8}$/.test(titleId)) return;
  const wanted = [...new Set(xuids.filter((x): x is string => typeof x === 'string' && /^[0-9]{1,20}$/.test(x)))]
    .slice(0, MAX_TAG_LOOKUP);
  if (!wanted.length) return;
  const others = db.prepare(`SELECT uuid FROM users WHERE xuid IN (${wanted.map(() => '?').join(',')}) AND uuid <> ?`)
    .all(...wanted, owner) as { uuid: string }[];
  const put = db.prepare(`INSERT INTO recent_players (owner_uuid, other_uuid, title_id, seen_at) VALUES (?, ?, ?, ?)
                          ON CONFLICT(owner_uuid, other_uuid) DO UPDATE SET title_id = excluded.title_id, seen_at = excluded.seen_at`);
  db.transaction(() => { for (const o of others) put.run(owner, o.uuid, titleId.toUpperCase(), now); })();
}

export function recentFor(db: DatabaseInstance, owner: string, now: number): RecentPlayer[] {
  const since = now - RECENT_DAYS * 24 * 3600 * 1000;
  const rows = db.prepare(`
    SELECT u.uuid, u.display_name, u.discriminator, u.icon, r.title_id, r.seen_at,
           (SELECT status FROM friends WHERE user_uuid = r.owner_uuid AND friend_uuid = r.other_uuid) AS mine,
           (SELECT status FROM friends WHERE user_uuid = r.other_uuid AND friend_uuid = r.owner_uuid) AS theirs
    FROM recent_players r JOIN users u ON u.uuid = r.other_uuid
    WHERE r.owner_uuid = ? AND r.seen_at >= ?
    ORDER BY r.seen_at DESC, u.display_name COLLATE NOCASE`).all(owner, since) as
    (Omit<RecentPlayer, 'relation'> & { mine: string | null; theirs: string | null })[];
  return rows
    .filter((r) => r.mine !== 'blocked' && r.theirs !== 'blocked')
    .slice(0, RECENT_MAX)
    .map(({ mine, theirs, ...p }) => ({
      ...p,
      relation: mine === 'accepted' ? 'friend' : mine === 'pending' ? 'sent' : theirs === 'pending' ? 'incoming' : 'none',
    }));
}
