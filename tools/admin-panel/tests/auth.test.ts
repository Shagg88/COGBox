import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { FastifyInstance } from 'fastify';
import { buildApp } from '../src/app.js';
import { openDb, type Db } from '../src/db.js';
import { addUser } from '../src/users.js';
import { totpCode } from '../src/crypto.js';
import type { DockerClient, MasterClient } from '../src/types.js';

const master = {} as MasterClient;
const docker = {} as DockerClient;
const BAD = { error: 'Wrong name, password or code.' };
const MIN = 60_000;

let db: Db, app: FastifyInstance, clock: number, password: string, secret: string;

beforeEach(async () => {
  db = openDb(':memory:');
  const c = await addUser(db, 'alice');
  password = c.password;
  secret = new URL(c.otpauth).searchParams.get('secret')!;
  clock = 1_800_000_000_000;
  app = await buildApp({ db, now: () => clock, master, docker, secureCookie: false });
  // stands in for the Task 5 routes, to check the must_change gate
  app.get('/admin/api/other', async () => ({ ok: true }));
});
afterEach(async () => { await app.close(); db.close(); });

const code = (offset = 0) => totpCode(secret, Math.floor(clock / 30_000) + offset);
const login = (body: object) => app.inject({ method: 'POST', url: '/admin/api/login', payload: body });
function cookieOf(res: { cookies: { name: string; value: string }[] }) {
  const c = res.cookies.find((x) => x.name === 'cogbox_admin');
  return c ? `cogbox_admin=${c.value}` : '';
}
async function signIn(c = code()) {
  const res = await login({ name: 'alice', password, code: c });
  expect(res.statusCode).toBe(200);
  const cookie = cookieOf(res);
  const me = await app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie } });
  return { res, cookie, csrf: me.json().csrf as string };
}
const auditRows = () => db.prepare('SELECT name, action FROM audit ORDER BY id').all();

