import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import Database from 'better-sqlite3';
import { buildServer } from '../src/server.js';
import { buildPrivateApi } from '../src/admin/private_api.js';
import { ErrorRing } from '../src/admin/error_ring.js';

const SESSION_INFO = '11'.repeat(8) + '22'.repeat(36) + '33'.repeat(16);
const SECRET = 'test-secret-0123456789';
const H = { 'x-admin-secret': SECRET };

describe('private admin api', () => {
  let app: any;
  let priv: any;
  let dir: string;
  let changed: string[] = [];
  const errors = new ErrorRing();

  beforeAll(async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cogbox-admin-'));
    app = await buildServer({ dbPath: path.join(dir, 'master.db') });
    const d = app.adminDeps;
    priv = await buildPrivateApi({
      ...d, errors, secret: SECRET, dataDir: dir,
      hostChanged: (u: string) => { changed.push(u); d.hostChanged(u); }
    });
  });
  afterAll(async () => { await priv.close(); await app.close(); fs.rmSync(dir, { recursive: true, force: true }); });

  const claim = async (name: string) =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: name } })).json();
  const host = async (c: any) =>
    (await app.inject({ method: 'POST', url: '/api/v1/lobbies', headers: { authorization: `COGBox ${c.uuid}:${c.token}` },
      payload: { title_id: '4D5308AB', session_info: SESSION_INFO } })).json();

  it('refuses without secret', async () => {
    for (const [method, url] of [['GET', '/status'], ['GET', '/lobbies'], ['POST', '/lobbies/x/close'],
                                 ['POST', '/backup'], ['GET', '/backups']] as const) {
      expect((await priv.inject({ method, url })).statusCode).toBe(401);
      expect((await priv.inject({ method, url, headers: { 'x-admin-secret': 'wrong' } })).statusCode).toBe(401);
    }
  });

  it('status counts players and lobbies', async () => {
    const a = await claim('Alpha');
    await host(a);
    const r = (await priv.inject({ url: '/status', headers: H })).json();
    expect(r.lobbies).toBe(1);
    expect(r.players.total).toBe(0);
    expect(r.players.by_region).toHaveProperty('europe');
    expect(r.pending_invites).toBe(0);
    expect(typeof r.uptime_s).toBe('number');
    expect(typeof r.version).toBe('string');
    expect(r.errors).toEqual([]);
  });

  it('lobbies list has tags and no uuids', async () => {
    const b = await claim('Bravo');
    await host(b);
    const text = (await priv.inject({ url: '/lobbies', headers: H })).body;
    const body = JSON.parse(text);
    expect(body.lobbies.length).toBeGreaterThanOrEqual(2);
    expect(body.lobbies.some((l: any) => l.host_tag === `Bravo#${b.discriminator}`)).toBe(true);
    expect(text).not.toContain(b.uuid);
    expect(text).not.toContain(b.xuid);
    expect(text).not.toContain(b.token);
  });

  it("close removes the lobby and the host's presence updates", async () => {
    const c = await claim('Charlie');
    const l = await host(c);
    changed = [];
    const r = (await priv.inject({ method: 'POST', url: `/lobbies/${l.lobby_id}/close`, headers: H })).json();
    expect(r).toEqual({ closed: true });
    expect(changed).toEqual([c.uuid]);
    expect(app.db.prepare('SELECT 1 FROM lobbies WHERE lobby_id = ?').get(l.lobby_id)).toBeUndefined();
    const again = (await priv.inject({ method: 'POST', url: `/lobbies/${l.lobby_id}/close`, headers: H })).json();
    expect(again).toEqual({ closed: false });
  });

  it('backup makes a database that opens and keeps 14', async () => {
    let last: any;
    for (let i = 0; i < 16; i++) { const res = await priv.inject({ method: 'POST', url: '/backup', headers: H }); if (res.statusCode !== 200) throw new Error(res.body); last = res.json(); }
    expect(last.size).toBeGreaterThan(0);
    const files = fs.readdirSync(path.join(dir, 'backups'));
    expect(files.length).toBe(14);
    const list = (await priv.inject({ url: '/backups', headers: H })).json().backups;
    expect(list.length).toBe(14);
    expect(list[0].name).toBe(last.name);
    const copy = new Database(path.join(dir, 'backups', list[0].name), { readonly: true });
    expect((copy.prepare('SELECT count(*) AS n FROM users').get() as any).n).toBeGreaterThanOrEqual(3);
    copy.close();
  });

  it('errors ring keeps the last 20', async () => {
    const ring = new ErrorRing();
    for (let i = 0; i < 250; i++) ring.push(`e${i}`);
    expect(ring.last(500).length).toBe(200);
    expect(ring.last(20).map((e) => e.msg)).toEqual(Array.from({ length: 20 }, (_, i) => `e${230 + i}`));
    for (let i = 0; i < 25; i++) errors.push(`boom ${i}`);
    const r = (await priv.inject({ url: '/status', headers: H })).json();
    expect(r.errors.length).toBe(20);
    expect(r.errors[19].msg).toBe('boom 24');
  });
});
