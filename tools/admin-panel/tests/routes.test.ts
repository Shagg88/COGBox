import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { FastifyInstance } from 'fastify';
import { buildApp } from '../src/app.js';
import { openDb, type Db } from '../src/db.js';
import { addUser } from '../src/users.js';
import { totpCode } from '../src/crypto.js';
import type { DockerClient, FabModDetail, FabModRow, FabReportGroup, FabStats, Lobby, MasterClient, ServiceHealth, Status } from '../src/types.js';

const up: ServiceHealth = { up: true, started_at: 'x', restarts: 0 };
const STATUS: Status = { uptime_s: 5, version: '1', players: { total: 0, by_title: {}, by_region: {} }, lobbies: 1, pending_invites: 0, errors: [] };
const LOBBY: Lobby = { lobby_id: 'L1', room_code: 'ABCD', title_id: 't', host_tag: 'h', is_public: true, created_at: 1 };

let db: Db, app: FastifyInstance, clock: number, secret: string, password: string;
let statusImpl: () => Promise<Status | null>;
let restartImpl: (s: string) => Promise<void>;
let restarts: string[], closed: string[], logCalls: [string, number][];
let fabCalls: unknown[][];
const FAB_STATS: FabStats = { disk: { total: 100, free: 40 }, month_bytes: 5, month_bytes_out: 9, mods: 2, creators: 1, open_reports: 1 };
const FAB_MODS: FabModRow[] = [{ id: 7, name: 'Alpha', game: 'gears3', author: 'Baird#1234', state: 'live', versions: 1, open_reports: 1, publishing: true, created_at: 1 }];
const FAB_MOD: FabModDetail = { id: 7, name: 'Alpha', game: 'gears3', type: 'other', author: 'Baird#1234', state: 'live', publishing: true,
  created_at: 1, updated_at: 1, screens: 1, versions: [], reports: [] };
const FAB_REPORTS: FabReportGroup[] = [{ mod: { id: 7, name: 'Alpha', game: 'gears3', author: 'Baird#1234', state: 'live', publishing: true },
  reports: [{ id: 1, reason: 'broken', text: 'crashes', reporter: 'Cole#0001', created_at: 1 }] }];

let logLines = ['line'];
const master: MasterClient = {
  status: () => statusImpl(),
  lobbies: async () => [LOBBY],
  closeLobby: async (id) => { closed.push(id); return true; },
  backup: async () => ({ name: 'master-20261111000000.db', size: 10 }),
  backups: async () => [{ name: 'master-20261111000000.db', size: 10, mtime: 1 }],
  fabReports: async () => FAB_REPORTS,
  fabStats: async () => FAB_STATS,
  fabMods: async (f) => { fabCalls.push(['mods', f]); return FAB_MODS; },
  fabMod: async (id) => (id === 7 ? FAB_MOD : null),
  fabAction: async (id, action, who) => { fabCalls.push([id, action, who]); return id === 9 ? 'no files' : id === 7; },
  fabPublishing: async (tag, allowed, who) => { fabCalls.push([tag, allowed, who]); return tag === 'Baird#1234'; },
};
const docker: DockerClient = {
  health: async () => ({ 'master-server': up, coturn: up, caddy: up }),
  logs: async (s, n) => { logCalls.push([s, n]); return logLines; },
  restart: async (s) => { await restartImpl(s); restarts.push(s); },
};

beforeEach(async () => {
  db = openDb(':memory:');
  const c = await addUser(db, 'alice');
  secret = new URL(c.otpauth).searchParams.get('secret')!;
  password = c.password;
  db.prepare('UPDATE users SET must_change = 0').run();
  clock = 1_800_000_000_000;
  statusImpl = async () => STATUS;
  restartImpl = async () => {};
  restarts = []; closed = []; logCalls = []; fabCalls = [];
  app = await buildApp({ db, now: () => clock, master, docker, secureCookie: false });
});
afterEach(async () => { await app.close(); db.close(); });