describe('auth', () => {
  it('login needs all three', async () => {
    for (const body of [{ name: 'alice', password }, { name: 'alice', code: code() }, { password, code: code() }, {}]) {
      const res = await login(body);
      expect(res.statusCode).toBe(401);
      expect(res.json()).toEqual(BAD);
      expect(cookieOf(res)).toBe('');
    }
    const { res, cookie, csrf } = await signIn();
    expect(res.json()).toEqual({ must_change: true });
    expect(csrf).toMatch(/^[0-9a-f]{64}$/);
    const c = res.cookies.find((x) => x.name === 'cogbox_admin')! as any;
    expect(c.httpOnly).toBe(true);
    expect(c.sameSite).toBe('Strict');
    expect(c.path).toBe('/admin');
    expect(c.value).toMatch(/^[0-9a-f]{64}$/);
    // only a hash of the session id is stored
    const ids = db.prepare('SELECT id FROM sessions').all() as { id: string }[];
    expect(ids).toHaveLength(1);
    expect(ids[0].id).not.toBe(c.value);
  });

  it('secure cookie by default', async () => {
    const app2 = await buildApp({ db, now: () => clock, master, docker });
    const res = await app2.inject({ method: 'POST', url: '/admin/api/login', payload: { name: 'alice', password, code: code() } });
    expect((res.cookies.find((x) => x.name === 'cogbox_admin') as any).secure).toBe(true);
    await app2.close();
  });

  it('same error for wrong name, password or code', async () => {
    for (const body of [
      { name: 'bob', password, code: code() },
      { name: 'alice', password: 'wrong password!', code: code() },
      { name: 'alice', password, code: code(5) },
    ]) {
      const res = await login(body);
      expect(res.statusCode).toBe(401);
      expect(res.json()).toEqual(BAD);
    }
  });

  it('locks after 5 failures for 15 minutes', async () => {
    for (let i = 0; i < 5; i++) expect((await login({ name: 'alice', password: 'nope', code: '000000' })).statusCode).toBe(401);
    const locked = await login({ name: 'alice', password, code: code() });
    expect(locked.statusCode).toBe(423);
    expect(locked.json()).toEqual({ error: 'Locked for 15 minutes after 5 failed sign-ins.' });
    clock += 15 * MIN - 1000;
    expect((await login({ name: 'alice', password, code: code() })).statusCode).toBe(423);
    clock += 1000;
    await signIn();
    expect((db.prepare('SELECT failed FROM users').get() as { failed: number }).failed).toBe(0);
  });

  it('success resets the failure count', async () => {
    for (let i = 0; i < 4; i++) await login({ name: 'alice', password: 'nope', code: '000000' });
    await signIn();
    for (let i = 0; i < 4; i++) await login({ name: 'alice', password: 'nope', code: '000000' });
    clock += 30_000;
    expect((await login({ name: 'alice', password, code: code() })).statusCode).toBe(200);
  });

  it('reused code refused', async () => {
    const c = code();
    await signIn(c);
    expect((await login({ name: 'alice', password, code: c })).statusCode).toBe(401);
    // an older step still inside the window is refused too
    expect((await login({ name: 'alice', password, code: code(-1) })).statusCode).toBe(401);
    clock += 30_000;
    expect((await login({ name: 'alice', password, code: code() })).statusCode).toBe(200);
    expect((db.prepare('SELECT last_totp_step FROM users').get() as any).last_totp_step).toBe(Math.floor(clock / 30_000));
  });

  it('absolute session expiry 12 h', async () => {
    const { cookie } = await signIn();
    const me = () => app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie } });
    for (let i = 0; i < 36; i++) { // 36 x 20 min = 12 h, never idle long
      clock += 20 * MIN;
      expect((await me()).statusCode).toBe(200);
    }
    clock += 1;
    expect((await me()).statusCode).toBe(401);
  });

  it('idle session expires', async () => {
    const { cookie } = await signIn();
    const me = () => app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie } });
    clock += 30 * MIN;
    expect((await me()).statusCode).toBe(200);
    clock += 30 * MIN + 1;
    expect((await me()).statusCode).toBe(401);
    expect((await me()).statusCode).toBe(401);
    expect((await app.inject({ method: 'GET', url: '/admin/api/me' })).statusCode).toBe(401);
    expect((await app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie: 'cogbox_admin=abc' } })).statusCode).toBe(401);
  });

  it('must_change blocks other routes until password set', async () => {
    const { cookie, csrf } = await signIn();
    const other = () => app.inject({ method: 'GET', url: '/admin/api/audit', headers: { cookie } });
    expect((await app.inject({ method: 'GET', url: '/admin/api/audit' })).statusCode).toBe(401);
    expect((await other()).statusCode).toBe(403);
    const me = await app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie } });
    expect(me.json()).toEqual({ name: 'alice', csrf, must_change: true });
    const pw = (p: string) => app.inject({ method: 'POST', url: '/admin/api/password', headers: { cookie, 'x-csrf-token': csrf }, payload: { password: p } });
    expect((await pw('short')).statusCode).toBe(400);
    expect((await pw('a much longer password')).statusCode).toBe(200);
    expect((await other()).statusCode).toBe(200);
    expect((await app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie } })).json().must_change).toBe(false);
    expect(auditRows()).toContainEqual({ name: 'alice', action: 'password_changed' });
  });

  it('POST without csrf is 403', async () => {
    const { cookie, csrf } = await signIn();
    const post = (h: Record<string, string>) => app.inject({ method: 'POST', url: '/admin/api/logout', headers: { cookie, ...h } });
    expect((await post({})).statusCode).toBe(403);
    expect((await post({ 'x-csrf-token': 'f'.repeat(64) })).statusCode).toBe(403);
    expect((await post({ 'x-csrf-token': csrf })).statusCode).toBe(200);
    expect((await app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie } })).statusCode).toBe(401);
  });

  it('each login and failure writes one audit row', async () => {
    await login({ name: 'alice', password: 'nope', code: '000000' });
    await login({ name: 'bob', password, code: code() });
    const { cookie, csrf } = await signIn();
    await app.inject({ method: 'POST', url: '/admin/api/logout', headers: { cookie, 'x-csrf-token': csrf } });
    expect(auditRows()).toEqual([
      { name: 'alice', action: 'login_failed' },
      { name: '(unknown name)', action: 'login_failed' },
      { name: 'alice', action: 'login' },
      { name: 'alice', action: 'logout' },
    ]);
  });

  it('concurrent logins with one code: exactly one succeeds', async () => {
    const c = code();
    const rs = await Promise.all([login({ name: 'alice', password, code: c }), login({ name: 'alice', password, code: c })]);
    expect(rs.map((r) => r.statusCode).sort()).toEqual([200, 401]);
    expect(db.prepare('SELECT count(*) AS n FROM sessions').get()).toEqual({ n: 1 });
  });

  it('parallel failures all count towards the lock', async () => {
    await Promise.all(Array.from({ length: 5 }, () => login({ name: 'alice', password: 'nope', code: '000000' })));
    expect((await login({ name: 'alice', password, code: code() })).statusCode).toBe(423);
  });

  it('login is rate limited to 10 a minute', async () => {
    for (let i = 0; i < 10; i++) await login({ name: 'bob', password: 'x', code: '1' });
    expect((await login({ name: 'bob', password: 'x', code: '1' })).statusCode).toBe(429);
  });
});
