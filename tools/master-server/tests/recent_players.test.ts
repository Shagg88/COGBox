import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { buildServer } from '../src/server.js';
import { Registry } from '../src/social/registry.js';
import WebSocket from 'ws';

// GitHub #353: players whose games connected (one match) see each other under Social > Recent players, so a tag seen in
// a match can be added as a friend.
const SESSION_INFO = '11'.repeat(8) + '22'.repeat(36) + '33'.repeat(16);
const G3 = '4D5308AB';

describe('recent players (registry)', () => {
  it('both sides, newest first, at most RECENT_MAX, forgotten after RECENT_MS', () => {
    let t = 1_000;
    const r = new Registry(() => t);
    r.noteMet('a', 'b', G3);
    t += 10;
    r.noteMet('c', 'a');
    expect(r.recentFor('a').map((x) => x.uuid)).toEqual(['c', 'b']);
    expect(r.recentFor('b').map((x) => x.uuid)).toEqual(['a']);
    expect(r.recentFor('b')[0].titleId).toBe(G3);
    r.noteMet('a', 'a');
    expect(r.recentFor('a').some((x) => x.uuid === 'a')).toBe(false);
    for (let i = 0; i < Registry.RECENT_MAX + 5; i++) { t += 1; r.noteMet('a', `p${i}`); }
    const list = r.recentFor('a');
    expect(list.length).toBe(Registry.RECENT_MAX);
    expect(list[0].uuid).toBe(`p${Registry.RECENT_MAX + 4}`);
    t += Registry.RECENT_MS + 1;
    expect(r.recentFor('a')).toEqual([]);
  });
});

describe('recent players (route)', () => {
  let app: any;
  let address: string;
  let n = 0;
  interface Claimed { uuid: string; token: string; xuid: string; discriminator: string; display_name: string }
  const claim = async (): Promise<Claimed> =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: `Recent ${String.fromCharCode(65 + (n++ % 26))}${n}` } })).json();
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });
  const tag = (c: Claimed) => `${c.display_name}#${c.discriminator}`;
  const open = (c: Claimed): Promise<WebSocket> => new Promise((resolve, reject) => {
    const ws = new WebSocket(`${address}/ws/signal?uuid=${c.uuid}&token=${c.token}&role=game&title_id=${G3}`);
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
  });
  const next = (ws: WebSocket, type: string): Promise<any> => new Promise((resolve) => {
    const on = (d: any) => { const m = JSON.parse(d.toString()); if (m.type === type) { ws.off('message', on); resolve(m); } };
    ws.on('message', on);
  });
  const cands = (to: Claimed) => JSON.stringify({ type: 'candidates', target_uuid: to.uuid, session_token: 'ab', port: 1000,
    candidates: [{ ip: '203.0.113.7', port: 40000, kind: 'srflx' }] });
  const recent = async (c: Claimed) =>
    (await app.inject({ method: 'GET', url: '/api/v1/friends/recent', headers: auth(c) })).json().recent as any[];
  const closeAll = async (...ws: WebSocket[]) => {
    await Promise.all(ws.map((w) => new Promise((r) => { w.once('close', r); w.close(); })));
    await new Promise((r) => setTimeout(r, 100));
  };

  beforeAll(async () => {
    app = await buildServer({ dbPath: ':memory:', rateLimitMax: 10000 });
    await app.listen({ port: 0, host: '127.0.0.1' });
    address = `ws://127.0.0.1:${app.server.address().port}`;
  });
  afterAll(async () => { await app.close(); });

  it('needs a signed-in player', async () => {
    expect((await app.inject({ method: 'GET', url: '/api/v1/friends/recent' })).statusCode).toBe(401);
  });

  it('a guest who connected to a public host: both see the other; adding moves them out of the list', async () => {
    const h = await claim(); const g = await claim(); const s = await claim();
    await app.inject({ method: 'POST', url: '/api/v1/lobbies', headers: auth(h),
      payload: { title_id: G3, session_info: SESSION_INFO, is_public: true } });
    const [wh, wg] = [await open(h), await open(g)];
    const got = next(wh, 'candidates');
    wg.send(cands(h));
    await got;
    const forH = await recent(h), forG = await recent(g);
    expect(forH.map((r) => r.tag)).toEqual([tag(g)]);
    expect(forG.map((r) => r.tag)).toEqual([tag(h)]);
    expect(forG[0].title_id).toBe(G3);
    expect(Object.keys(forG[0]).sort()).toEqual(['at', 'icon', 'tag', 'title_id', 'uuid']);   // no xuid, no address
    expect(await recent(s)).toEqual([]);   // someone who was not there
    await app.inject({ method: 'POST', url: '/api/v1/friends/requests', headers: auth(g), payload: { tag: tag(h) } });
    expect(await recent(g)).toEqual([]);   // a pending request: in Requests, not here
    expect(await recent(h)).toEqual([]);   // the incoming request, likewise
    await closeAll(wh, wg);
  });

  it('a refused connection (strangers) is not a meeting; a block hides the player both ways', async () => {
    const a = await claim(); const b = await claim();
    const [wa, wb] = [await open(a), await open(b)];
    const refused = next(wa, 'candidates_error');
    wa.send(cands(b));
    await refused;
    expect(await recent(a)).toEqual([]);
    const h = await claim();
    await app.inject({ method: 'POST', url: '/api/v1/lobbies', headers: auth(h),
      payload: { title_id: G3, session_info: SESSION_INFO, is_public: true } });
    const wh = await open(h);
    const got = next(wh, 'candidates');
    wa.send(cands(h));
    await got;
    expect((await recent(a)).length).toBe(1);
    await app.inject({ method: 'POST', url: `/api/v1/friends/${a.uuid}/block`, headers: auth(h) });
    expect(await recent(h)).toEqual([]);
    expect(await recent(a)).toEqual([]);
    await closeAll(wa, wb, wh);
  });
});