async function signIn() {
  // a fresh TOTP step each time: a code is refused if already used
  clock += 30_000;
  const code = totpCode(secret, Math.floor(clock / 30_000));
  const res = await app.inject({ method: 'POST', url: '/admin/api/login', payload: { name: 'alice', password, code } });
  const ck = res.cookies.find((x) => x.name === 'cogbox_admin')!;
  const cookie = `cogbox_admin=${ck.value}`;
  const me = await app.inject({ method: 'GET', url: '/admin/api/me', headers: { cookie } });
  return { cookie, csrf: me.json().csrf as string };
}
const post = async (url: string, payload?: object) => {
  const { cookie, csrf } = await signIn();
  return app.inject({ method: 'POST', url, headers: { cookie, 'x-csrf-token': csrf }, payload });
};
const get = async (url: string) => {
  const { cookie } = await signIn();
  return app.inject({ method: 'GET', url, headers: { cookie } });
};
const rows = () => db.prepare("SELECT action, detail FROM audit WHERE action NOT IN ('login','login_failed') ORDER BY id").all();

describe('routes', () => {
  it('overview reports master down when the private listener times out', async () => {
    statusImpl = () => new Promise((r) => setTimeout(() => r(null), 2000));
    const t = Date.now();
    const res = await get('/admin/api/overview');
    expect(Date.now() - t).toBeLessThan(3000);
    expect(res.statusCode).toBe(200);
    expect(res.json().status).toBeNull();
    expect(res.json().health.coturn.up).toBe(true);
    expect(res.json().last_action).toBeNull();
  });

  it('overview survives a docker failure and shows the last action', async () => {
    await post('/admin/api/backup');
    const orig = docker.health;
    docker.health = async () => { throw new Error('no socket'); };
    const res = await get('/admin/api/overview');
    docker.health = orig;
    expect(res.statusCode).toBe(200);
    expect(res.json().health).toBeNull();
    expect(res.json().status.version).toBe('1');
    expect(res.json().last_action.action).toBe('backup');
  });

  it('restart needs RESTART', async () => {
    for (const body of [{ service: 'coturn' }, { service: 'coturn', confirm: 'restart' }, { service: 'coturn', confirm: true }]) {
      expect((await post('/admin/api/restart', body)).statusCode).toBe(400);
    }
    expect(restarts).toEqual([]);
    expect(rows()).toEqual([]);
  });

  it('restart of caddy is 400', async () => {
    expect((await post('/admin/api/restart', { service: 'caddy', confirm: 'RESTART' })).statusCode).toBe(400);
    expect(restarts).toEqual([]);
  });

  it('two restarts both audited', async () => {
    expect((await post('/admin/api/restart', { service: 'master-server', confirm: 'RESTART' })).statusCode).toBe(200);
    expect((await post('/admin/api/restart', { service: 'coturn', confirm: 'RESTART' })).statusCode).toBe(200);
    expect(rows()).toEqual([{ action: 'restart', detail: 'master-server' }, { action: 'restart', detail: 'coturn' }]);
  });

  it('docker failure is 502 and audited', async () => {
    restartImpl = async () => { throw new Error('boom'); };
    const res = await post('/admin/api/restart', { service: 'coturn', confirm: 'RESTART' });
    expect(res.statusCode).toBe(502);
    expect(res.json().error).toContain('boom');
    const r = rows() as { action: string; detail: string }[];
    expect(r).toHaveLength(1);
    expect(r[0].action).toBe('restart');
    expect(r[0].detail).toContain('coturn');
    expect(r[0].detail).toContain('boom');
  });

  it('close lobby audited with room code', async () => {
    expect((await post('/admin/api/lobbies/L1/close', {})).statusCode).toBe(400);
    const res = await post('/admin/api/lobbies/L1/close', { confirm: true });
    expect(res.statusCode).toBe(200);
    expect(closed).toEqual(['L1']);
    expect(rows()).toEqual([{ action: 'lobby_close', detail: 'ABCD' }]);
    expect((await post('/admin/api/lobbies/nope/close', { confirm: true })).statusCode).toBe(404);
    expect(rows()).toHaveLength(1);
  });

  it('logs rejects unknown service', async () => {
    expect((await get('/admin/api/logs?service=caddy')).statusCode).toBe(400);
    expect((await get('/admin/api/logs')).statusCode).toBe(400);
    const res = await get('/admin/api/logs?service=coturn');
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({ lines: ['line'] });
    expect(logCalls).toEqual([['coturn', 300]]);
  });

  it('logs come back scrubbed', async () => {
    logLines = [
      'session: 001000000000000001: new connection from 203.0.113.7:51234',
      'error for 2f27a1b4-0c3d-4e5f-8a9b-1c2d3e4f5a6b on /ws/signal?token=abc123',
    ];
    try {
      for (const s of ['coturn', 'master-server']) {
        const lines = (await get(`/admin/api/logs?service=${s}`)).json().lines as string[];
        expect(lines.join(' ')).not.toMatch(/203\.0\.113|51234|2f27a1b4|abc123/);
        expect(lines[0]).toContain('<ip>');
        expect(lines[1]).toContain('<uuid>');
        expect(lines[1]).toContain('token=REDACTED');
      }
    } finally { logLines = ['line']; }
  });

  it('lobbies, backups, backup and audit', async () => {
    expect((await get('/admin/api/lobbies')).json()).toEqual([LOBBY]);
    expect((await get('/admin/api/backups')).json()).toHaveLength(1);
    const b = await post('/admin/api/backup');
    expect(b.json().name).toBe('master-20261111000000.db');
    await post('/admin/api/restart', { service: 'coturn', confirm: 'RESTART' });
    const a = (await get('/admin/api/audit')).json() as { id: number; action: string }[];
    expect(a[0].id).toBeGreaterThan(a[a.length - 1].id);
    expect(a.map((x) => x.action)).toContain('backup');
  });

  it('every route 401 without session', async () => {
    const cases: [string, string][] = [
      ['GET', '/admin/api/overview'], ['GET', '/admin/api/lobbies'], ['POST', '/admin/api/lobbies/L1/close'],
      ['GET', '/admin/api/logs?service=coturn'], ['POST', '/admin/api/restart'], ['POST', '/admin/api/backup'],
      ['GET', '/admin/api/backups'], ['GET', '/admin/api/audit'],
      ['GET', '/admin/api/fabricator/reports'], ['GET', '/admin/api/fabricator/stats'],
      ['GET', '/admin/api/fabricator/mods'], ['GET', '/admin/api/fabricator/mods/7'],
      ['POST', '/admin/api/fabricator/mods/7/takedown'], ['POST', '/admin/api/fabricator/creators/Baird%231234/publishing'],
    ];
    for (const [method, url] of cases) {
      const res = await app.inject({ method: method as 'GET' | 'POST', url });
      expect([url, res.statusCode]).toEqual([url, 401]);
    }
  });
});

