import { describe, it, expect, beforeEach } from 'vitest';
import { initDb, DatabaseInstance } from '../src/db.js';
import { accept, block, decline, findByTag, listFor, remove, request } from '../src/social/friends.js';

// Friends rules on an in-memory database (social design step 2).
describe('friends', () => {
  let db: DatabaseInstance;
  let n = 0;
  const user = (name: string, disc: string): string => {
    const uuid = `u-${++n}`;
    db.prepare(`INSERT INTO users (uuid, display_name, discriminator, xuid, token_hash, created_at, last_seen)
                VALUES (?, ?, ?, ?, 'h', 0, 0)`).run(uuid, name, disc, String(1000 + n));
    return uuid;
  };
  const rows = () => db.prepare('SELECT COUNT(*) AS c FROM friends').get() as { c: number };
  const uuids = (refs: { uuid: string }[]) => refs.map((r) => r.uuid);

  beforeEach(() => { db = initDb(':memory:'); });

  it('request then accept makes both friends', () => {
    const a = user('Marcus', '0001'); const b = user('Dom', '0002');
    expect(request(db, a, b)).toBe('pending');
    expect(uuids(listFor(db, b).incoming)).toEqual([a]);
    expect(uuids(listFor(db, a).outgoing)).toEqual([b]);
    expect(accept(db, b, a)).toBe(true);
    expect(uuids(listFor(db, a).friends)).toEqual([b]);
    expect(uuids(listFor(db, b).friends)).toEqual([a]);
    expect(listFor(db, a).friends[0].tag).toBe('Dom#0002');
  });

  it('crossed requests become one friendship', () => {
    const a = user('Marcus', '0001'); const b = user('Dom', '0002');
    expect(request(db, a, b)).toBe('pending');
    expect(request(db, b, a)).toBe('accepted');
    expect(uuids(listFor(db, a).friends)).toEqual([b]);
    expect(listFor(db, a).incoming).toEqual([]);
    expect(listFor(db, a).outgoing).toEqual([]);
  });

  it('decline leaves nothing', () => {
    const a = user('Marcus', '0001'); const b = user('Dom', '0002');
    request(db, a, b);
    decline(db, b, a);
    expect(rows().c).toBe(0);
  });

  it('a blocked player cannot request and is not told; the blocker can lift it by requesting', () => {
    const a = user('Marcus', '0001'); const b = user('Dom', '0002');
    request(db, a, b);
    block(db, b, a);
    expect(request(db, a, b)).toBe('not_found');
    expect(listFor(db, b).incoming).toEqual([]);
    expect(request(db, b, a)).toBe('pending');
    expect(uuids(listFor(db, a).incoming)).toEqual([b]);
  });

  it('remove twice is fine and leaves no rows', () => {
    const a = user('Marcus', '0001'); const b = user('Dom', '0002');
    request(db, a, b); accept(db, b, a);
    remove(db, a, b);
    remove(db, a, b);
    expect(rows().c).toBe(0);
  });

  it('accept without a request is refused', () => {
    const a = user('Marcus', '0001'); const b = user('Dom', '0002');
    expect(accept(db, b, a)).toBe(false);
  });

  it('finds a tag with any name case; refuses self', () => {
    const a = user('Marcus', '0001');
    expect(findByTag(db, 'marcus#0001')?.uuid).toBe(a);
    expect(findByTag(db, 'Marcus#1')).toBeNull();
    expect(findByTag(db, 'Marcus')).toBeNull();
    expect(request(db, a, a)).toBe('self');
  });

  it('limits: 20 pending outgoing, 100 friends', () => {
    const a = user('Marcus', '0001');
    for (let i = 0; i < 20; i++) expect(request(db, a, user('Pending', String(100 + i).padStart(4, '0')))).toBe('pending');
    expect(request(db, a, user('Pending', '0200'))).toBe('limit');
    const b = user('Popular', '0001');
    for (let i = 0; i < 100; i++) {
      const f = user('Fan', String(1000 + i).padStart(4, '0'));
      request(db, f, b); accept(db, b, f);
    }
    expect(request(db, user('Late', '0001'), b)).toBe('limit');
  });
});
