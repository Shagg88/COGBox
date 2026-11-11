import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { buildServer } from '../src/server.js';
import WebSocket from 'ws';

// Social step 4: friends-only lobbies, Join friend and invites.
const SESSION_INFO = '11'.repeat(8) + '22'.repeat(36) + '33'.repeat(16);
const G3 = '4D5308AB';
const G2 = '4D5307E6';   // a title not confirmed for joins (not one of the four games)

describe('join and invites', () => {
  let app: any;
  let address: string;
  let clock = 1_000_000_000;
  let n = 0;

  interface Claimed { uuid: string; token: string; xuid: string; discriminator: string; display_name: string }
  const claim = async (): Promise<Claimed> =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: `Player ${String.fromCharCode(65 + (n++ % 26))}${n}` } })).json();
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });
  const tag = (c: Claimed) => `${c.display_name}#${c.discriminator}`;
  const befriend = async (a: Claimed, b: Claimed) => {
    await app.inject({ method: 'POST', url: '/api/v1/friends/requests', headers: auth(a), payload: { tag: tag(b) } });
    await app.inject({ method: 'POST', url: `/api/v1/friends/${a.uuid}/accept`, headers: auth(b) });
  };
  const host = async (c: Claimed, title = G3, isPublic = false) =>
    (await app.inject({ method: 'POST', url: '/api/v1/lobbies', headers: auth(c),
      payload: { title_id: title, session_info: SESSION_INFO, is_public: isPublic } })).json() as { lobby_id: string; room_code: string };
  const open = (c: Claimed, query: string): Promise<WebSocket> => new Promise((resolve, reject) => {
    const ws = new WebSocket(`${address}/ws/signal?uuid=${c.uuid}&token=${c.token}${query}`);
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
  });
  const next = (ws: WebSocket, type: string): Promise<any> => new Promise((resolve) => {
    const on = (d: any) => { const m = JSON.parse(d.toString()); if (m.type === type) { ws.off('message', on); resolve(m); } };
    ws.on('message', on);
  });
  const quiet = (ws: WebSocket, type: string, ms = 300): Promise<boolean> => new Promise((resolve) => {
    let got = false;
    const on = (d: any) => { if (JSON.parse(d.toString()).type === type) got = true; };
    ws.on('message', on);
    setTimeout(() => { ws.off('message', on); resolve(!got); }, ms);
  });
  const presenceOf = async (viewer: Claimed, who: Claimed) =>
    (await app.inject({ method: 'GET', url: '/api/v1/friends', headers: auth(viewer) })).json()
      .friends.find((f: any) => f.uuid === who.uuid)?.presence;

  beforeAll(async () => {
    app = await buildServer({ dbPath: ':memory:', rateLimitMax: 10000, now: () => clock });
    await app.listen({ port: 0, host: '127.0.0.1' });
    address = `ws://127.0.0.1:${app.server.address().port}`;
  });
  afterAll(async () => { await app.close(); });

  it('a friends-only lobby is not in search; its room code answers only the host and friends', async () => {
    const h = await claim(); const f = await claim(); const s = await claim();
    await befriend(h, f);
    const lobby = await host(h);
    const search = await app.inject({ method: 'GET', url: `/api/v1/lobbies?title_id=${G3}` });
    expect(search.json().lobbies.some((l: any) => l.lobby_id === lobby.lobby_id)).toBe(false);
    const url = `/api/v1/lobbies/lookup/${lobby.room_code}`;
    expect((await app.inject({ method: 'GET', url })).statusCode).toBe(404);
    expect((await app.inject({ method: 'GET', url, headers: auth(s) })).statusCode).toBe(404);
    expect((await app.inject({ method: 'GET', url, headers: auth(f) })).statusCode).toBe(200);
    expect((await app.inject({ method: 'GET', url, headers: auth(h) })).statusCode).toBe(200);
  });

  it('presence: a friends-only lobby is joinable for a friend, only in a game confirmed for joins', async () => {
    const h = await claim(); const f = await claim();
    await befriend(h, f);
    const game = await open(h, `&role=game&title_id=${G3}`);
    const lobby = await host(h);
    expect(await presenceOf(f, h)).toMatchObject({ state: 'joinable', title_id: G3, lobby_id: lobby.lobby_id });
    game.close();
    const h2 = await claim();
    await befriend(h2, f);
    const g2 = await open(h2, `&role=game&title_id=${G2}`);
    await host(h2, G2, true);
    expect((await presenceOf(f, h2)).state).toBe('playing');
    g2.close();
  });

  it('Join friend: the joiner\'s game is told the room code; refused for a non-friend or without a game', async () => {
    const h = await claim(); const f = await claim(); const s = await claim();
    await befriend(h, f);
    const hostGame = await open(h, `&role=game&title_id=${G3}`);
    const lobby = await host(h);
    const noGame = await app.inject({ method: 'POST', url: `/api/v1/friends/${h.uuid}/join`, headers: auth(f) });
    expect(noGame.statusCode).toBe(409);
    expect(noGame.json().room_code).toBe(lobby.room_code);   // the launcher starts the game with it
    const fGame = await open(f, `&role=game&title_id=${G3}`);
    const told = next(fGame, 'join');
    const ok = await app.inject({ method: 'POST', url: `/api/v1/friends/${h.uuid}/join`, headers: auth(f) });
    expect(ok.statusCode).toBe(200);
    const m = await told;
    expect(m.room_code).toBe(lobby.room_code);
    expect(m.from_game_invite).toBe(false);
    expect((await app.inject({ method: 'POST', url: `/api/v1/friends/${h.uuid}/join`, headers: auth(s) })).statusCode).toBe(404);
    hostGame.close(); fGame.close();
  });

  it('invites: to a friend while hosting; accept tells the game; decline tells the sender nothing; they expire', async () => {
    const h = await claim(); const f = await claim(); const s = await claim();
    await befriend(h, f);
    const hostGame = await open(h, `&role=game&title_id=${G3}`);
    const fLauncher = await open(f, '&role=launcher');
    expect((await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: f.uuid } })).statusCode).toBe(404);  // nothing hosted yet
    const lobby = await host(h);
    expect((await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: s.uuid } })).statusCode).toBe(404);  // not a friend
    const pushed = next(fLauncher, 'invite');
    const sent = await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: f.uuid } });
    expect(sent.statusCode).toBe(200);
    const inv = await pushed;
    expect(inv.from.uuid).toBe(h.uuid);
    expect(inv.title_id).toBe(G3);
    const list = (await app.inject({ method: 'GET', url: '/api/v1/invites', headers: auth(f) })).json().invites;
    expect(list.map((i: any) => i.id)).toEqual([inv.id]);

    const fGame = await open(f, `&role=game&title_id=${G3}`);
    const told = next(fGame, 'join');
    expect((await app.inject({ method: 'POST', url: `/api/v1/invites/${inv.id}/accept`, headers: auth(f) })).statusCode).toBe(200);
    const m = await told;
    expect(m.room_code).toBe(lobby.room_code);
    expect(m.from_game_invite).toBe(true);

    const second = (await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: f.uuid } })).json();
    const hostQuiet = quiet(hostGame, 'invite_declined');
    expect((await app.inject({ method: 'POST', url: `/api/v1/invites/${second.id}/decline`, headers: auth(f) })).statusCode).toBe(200);
    expect(await hostQuiet).toBe(true);

    const third = (await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: f.uuid } })).json();
    clock += 10 * 60 * 1000 + 1;
    expect((await app.inject({ method: 'GET', url: '/api/v1/invites', headers: auth(f) })).json().invites).toEqual([]);
    expect((await app.inject({ method: 'POST', url: `/api/v1/invites/${third.id}/accept`, headers: auth(f) })).statusCode).toBe(404);
    hostGame.close(); fLauncher.close(); fGame.close();
  });
  it('a pending invite from a deleted account drops out instead of breaking the list', async () => {
    const h = await claim(); const f = await claim();
    await befriend(h, f);
    await host(h);
    const sent = (await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: f.uuid } })).json();
    expect((await app.inject({ method: 'DELETE', url: '/api/v1/auth/account', headers: auth(h) })).statusCode).toBe(200);
    const list = await app.inject({ method: 'GET', url: '/api/v1/invites', headers: auth(f) });
    expect(list.statusCode).toBe(200);
    expect(list.json().invites).toEqual([]);
    expect((await app.inject({ method: 'POST', url: `/api/v1/invites/${sent.id}/accept`, headers: auth(f) })).statusCode).toBe(404);
  });
  it('an invite from someone who is no longer a friend cannot be accepted', async () => {
    const h = await claim(); const f = await claim();
    await befriend(h, f);
    await host(h);
    const ws = await open(f, `&role=game&title_id=${G3}`);
    const sent = (await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: f.uuid } })).json();
    await app.inject({ method: 'DELETE', url: `/api/v1/friends/${f.uuid}`, headers: auth(h) });
    const res = await app.inject({ method: 'POST', url: `/api/v1/invites/${sent.id}/accept`, headers: auth(f) });
    expect(res.statusCode).toBe(404);
    await new Promise((r) => { ws.once('close', r); ws.close(); });
    await new Promise((r) => setTimeout(r, 100));   // the server's presence update for the closed game
  });
  it('a player polling from the launcher shows online for 60 s, then offline; a game still shows playing', async () => {
    const a = await claim(); const b = await claim();
    await befriend(a, b);
    clock += 61_000;   // A's request while befriending showed A online for 60 s
    expect((await presenceOf(b, a)).state).toBe('offline');
    await app.inject({ method: 'GET', url: '/api/v1/friends', headers: auth(a) });   // the launcher's poll
    expect((await presenceOf(b, a)).state).toBe('online');
    clock += 30_000;
    expect((await presenceOf(b, a)).state).toBe('online');
    clock += 31_000;
    expect((await presenceOf(b, a)).state).toBe('offline');
    const ws = await open(a, `&role=game&title_id=${G3}`);
    expect((await presenceOf(b, a)).state).toBe('playing');
    await new Promise((r) => { ws.once('close', r); ws.close(); });
    await new Promise((r) => setTimeout(r, 100));
  });
  it("a joinable friend's presence carries the session id and room code", async () => {
    const a = await claim(); const b = await claim();
    await befriend(a, b);
    const ws = await open(a, `&role=game&title_id=${G3}`);
    const l = await host(a);
    const p = await presenceOf(b, a);
    expect(p.state).toBe('joinable');
    expect(p.session_id).toBe('11'.repeat(8));
    expect(p.room_code).toBe((l as any).room_code);
    await new Promise((r) => { ws.once('close', r); ws.close(); });
    await new Promise((r) => setTimeout(r, 100));
  });
  const cands = (to: Claimed) => JSON.stringify({ type: 'candidates', target_uuid: to.uuid, session_token: 'ab', port: 1000,
    candidates: [{ ip: '203.0.113.7', port: 40000, kind: 'srflx' }] });
  const gameOf = (c: Claimed) => open(c, `&role=game&title_id=${G3}`);
  const closeAll = async (...ws: WebSocket[]) => {
    await Promise.all(ws.map((w) => new Promise((r) => { w.once('close', r); w.close(); })));
    await new Promise((r) => setTimeout(r, 100));
  };
  it('a stranger cannot send connection details to a player', async () => {
    const a = await claim(); const b = await claim();
    const [wa, wb] = [await gameOf(a), await gameOf(b)];
    const refused = next(wa, 'candidates_error');
    const silent = quiet(wb, 'candidates');
    wa.send(cands(b));
    expect((await refused).type).toBe('candidates_error');
    expect(await silent).toBe(true);
    await closeAll(wa, wb);
  });
  it('friends can connect', async () => {
    const a = await claim(); const b = await claim();
    await befriend(a, b);
    const [wa, wb] = [await gameOf(a), await gameOf(b)];
    const got = next(wb, 'candidates');
    wa.send(cands(b));
    expect((await got).from_uuid).toBe(a.uuid);
    await closeAll(wa, wb);
  });
  it('anyone can connect to a public host, the host can answer, and players of one match can reach each other', async () => {
    const h = await claim(); const g1 = await claim(); const g2 = await claim();
    const [wh, w1, w2] = [await gameOf(h), await gameOf(g1), await gameOf(g2)];
    await host(h, G3, true);
    let got = next(wh, 'candidates'); w1.send(cands(h)); await got;     // a guest to the public host
    got = next(w1, 'candidates'); wh.send(cands(g1)); await got;        // the host answers a stranger who joined
    got = next(wh, 'candidates'); w2.send(cands(h)); await got;
    got = next(w2, 'candidates'); w1.send(cands(g2)); await got;        // guest to guest, through the shared host
    await closeAll(wh, w1, w2);
  });
  it('a friends-only host is not reachable by a stranger', async () => {
    const h = await claim(); const s = await claim();
    const [wh, ws] = [await gameOf(h), await gameOf(s)];
    await host(h, G3, false);
    const refused = next(ws, 'candidates_error');
    ws.send(cands(h));
    await refused;
    await closeAll(wh, ws);
  });
  it('a launcher going offline shows offline at once', async () => {
    const a = await claim(); const b = await claim();
    await befriend(a, b);
    await app.inject({ method: 'GET', url: '/api/v1/friends', headers: auth(a) });
    expect((await presenceOf(b, a)).state).toBe('online');
    const res = await app.inject({ method: 'POST', url: '/api/v1/friends/offline', headers: auth(a) });
    expect(res.statusCode).toBe(200);
    expect((await presenceOf(b, a)).state).toBe('offline');
  });
  it("a game's requests do not keep a player online after its connection closes", async () => {
    const a = await claim(); const b = await claim();
    await befriend(a, b);
    clock += 61_000;
    const ws = await open(a, `&role=game&title_id=${G3}`);
    await app.inject({ method: 'GET', url: '/api/v1/friends', headers: { ...auth(a), 'user-agent': 'COGBox-online/1' } });
    await new Promise((r) => { ws.once('close', r); ws.close(); });
    await new Promise((r) => setTimeout(r, 100));
    expect((await presenceOf(b, a)).state).toBe('offline');
  });

  it('Invite Only: friends cannot Join and see no join, but the host can still invite, and it can be turned off', async () => {
    const h = await claim(); const f = await claim();
    await befriend(h, f);
    const hostGame = await open(h, `&role=game&title_id=${G3}`);
    const fGame = await open(f, `&role=game&title_id=${G3}`);
    const lobby = await host(h);
    const set = (on: boolean) => app.inject({ method: 'PATCH', url: `/api/v1/lobbies/${lobby.lobby_id}`, headers: auth(h),
                                             payload: { invite_only: on } });
    const join = () => app.inject({ method: 'POST', url: `/api/v1/friends/${h.uuid}/join`, headers: auth(f) });
    expect((await set(true)).statusCode).toBe(200);
    expect((await join()).statusCode).toBe(404);
    expect((await presenceOf(f, h)).state).toBe('playing');
    const invited = await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_uuid: f.uuid } });
    expect(invited.statusCode).toBe(200);
    expect((await set(false)).statusCode).toBe(200);
    expect((await join()).statusCode).toBe(200);
    hostGame.close(); fGame.close();
  });

  it('a host can report how many players its lobby has; search shows it', async () => {
    const h = await claim(); const f = await claim();
    const lobby = await host(h, G3, true);
    const url = `/api/v1/lobbies/${lobby.lobby_id}`;
    const set = (who: Claimed, n: unknown) => app.inject({ method: 'PATCH', url, headers: auth(who), payload: { current_players: n } });
    expect((await set(h, 4)).statusCode).toBe(200);
    const found = (await app.inject({ method: 'GET', url: `/api/v1/lobbies?title_id=${G3}` })).json().lobbies.find((l: any) => l.lobby_id === lobby.lobby_id);
    expect(found.current_players).toBe(4);
    expect((await set(f, 2)).statusCode).toBe(403);
    expect((await set(h, 0)).statusCode).toBe(400);
    expect((await set(h, 'x')).statusCode).toBe(400);
  });

  it('region: a search finds only its own region; a search for Any finds every region', async () => {
    const mk = async (region: string) => {
      const h = await claim();
      return (await app.inject({ method: 'POST', url: '/api/v1/lobbies', headers: auth(h), payload: {
        title_id: 'AAAAAAAA', session_info: '11'.repeat(60), region, is_public: true, max_players: 10, properties: [], contexts: [] } })).json().lobby_id;
    };
    const us = await mk('us-e'); const eu = await mk('europe'); const any = await mk('global');
    const ids = async (q: string) => (await app.inject({ method: 'GET', url: `/api/v1/lobbies?title_id=AAAAAAAA${q}` })).json().lobbies.map((l: any) => l.lobby_id);
    const inUs = await ids('&region=us-e');
    expect(inUs).toContain(us); expect(inUs).not.toContain(any); expect(inUs).not.toContain(eu);
    const all = await ids('');
    expect(all).toContain(us); expect(all).toContain(eu); expect(all).toContain(any);
  });

  it('population: players per region, across every game', async () => {
    const before = (await app.inject({ method: 'GET', url: '/api/v1/population/regions' })).json();
    const a = await claim(); const b = await claim();
    const [wa, wb] = await Promise.all([open(a, `&role=game&title_id=${G3}&region=oceania`), open(b, `&role=game&title_id=${G2}&region=oceania`)]);
    const now = (await app.inject({ method: 'GET', url: '/api/v1/population/regions' })).json();
    expect(now.oceania).toBe(before.oceania + 2);
    expect(Object.keys(now).sort()).toEqual(['europe', 'oceania', 'us-e', 'us-w']);
    await Promise.all([wa, wb].map((w) => new Promise((r) => { w.once('close', r); w.close(); })));
    await new Promise((r) => setTimeout(r, 100));
  });

  it('population: counts games online by title and region', async () => {
    const a = await claim(); const b = await claim(); const c = await claim();
    const T = 'BBBBBBBB';
    const [wa, wb, wc] = await Promise.all([open(a, `&role=game&title_id=${T}&region=us-e`), open(b, `&role=game&title_id=${T}&region=europe`), open(c, `&role=game&title_id=${T}`)]);
    const pop = async (q: string) => (await app.inject({ method: 'GET', url: `/api/v1/population?title_id=${T}${q}` })).json();
    expect(await pop('&region=us-e')).toEqual({ worldwide: 3, region: 1 });
    expect(await pop('')).toEqual({ worldwide: 3, region: 3 });
    expect(await pop('&region=global')).toEqual({ worldwide: 3, region: 3 });
    await Promise.all([wa, wb, wc].map((w) => new Promise((r) => { w.once('close', r); w.close(); })));
    await new Promise((r) => setTimeout(r, 100));
    expect(await pop('')).toEqual({ worldwide: 0, region: 0 });
  });

  it('non-friends: a player met through a match can be invited by XUID, joined, and nobody else', async () => {
    const h = await claim(); const s = await claim(); const other = await claim();
    const [hGame, sGame] = await Promise.all([gameOf(h), gameOf(s)]);
    await host(h, G3, true);
    const got = next(hGame, 'candidates');
    sGame.send(cands(h));
    await got;   // they have met
    const party = await host(h);   // the friends-only party the invite is to
    const invite = (who: Claimed, to: Claimed) =>
      app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(who), payload: { to_xuid: to.xuid } });
    expect((await invite(h, other)).statusCode).toBe(404);
    const url = `/api/v1/lobbies/lookup/${party.room_code}`;
    expect((await app.inject({ method: 'GET', url, headers: auth(other) })).statusCode).toBe(404);
    expect((await app.inject({ method: 'GET', url, headers: auth(s) })).statusCode).toBe(200);
    expect((await invite(h, s)).statusCode).toBe(200);
    const inv = (await app.inject({ method: 'GET', url: '/api/v1/invites', headers: auth(s) })).json().invites[0];
    expect(inv.room_code).toBe(party.room_code);
    const told = next(sGame, 'join');
    expect((await app.inject({ method: 'POST', url: `/api/v1/invites/${inv.id}/accept`, headers: auth(s) })).statusCode).toBe(200);
    expect((await told).room_code).toBe(party.room_code);
    await closeAll(hGame, sGame);
  });

  it('non-friends: someone who blocked you cannot be invited, even after meeting', async () => {
    const h = await claim(); const s = await claim();
    const [hGame, sGame] = await Promise.all([gameOf(h), gameOf(s)]);
    await host(h, G3, true);
    const got = next(hGame, 'candidates');
    sGame.send(cands(h));
    await got;
    expect((await app.inject({ method: 'POST', url: `/api/v1/friends/${h.uuid}/block`, headers: auth(s) })).statusCode).toBe(200);
    expect((await app.inject({ method: 'POST', url: '/api/v1/invites', headers: auth(h), payload: { to_xuid: s.xuid } })).statusCode).toBe(404);
    await closeAll(hGame, sGame);
  });

  it('a match does not replace the host party lobby, so friends can still join after it', async () => {
    const h = await claim(); const f = await claim();
    await befriend(h, f);
    const [hGame, fGame] = await Promise.all([gameOf(h), gameOf(f)]);
    const party = await host(h);
    const match = await host(h, G3, true);
    expect((await app.inject({ method: 'DELETE', url: `/api/v1/lobbies/${match.lobby_id}`, headers: auth(h) })).statusCode).toBe(200);
    const told = next(fGame, 'join');
    expect((await app.inject({ method: 'POST', url: `/api/v1/friends/${h.uuid}/join`, headers: auth(f) })).statusCode).toBe(200);
    expect((await told).room_code).toBe(party.room_code);
    await closeAll(hGame, fGame);
  });

  it('a new lobby of the same kind still replaces the old one', async () => {
    const h = await claim();
    await host(h, G3, true);
    const second = await host(h, G3, true);
    const list = (await app.inject({ method: 'GET', url: `/api/v1/lobbies?title_id=${G3}` })).json().lobbies.filter((l: any) => l.host_uuid === h.uuid);
    expect(list.map((l: any) => l.lobby_id)).toEqual([second.lobby_id]);
  });

  it('guests of one match reach each other by XUID; a stranger cannot be reached that way', async () => {
    const h = await claim(); const a = await claim(); const b = await claim(); const c = await claim();
    const [hG, aG, bG, cG] = await Promise.all([gameOf(h), gameOf(a), gameOf(b), gameOf(c)]);
    await host(h, G3, true);
    for (const [g] of [[aG], [bG]]) {
      const got = next(hG, 'candidates');
      g.send(cands(h));
      await got;
    }
    const byXuid = (to: Claimed) => JSON.stringify({ ...JSON.parse(cands(to)), target_uuid: undefined, target_xuid: to.xuid });
    const told = next(bG, 'candidates');
    aG.send(byXuid(b));
    const m = await told;
    expect(m.from_uuid).toBe(a.uuid);
    expect(m.from_xuid).toBe(a.xuid);
    const refused = next(aG, 'candidates_error');
    aG.send(byXuid(c));
    await refused;
    await closeAll(hG, aG, bG, cG);
  });

  it('Invite Only: only the host can set it, and only to true or false', async () => {
    const h = await claim(); const f = await claim();
    const lobby = await host(h);
    const url = `/api/v1/lobbies/${lobby.lobby_id}`;
    expect((await app.inject({ method: 'PATCH', url, headers: auth(f), payload: { invite_only: true } })).statusCode).toBe(403);
    expect((await app.inject({ method: 'PATCH', url, headers: auth(h), payload: { invite_only: 'yes' } })).statusCode).toBe(400);
  });
});