describe('fabricator routes', () => {
  it('reports and stats come from the master', async () => {
    expect((await get('/admin/api/fabricator/reports')).json()).toEqual(FAB_REPORTS);
    expect((await get('/admin/api/fabricator/stats')).json()).toEqual(FAB_STATS);
  });

  it('lists mods by state and name, and shows one mod', async () => {
    expect((await get('/admin/api/fabricator/mods?state=taken_down&q=alp')).json()).toEqual(FAB_MODS);
    expect((await get('/admin/api/fabricator/mods')).json()).toEqual(FAB_MODS);
    expect((await get('/admin/api/fabricator/mods?state=bogus')).statusCode).toBe(400);
    expect(fabCalls).toEqual([['mods', { state: 'taken_down', q: 'alp' }], ['mods', { state: undefined, q: '' }]]);
    expect((await get('/admin/api/fabricator/mods/7')).json()).toEqual(FAB_MOD);
    expect((await get('/admin/api/fabricator/mods/8')).statusCode).toBe(404);
    expect((await get('/admin/api/fabricator/mods/abc')).statusCode).toBe(404);
  });

  it('mod actions need confirm, pass the signed-in admin as who and are audited', async () => {
    expect((await post('/admin/api/fabricator/mods/7/takedown', {})).statusCode).toBe(400);
    expect((await post('/admin/api/fabricator/mods/7/explode', { confirm: true })).statusCode).toBe(400);
    expect(fabCalls).toEqual([]);
    for (const a of ['takedown', 'restore', 'delete-files']) {
      expect((await post(`/admin/api/fabricator/mods/7/${a}`, { confirm: true })).statusCode).toBe(200);
    }
    expect(fabCalls).toEqual([[7, 'takedown', 'alice'], [7, 'restore', 'alice'], [7, 'delete-files', 'alice']]);
    expect(rows()).toEqual([
      { action: 'fab_takedown', detail: 'mod 7' }, { action: 'fab_restore', detail: 'mod 7' }, { action: 'fab_delete_files', detail: 'mod 7' },
    ]);
    expect((await post('/admin/api/fabricator/mods/8/takedown', { confirm: true })).statusCode).toBe(404);
    expect(rows()).toHaveLength(3);
  });

  it('restoring a mod with no files left is 409 with the reason and not audited', async () => {
    const r = await post('/admin/api/fabricator/mods/9/restore', { confirm: true });
    expect(r.statusCode).toBe(409);
    expect(r.json().error).toMatch(/no files/i);
    expect(rows()).toEqual([]);
  });

  it('publishing takes a tag and is audited', async () => {
    expect((await post('/admin/api/fabricator/creators/Baird%231234/publishing', { confirm: true })).statusCode).toBe(400);
    const r = await post('/admin/api/fabricator/creators/Baird%231234/publishing', { allowed: false, confirm: true });
    expect(r.statusCode).toBe(200);
    expect(fabCalls).toEqual([['Baird#1234', false, 'alice']]);
    expect(rows()).toEqual([{ action: 'fab_publishing', detail: 'Baird#1234 stopped' }]);
    expect((await post('/admin/api/fabricator/creators/Nobody%230000/publishing', { allowed: true, confirm: true })).statusCode).toBe(404);
  });
});

