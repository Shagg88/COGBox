import { DatabaseInstance } from '../db.js';

// GitHub #353: a player's COGBox tag (name#discriminator) by the XUID the games know them by, so the game can show
// it after each name in a match (lobby, scoreboard). Tags are public (friend requests are sent by tag).
export const MAX_TAG_LOOKUP = 32;   // a match has at most 10 players; split screen and changes over a session stay well under

export interface PlayerTag { xuid: string; display_name: string; discriminator: string }

export function tagsForXuids(db: DatabaseInstance, xuids: unknown[]): PlayerTag[] {
  const wanted = [...new Set(xuids.filter((x): x is string => typeof x === 'string' && /^[0-9]{1,20}$/.test(x)))]
    .slice(0, MAX_TAG_LOOKUP);
  if (!wanted.length) return [];
  const rows = db.prepare(`SELECT xuid, display_name, discriminator FROM users WHERE xuid IN (${wanted.map(() => '?').join(',')})`)
    .all(...wanted) as PlayerTag[];
  const byXuid = new Map(rows.map((r) => [r.xuid, r]));
  return wanted.flatMap((x) => (byXuid.has(x) ? [byXuid.get(x)!] : []));
}
