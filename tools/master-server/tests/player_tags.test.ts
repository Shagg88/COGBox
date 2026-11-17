import { describe, it, expect, beforeEach } from 'vitest';
import { initDb, DatabaseInstance } from '../src/db.js';
import { tagsForXuids, MAX_TAG_LOOKUP } from '../src/social/tags.js';

// GitHub #353: the COGBox tag of each player in a match, by their in-game XUID (the game shows it after the name).
describe('player tags', () => {
  let db: DatabaseInstance;
  const user = (uuid: string, name: string, disc: string, xuid: string) =>
    db.prepare(`INSERT INTO users (uuid, display_name, discriminator, xuid, token_hash, created_at, last_seen)
                VALUES (?, ?, ?, ?, 'h', 0, 0)`).run(uuid, name, disc, xuid);

  beforeEach(() => { db = initDb(':memory:'); });

  it('returns name and discriminator for known XUIDs and leaves unknown ones out', () => {
    user('u1', 'Marcus', '0042', '2533274790395904');
    user('u2', 'Dom', '1234', '2533274790395905');
    const tags = tagsForXuids(db, ['2533274790395904', '9999', '2533274790395905']);
    expect(tags).toEqual([
      { xuid: '2533274790395904', display_name: 'Marcus', discriminator: '0042' },
      { xuid: '2533274790395905', display_name: 'Dom', discriminator: '1234' },
    ]);
  });

  it('ignores anything that is not a decimal XUID, duplicates, and more than the limit', () => {
    user('u1', 'Marcus', '0042', '100');
    expect(tagsForXuids(db, ["100' OR 1=1 --", '', 'abc', '100', '100'])).toEqual([
      { xuid: '100', display_name: 'Marcus', discriminator: '0042' },
    ]);
    const many = Array.from({ length: MAX_TAG_LOOKUP + 5 }, (_, i) => String(1000 + i));
    many.forEach((x, i) => user(`m${i}`, 'P', String(i).padStart(4, '0'), x));
    expect(tagsForXuids(db, many)).toHaveLength(MAX_TAG_LOOKUP);
  });
});

describe('POST /api/v1/players/tags', () => {
  let app: any;
  beforeEach(async () => { app = await (await import('../src/server.js')).buildServer({ dbPath: ':memory:', rateLimitMax: 10000 }); });

  it('needs a signed-in account and returns the tags of the XUIDs asked for', async () => {
    const claim = async (name: string) =>
      (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: name } })).json() as
        { uuid: string; token: string; xuid: string; discriminator: string; display_name: string };
    const a = await claim('Marcus'); const b = await claim('Dom');
    const none = await app.inject({ method: 'POST', url: '/api/v1/players/tags', payload: { xuids: [b.xuid] } });
    expect(none.statusCode).toBe(401);
    const r = await app.inject({ method: 'POST', url: '/api/v1/players/tags', headers: { authorization: `COGBox ${a.uuid}:${a.token}` },
      payload: { xuids: [b.xuid, '12345'] } });
    expect(r.statusCode).toBe(200);
    expect(r.json()).toEqual({ tags: [{ xuid: b.xuid, display_name: 'Dom', discriminator: b.discriminator }] });
    await app.close();
  });
});
