import { describe, it, expect, afterEach } from 'vitest';
import { buildServer } from '../src/server.js';

// The games' lobby search (GET /api/v1/lobbies): no room code in its results, and a switch (off by default) that
// requires a signed-in player once most players are past 0.6.0, whose runtime searches without signing in.
const SESSION_INFO = '11'.repeat(8) + '22'.repeat(36) + '33'.repeat(16);
const G3 = '4D5308AB';

describe('lobby search', () => {
  let app: any;
  afterEach(async () => { await app?.close(); app = undefined; });

  const start = async (opts: { lobbySearchRequiresSignIn?: boolean } = {}) => {
    app = await buildServer({ dbPath: ':memory:', rateLimitMax: 10000, ...opts });
    const c = (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: 'Searcher' } })).json();
    const auth = { authorization: `COGBox ${c.uuid}:${c.token}` };
    const lobby = (await app.inject({ method: 'POST', url: '/api/v1/lobbies', headers: auth,
      payload: { title_id: G3, session_info: SESSION_INFO, is_public: true } })).json();
    return { auth, lobby };
  };
  const search = (headers: Record<string, string> = {}) =>
    app.inject({ method: 'GET', url: `/api/v1/lobbies?title_id=${G3}`, headers });

  it('leaves the room code out of search results (the lookup by code still has it)', async () => {
    const { auth, lobby } = await start();
    const r = await search();
    expect(r.statusCode).toBe(200);
    const found = r.json().lobbies.find((l: any) => l.lobby_id === lobby.lobby_id);
    expect(found).toBeDefined();
    expect(found).not.toHaveProperty('room_code');
    expect(r.body).not.toContain(lobby.room_code);
    const looked = await app.inject({ method: 'GET', url: `/api/v1/lobbies/lookup/${lobby.room_code}`, headers: auth });
    expect(looked.json().lobby.room_code).toBe(lobby.room_code);
  });

  it('answers without sign-in by default (0.6.0 runtimes search without it)', async () => {
    await start();
    expect((await search()).statusCode).toBe(200);
  });

  it('with the switch on, needs a signed-in player', async () => {
    const { auth } = await start({ lobbySearchRequiresSignIn: true });
    expect((await search()).statusCode).toBe(401);
    expect((await search({ authorization: 'COGBox nobody:wrong' })).statusCode).toBe(401);
    expect((await search(auth)).statusCode).toBe(200);
  });

  it('the switch comes from LOBBY_SEARCH_REQUIRES_SIGN_IN', async () => {
    const before = process.env.LOBBY_SEARCH_REQUIRES_SIGN_IN;
    process.env.LOBBY_SEARCH_REQUIRES_SIGN_IN = 'true';
    try {
      await start();
      expect((await search()).statusCode).toBe(401);
    } finally {
      if (before === undefined) delete process.env.LOBBY_SEARCH_REQUIRES_SIGN_IN;
      else process.env.LOBBY_SEARCH_REQUIRES_SIGN_IN = before;
    }
  });
});
