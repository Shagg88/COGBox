import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { buildServer } from '../src/server.js';
import WebSocket from 'ws';

// Open matches (Social tab): public match lobbies of Gears 2 / 3 / Judgment whose host's game is online, with no
// name, uuid, xuid or address, and Join from the list.
const SESSION_INFO = '11'.repeat(8) + '22'.repeat(36) + '33'.repeat(16);
const G1 = '4D5307D5';
const G3 = '4D5308AB';
const PLAYLIST = 0x1000000b;

describe('open matches', () => {
  let app: any;
  let address: string;
  let n = 0;
  const sockets: WebSocket[] = [];

  interface Claimed { uuid: string; token: string; xuid: string; discriminator: string; display_name: string }
  const claim = async (): Promise<Claimed> =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: `Match ${String.fromCharCode(65 + (n++ % 26))}${n}` } })).json();
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });
  const host = async (c: Claimed, opts: { title?: string; isPublic?: boolean; max?: number; players?: number; playlist?: number } = {}) =>
    (await app.inject({ method: 'POST', url: '/api/v1/lobbies', headers: auth(c),
      payload: { title_id: opts.title ?? G3, session_info: SESSION_INFO, is_public: opts.isPublic ?? true, region: 'us',
                 max_players: opts.max ?? 10, current_players: opts.players ?? 3,
                 properties: [{ id: PLAYLIST, value: opts.playlist ?? 15 }], contexts: [{ id: 0x800b, value: 3 }] } })).json() as
      { lobby_id: string; room_code: string };
  const game = (c: Claimed, title = G3): Promise<WebSocket> => new Promise((resolve, reject) => {
    const ws = new WebSocket(`${address}/ws/signal?uuid=${c.uuid}&token=${c.token}&role=game&title_id=${title}`);
    ws.once('open', () => { sockets.push(ws); setTimeout(() => resolve(ws), 50); });
    ws.once('error', reject);
  });
  const next = (ws: WebSocket, type: string): Promise<any> => new Promise((resolve) => {
    const on = (d: any) => { const m = JSON.parse(d.toString()); if (m.type === type) { ws.off('message', on); resolve(m); } };
    ws.on('message', on);
  });
  const list = async (c: Claimed) => (await app.inject({ method: 'GET', url: '/api/v1/matches', headers: auth(c) })).json().matches as any[];

  beforeAll(async () => {
    app = await buildServer({ dbPath: ':memory:', rateLimitMax: 10000 });
    await app.listen({ port: 0, host: '127.0.0.1' });
    address = `ws://127.0.0.1:${app.server.address().port}`;
  });
  afterAll(async () => { for (const s of sockets) s.close(); await new Promise((r) => setTimeout(r, 100)); await app.close(); });

  it('needs a signed-in player', async () => {
    expect((await app.inject({ method: 'GET', url: '/api/v1/matches' })).statusCode).toBe(401);
  });

  it('lists a public match with its game, playlist, players and region, and nothing that names the host', async () => {
    const h = await claim(); const v = await claim();
    await game(h);
    const lobby = await host(h, { playlist: 15, players: 6 });
    const m = (await list(v)).find((x) => x.lobby_id === lobby.lobby_id);
    expect(m).toEqual({ lobby_id: lobby.lobby_id, title_id: G3, region: 'us', max_players: 10, current_players: 6,
                        playlist: 15, in_progress: false });
    const text = JSON.stringify(await list(v));
    for (const secret of [h.uuid, h.xuid, h.display_name, lobby.room_code, SESSION_INFO]) expect(text).not.toContain(secret);
  });

  it('leaves out parties / friends-only lobbies, Gears 1, the caller\'s own match and hosts whose game is not running', async () => {
    const v = await claim();
    const a = await claim(); await game(a); const priv = await host(a, { isPublic: false });
    const b = await claim(); await game(b, G1); const g1 = await host(b, { title: G1 });
    const c = await claim(); const offline = await host(c);   // no game connected
    await game(v); const mine = await host(v);
    const ids = (await list(v)).map((x) => x.lobby_id);
    for (const l of [priv, g1, offline, mine]) expect(ids).not.toContain(l.lobby_id);
  });

  it('shows in progress once the host marks the match started', async () => {
    const h = await claim(); const v = await claim();
    await game(h);
    const lobby = await host(h);
    const r = await app.inject({ method: 'PATCH', url: `/api/v1/lobbies/${lobby.lobby_id}`, headers: auth(h), payload: { in_progress: true } });
    expect(r.statusCode).toBe(200);
    expect((await list(v)).find((x) => x.lobby_id === lobby.lobby_id).in_progress).toBe(true);
    expect((await app.inject({ method: 'PATCH', url: `/api/v1/lobbies/${lobby.lobby_id}`, headers: auth(h), payload: { in_progress: 'yes' } })).statusCode).toBe(400);
  });

  it('busiest first', async () => {
    const v = await claim();
    const a = await claim(); await game(a); const few = await host(a, { players: 2 });
    const b = await claim(); await game(b); const many = await host(b, { players: 9 });
    const ids = (await list(v)).map((x) => x.lobby_id);
    expect(ids.indexOf(many.lobby_id)).toBeLessThan(ids.indexOf(few.lobby_id));
  });

  it('Join tells the running game, or answers 409 with the room code so the launcher starts it', async () => {
    const h = await claim(); const j = await claim();
    await game(h);
    const lobby = await host(h);
    const started = await app.inject({ method: 'POST', url: `/api/v1/matches/${lobby.lobby_id}/join`, headers: auth(j) });
    expect(started.statusCode).toBe(409);
    expect(started.json()).toMatchObject({ title_id: G3, room_code: lobby.room_code });
    const jg = await game(j);
    const told = next(jg, 'join');
    const r = await app.inject({ method: 'POST', url: `/api/v1/matches/${lobby.lobby_id}/join`, headers: auth(j) });
    expect(r.statusCode).toBe(200);
    expect((await told).room_code).toBe(lobby.room_code);
  });

  it('Join refuses a full match, a private lobby and one that is gone', async () => {
    const h = await claim(); const j = await claim();
    await game(h);
    const full = await host(h, { players: 10, max: 10 });
    expect((await app.inject({ method: 'POST', url: `/api/v1/matches/${full.lobby_id}/join`, headers: auth(j) })).statusCode).toBe(403);
    const k = await claim(); await game(k);
    const priv = await host(k, { isPublic: false });
    expect((await app.inject({ method: 'POST', url: `/api/v1/matches/${priv.lobby_id}/join`, headers: auth(j) })).statusCode).toBe(404);
    expect((await app.inject({ method: 'POST', url: '/api/v1/matches/nope/join', headers: auth(j) })).statusCode).toBe(404);
  });
});
