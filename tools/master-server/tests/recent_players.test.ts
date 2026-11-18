import { describe, it, expect, beforeEach } from 'vitest';
import { initDb, DatabaseInstance } from '../src/db.js';
import { recordRecent, recentFor, RECENT_DAYS, RECENT_MAX } from '../src/social/recent.js';

// Recent players: the COGBox players you were in a match with, newest first, so you can add them as friends.
describe('recent players', () => {
  let db: DatabaseInstance;
  const user = (uuid: string, name: string, disc: string, xuid: string) =>
    db.prepare(`INSERT INTO users (uuid, display_name, discriminator, xuid, token_hash, created_at, last_seen)
                VALUES (?, ?, ?, ?, 'h', 0, 0)`).run(uuid, name, disc, xuid);
  const link = (from: string, to: string, status: string) =>
    db.prepare('INSERT INTO friends (user_uuid, friend_uuid, status, updated_at) VALUES (?, ?, ?, 0)').run(from, to, status);
  const day = 24 * 3600 * 1000;

  beforeEach(() => {
    db = initDb(':memory:');
    user('me', 'Flapper', '5531', '100');
    user('u1', 'Raven', '2210', '101');
    user('u2', 'Kestrel', '0917', '102');
  });

  it('keeps the other COGBox players of a match, newest first, without yourself or unknown XUIDs', () => {
    recordRecent(db, 'me', '4D5308AB', ['101', '100', '999'], 1000);
    recordRecent(db, 'me', '4D5309C9', ['102'], 2000);
    expect(recentFor(db, 'me', 3000)).toEqual([
      { uuid: 'u2', display_name: 'Kestrel', discriminator: '0917', icon: '', title_id: '4D5309C9', seen_at: 2000, relation: 'none' },
      { uuid: 'u1', display_name: 'Raven', discriminator: '2210', icon: '', title_id: '4D5308AB', seen_at: 1000, relation: 'none' },
    ]);
  });

  it('a player met again moves to the top with the newer game', () => {
    recordRecent(db, 'me', '4D5308AB', ['101', '102'], 1000);
    recordRecent(db, 'me', '4D5309C9', ['101'], 5000);
    const r = recentFor(db, 'me', 6000);
    expect(r.map((p) => [p.uuid, p.title_id, p.seen_at])).toEqual([['u1', '4D5309C9', 5000], ['u2', '4D5308AB', 1000]]);
  });

  it('says whether each is already a friend, asked by you, or asking you', () => {
    user('u3', 'Dizzy', '5104', '103');
    link('me', 'u1', 'accepted'); link('u1', 'me', 'accepted');
    link('me', 'u2', 'pending');
    link('u3', 'me', 'pending');
    recordRecent(db, 'me', '4D5308AB', ['101', '102', '103'], 1000);
    const rel = Object.fromEntries(recentFor(db, 'me', 2000).map((p) => [p.uuid, p.relation]));
    expect(rel).toEqual({ u1: 'friend', u2: 'sent', u3: 'incoming' });
  });

  it('leaves out players either side blocked, ones older than the limit, and keeps at most RECENT_MAX', () => {
    user('u3', 'Dizzy', '5104', '103');
    link('u1', 'me', 'blocked');
    link('me', 'u3', 'blocked');
    recordRecent(db, 'me', '4D5308AB', ['101', '103'], 1000);
    recordRecent(db, 'me', '4D5308AB', ['102'], 1000);
    expect(recentFor(db, 'me', 1000 + (RECENT_DAYS + 1) * day)).toEqual([]);
    expect(recentFor(db, 'me', 2000).map((p) => p.uuid)).toEqual(['u2']);
    for (let i = 0; i < RECENT_MAX + 5; i++) user(`m${i}`, 'P', String(i).padStart(4, '0'), String(2000 + i));
    recordRecent(db, 'me', '4D5308AB', Array.from({ length: 32 }, (_, i) => String(2000 + i)), 3000);
    recordRecent(db, 'me', '4D5308AB', Array.from({ length: 8 }, (_, i) => String(2032 + i)), 3001);
    expect(recentFor(db, 'me', 4000)).toHaveLength(RECENT_MAX);
  });

  it('ignores anything that is not a decimal XUID and more than 32 per report', () => {
    recordRecent(db, 'me', '4D5308AB', ["101' OR 1=1 --", '', 'abc'], 1000);
    expect(recentFor(db, 'me', 2000)).toEqual([]);
    recordRecent(db, 'me', 'not a title', ['101'], 1000);
    expect(recentFor(db, 'me', 2000)).toEqual([]);
  });
});

describe('/api/v1/players/recent', () => {
  let app: any;
  beforeEach(async () => { app = await (await import('../src/server.js')).buildServer({ dbPath: ':memory:', rateLimitMax: 10000 }); });

  it('needs a signed-in account', async () => {
    expect((await app.inject({ method: 'GET', url: '/api/v1/players/recent' })).statusCode).toBe(401);
    expect((await app.inject({ method: 'POST', url: '/api/v1/players/recent', payload: { title_id: '4D5308AB', xuids: [] } })).statusCode).toBe(401);
  });

  it('records a match report and lists it', async () => {
    const claim = async (name: string) =>
      (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: name } })).json();
    const a = await claim('Marcus');
    const b = await claim('Dom');
    const auth = { authorization: `COGBox ${a.uuid}:${a.token}` };
    const post = await app.inject({ method: 'POST', url: '/api/v1/players/recent', headers: auth,
                                    payload: { title_id: '4D5308AB', xuids: [b.xuid, a.xuid] } });
    expect(post.statusCode).toBe(200);
    const list = (await app.inject({ method: 'GET', url: '/api/v1/players/recent', headers: auth })).json();
    expect(list.recent.map((p: any) => [p.uuid, p.display_name, p.title_id])).toEqual([[b.uuid, 'Dom', '4D5308AB']]);
    await app.close();
  });
});
