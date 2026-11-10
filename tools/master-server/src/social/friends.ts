import { DatabaseInstance } from '../db.js';
import { FriendRef } from './types.js';

// Friends on the `friends` table (social design step 2). Rows:
//   request     from -> to   'pending'
//   friendship  a -> b and b -> a 'accepted'
//   block       me -> other  'blocked' (the other side is never told)
export const MAX_FRIENDS = 100;
export const MAX_PENDING_OUT = 20;

export type RequestResult = 'pending' | 'accepted' | 'not_found' | 'self' | 'limit';

const status = (db: DatabaseInstance, from: string, to: string): string | undefined =>
  (db.prepare('SELECT status FROM friends WHERE user_uuid = ? AND friend_uuid = ?').get(from, to) as
    { status: string } | undefined)?.status;

const countFriends = (db: DatabaseInstance, uuid: string): number =>
  (db.prepare("SELECT COUNT(*) AS c FROM friends WHERE user_uuid = ? AND status = 'accepted'").get(uuid) as { c: number }).c;

const setRow = (db: DatabaseInstance, from: string, to: string, s: string) =>
  db.prepare(`INSERT INTO friends (user_uuid, friend_uuid, status, updated_at) VALUES (?, ?, ?, ?)
              ON CONFLICT(user_uuid, friend_uuid) DO UPDATE SET status = excluded.status, updated_at = excluded.updated_at`)
    .run(from, to, s, Date.now());

const deleteBoth = (db: DatabaseInstance, a: string, b: string) =>
  db.prepare('DELETE FROM friends WHERE (user_uuid = ? AND friend_uuid = ?) OR (user_uuid = ? AND friend_uuid = ?)')
    .run(a, b, b, a);

// `Name#1234`: the name in any case, the discriminator exactly.
export function findByTag(db: DatabaseInstance, tag: string): { uuid: string } | null {
  const m = /^(.+)#(\d{4})$/.exec(typeof tag === 'string' ? tag.trim() : '');
  if (!m) return null;
  const row = db.prepare('SELECT uuid FROM users WHERE display_name = ? COLLATE NOCASE AND discriminator = ?')
    .get(m[1].trim(), m[2]) as { uuid: string } | undefined;
  return row ?? null;
}

export function request(db: DatabaseInstance, from: string, to: string): RequestResult {
  if (from === to) return 'self';
  return db.transaction((): RequestResult => {
    if (status(db, to, from) === 'blocked') return 'not_found';
    if (status(db, from, to) === 'accepted') return 'accepted';
    if (status(db, to, from) === 'pending') {   // they asked first: this is their answer
      if (countFriends(db, from) >= MAX_FRIENDS || countFriends(db, to) >= MAX_FRIENDS) return 'limit';
      setRow(db, from, to, 'accepted');
      setRow(db, to, from, 'accepted');
      return 'accepted';
    }
    if (status(db, from, to) === 'pending') return 'pending';
    const pendingOut = (db.prepare("SELECT COUNT(*) AS c FROM friends WHERE user_uuid = ? AND status = 'pending'")
      .get(from) as { c: number }).c;
    if (pendingOut >= MAX_PENDING_OUT) return 'limit';
    if (countFriends(db, from) >= MAX_FRIENDS || countFriends(db, to) >= MAX_FRIENDS) return 'limit';
    setRow(db, from, to, 'pending');   // replaces a block of ours: asking lifts it
    return 'pending';
  })();
}

export function accept(db: DatabaseInstance, me: string, other: string): boolean {
  return db.transaction((): boolean => {
    if (status(db, other, me) !== 'pending') return false;
    if (countFriends(db, me) >= MAX_FRIENDS || countFriends(db, other) >= MAX_FRIENDS) return false;
    setRow(db, other, me, 'accepted');
    setRow(db, me, other, 'accepted');
    return true;
  })();
}

export function decline(db: DatabaseInstance, me: string, other: string): void {
  db.prepare("DELETE FROM friends WHERE user_uuid = ? AND friend_uuid = ? AND status = 'pending'").run(other, me);
}

export function block(db: DatabaseInstance, me: string, other: string): void {
  db.transaction(() => {
    deleteBoth(db, me, other);
    setRow(db, me, other, 'blocked');
  })();
}

// Removes a friendship or requests either way; a block the other placed on me stays.
export function remove(db: DatabaseInstance, me: string, other: string): void {
  db.transaction(() => {
    const theirBlock = status(db, other, me) === 'blocked';
    deleteBoth(db, me, other);
    if (theirBlock) setRow(db, other, me, 'blocked');
  })();
}

export function listFor(db: DatabaseInstance, me: string): { friends: FriendRef[]; incoming: FriendRef[]; outgoing: FriendRef[] } {
  const refs = (sql: string): FriendRef[] =>
    (db.prepare(sql).all(me) as { uuid: string; display_name: string; discriminator: string }[])
      .map((r) => ({ uuid: r.uuid, tag: `${r.display_name}#${r.discriminator}` }));
  return {
    friends: refs(`SELECT u.uuid, u.display_name, u.discriminator FROM friends f JOIN users u ON u.uuid = f.friend_uuid
                   WHERE f.user_uuid = ? AND f.status = 'accepted' ORDER BY u.display_name COLLATE NOCASE`),
    incoming: refs(`SELECT u.uuid, u.display_name, u.discriminator FROM friends f JOIN users u ON u.uuid = f.user_uuid
                    WHERE f.friend_uuid = ? AND f.status = 'pending' ORDER BY f.updated_at`),
    outgoing: refs(`SELECT u.uuid, u.display_name, u.discriminator FROM friends f JOIN users u ON u.uuid = f.friend_uuid
                    WHERE f.user_uuid = ? AND f.status = 'pending' ORDER BY f.updated_at`),
  };
}