describe('pages', () => {
  it('serves the page with the CSP', async () => {
    const res = await app.inject({ method: 'GET', url: '/admin/' });
    expect(res.statusCode).toBe(200);
    expect(res.body).toContain('COGBox Admin');
    expect(res.headers['content-security-policy']).toBe("default-src 'self'; frame-ancestors 'none'");
  });
});

describe('default deny', () => {
  it('an unknown API path without a session gets 401', async () => {
    const res = await app.inject({ method: 'GET', url: '/admin/api/nope' });
    expect(res.statusCode).toBe(401);
  });
});

describe('encoded API paths', () => {
  for (const [method, url] of [['GET', '/admin/%61pi/audit'], ['GET', '/admin/api%2Faudit'], ['GET', '/admin/%61pi/overview'], ['POST', '/admin/%61pi/restart'], ['GET', '/admin/api%2F..%2Fapi%2Fme']] as const) {
    it(`${method} ${url} without a session is refused`, async () => {
      const res = await app.inject({ method, url, payload: method === 'POST' ? { service: 'coturn', confirm: 'RESTART' } : undefined });
      expect([401, 404]).toContain(res.statusCode);
      expect(res.body).not.toContain('COGBox Admin');
      expect(restarts).toEqual([]);
    });
  }
  it('signed in, an unknown API path is 404', async () => {
    expect((await get('/admin/api/nope')).statusCode).toBe(404);
  });
});
