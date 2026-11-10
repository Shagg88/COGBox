import { describe, it, expect, afterAll, beforeAll } from 'vitest';
import { buildServer } from '../src/server.js';

// Behind Caddy on the same machine: the limit is per player, and only Caddy may say who the player is.
describe('behind the HTTPS proxy', () => {
  let app: any;
  beforeAll(async () => { app = await buildServer({ dbPath: ':memory:', rateLimitMax: 2 }); });
  afterAll(async () => { await app.close(); });
  const get = (remoteAddress: string, xff: string) =>
    app.inject({ method: 'GET', url: '/api/v1/friends', remoteAddress, headers: { 'x-forwarded-for': xff } });

  it('players behind the proxy each have their own limit', async () => {
    for (let i = 0; i < 2; ++i) expect((await get('127.0.0.1', '198.51.100.1')).statusCode).toBe(401);
    expect((await get('127.0.0.1', '198.51.100.1')).statusCode).toBe(429);
    expect((await get('127.0.0.1', '198.51.100.2')).statusCode).toBe(401);
  });
  it('someone connecting directly cannot fake the address', async () => {
    for (let i = 0; i < 2; ++i) expect((await get('203.0.113.9', `198.51.100.${10 + i}`)).statusCode).toBe(401);
    expect((await get('203.0.113.9', '198.51.100.99')).statusCode).toBe(429);
  });
});

// Players behind one address (a household, CGNAT) each get their own limit; made-up accounts do not.
describe('rate limit per signed-in account', () => {
  let app: any;
  beforeAll(async () => { app = await buildServer({ dbPath: ':memory:', rateLimitMax: 2 }); });
  afterAll(async () => { await app.close(); });
  const claim = async (name: string, remoteAddress: string) =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', remoteAddress, payload: { display_name: name } })).json();
  const poll = (authorization: string, remoteAddress = '198.51.100.50') =>
    app.inject({ method: 'GET', url: '/api/v1/friends', remoteAddress, headers: { authorization } });

  it('two accounts on one address have separate limits', async () => {
    const a = await claim('Anya', '203.0.113.1'); const b = await claim('Baird', '203.0.113.2');
    const ah = `COGBox ${a.uuid}:${a.token}`, bh = `COGBox ${b.uuid}:${b.token}`;
    for (let i = 0; i < 2; ++i) expect((await poll(ah)).statusCode).toBe(200);
    expect((await poll(ah)).statusCode).toBe(429);
    expect((await poll(bh)).statusCode).toBe(200);
  });
  it('made-up accounts all count against the address', async () => {
    for (let i = 0; i < 2; ++i) expect((await poll(`COGBox fake${i}:nope`, '198.51.100.60')).statusCode).toBe(401);
    expect((await poll('COGBox fake9:nope', '198.51.100.60')).statusCode).toBe(429);
  });
});
