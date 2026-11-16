import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import Database from 'better-sqlite3';
import type { FastifyInstance } from 'fastify';
import { buildServer } from '../src/server.js';
import { modZip, png, meta, sha256 } from './fabricator_helpers.js';

const DAY = 24 * 3600 * 1000;
const API = '/api/v1/fabricator';
type Claimed = { uuid: string; token: string };

describe('fabricator creator pages and deleted accounts', () => {
  let dir: string;
  let dbPath: string;
  let app: FastifyInstance;
  let clock: number;

  const claim = async (display_name: string): Promise<Claimed> =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name } })).json();
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });

  async function publish(c: Claimed, name: string, file: Buffer): Promise<{ mod_id: number; version_id: number }> {
    const s = await app.inject({ method: 'POST', url: `${API}/uploads`, headers: auth(c), payload: { meta: meta({ name }), size: file.length, sha256: sha256(file) } });
    expect(s.statusCode).toBe(200);
    const { id, chunks } = s.json();
    for (let n = 0; n < chunks; n++) {
      const body = file.subarray(n * 1024, (n + 1) * 1024);
      const r = await app.inject({ method: 'PUT', url: `${API}/uploads/${id}/chunks/${n}`, payload: body,
        headers: { ...auth(c), 'content-type': 'application/octet-stream', 'x-chunk-sha256': sha256(body) } });
      expect(r.statusCode).toBe(204);
    }
    await app.inject({ method: 'PUT', url: `${API}/uploads/${id}/screens/0`, payload: await png(), headers: { ...auth(c), 'content-type': 'application/octet-stream' } });
    const f = await app.inject({ method: 'POST', url: `${API}/uploads/${id}/finish`, headers: auth(c) });
    expect(f.statusCode).toBe(200);
    return f.json();
  }
  const rows = (q: string, ...a: unknown[]): any[] => { const d = new Database(dbPath); try { return d.prepare(q).all(...a); } finally { d.close(); } };
  const filesOnDisk = (): string[] => {
    const out: string[] = [];
    const walk = (p: string) => { for (const e of fs.readdirSync(p, { withFileTypes: true })) e.isDirectory() ? walk(path.join(p, e.name)) : out.push(path.join(p, e.name)); };
    walk(path.join(dir, 'fabricator', 'files'));
    return out;
  };

  beforeEach(async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabcr-'));
    dbPath = path.join(dir, 'master.db');
    clock = Date.now() + 4 * DAY;
    app = await buildServer({ dbPath, rateLimitMax: 10000, now: () => clock, fabricator: { chunkBytes: 1024, diskFullFraction: 1 } });
  });
  afterEach(async () => { await app.close(); fs.rmSync(dir, { recursive: true, force: true }); });

  it('needs Authorization', async () => {
    for (const [method, url] of [['GET', '/creator/mods'], ['DELETE', '/creator/mods/1'], ['GET', '/creator/limits']] as const) {
      expect((await app.inject({ method, url: API + url })).statusCode).toBe(401);
    }
  });

  it('lists the creators mods, removes one and gives the quota back', async () => {
    const a = await claim('Baird');
    const zip = await modZip('[Mod]\nName=A\n', 2300);
    const { mod_id } = await publish(a, 'Alpha', zip);
    const list = (await app.inject({ method: 'GET', url: `${API}/creator/mods`, headers: auth(a) })).json();
    expect(list.mods).toHaveLength(1);
    expect(list.mods[0]).toMatchObject({ id: mod_id, name: 'Alpha', game: 'gears3', state: 'live', downloads: 0, reports_open: 0 });
    expect(list.mods[0].versions[0]).toMatchObject({ version: '1.0', size: zip.length, state: 'live' });
    let lim = (await app.inject({ method: 'GET', url: `${API}/creator/limits`, headers: auth(a) })).json();
    expect(lim).toMatchObject({ can_publish: null, bytes_used: zip.length, uploads_today: 1, chunk_bytes: 1024 });
    expect(filesOnDisk()).toHaveLength(1);

    const del = await app.inject({ method: 'DELETE', url: `${API}/creator/mods/${mod_id}`, headers: auth(a) });
    expect(del.statusCode).toBe(204);
    expect(filesOnDisk()).toHaveLength(0);
    lim = (await app.inject({ method: 'GET', url: `${API}/creator/limits`, headers: auth(a) })).json();
    expect(lim.bytes_used).toBe(0);
    const after = (await app.inject({ method: 'GET', url: `${API}/creator/mods`, headers: auth(a) })).json();
    expect(after.mods[0].state).toBe('removed');
    expect((await app.inject({ method: 'GET', url: `${API}/mods/${mod_id}` })).statusCode).toBe(404);
    expect(rows('SELECT 1 FROM fab_mods_fts WHERE rowid = ?', mod_id)).toHaveLength(0);
    expect((await app.inject({ method: 'DELETE', url: `${API}/creator/mods/${mod_id}`, headers: auth(a) })).statusCode).toBe(404);
  });

  it('keeps a file another live mod still uses', async () => {
    const a = await claim('Baird');
    const zip = await modZip('[Mod]\nName=A\n', 2300);
    const one = await publish(a, 'Alpha', zip);
    await publish(a, 'Beta', zip);
    await app.inject({ method: 'DELETE', url: `${API}/creator/mods/${one.mod_id}`, headers: auth(a) });
    expect(filesOnDisk()).toHaveLength(1);
  });

  it('someone elses mod is 404', async () => {
    const a = await claim('Baird');
    const b = await claim('Cole');
    const { mod_id } = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 100));
    expect((await app.inject({ method: 'DELETE', url: `${API}/creator/mods/${mod_id}`, headers: auth(b) })).statusCode).toBe(404);
    expect(rows('SELECT state FROM fab_mods WHERE id = ?', mod_id)[0].state).toBe('live');
  });

  it('limits say account too new for a 1-day-old account', async () => {
    const a = await claim('Baird');
    clock -= 3 * DAY;
    const lim = (await app.inject({ method: 'GET', url: `${API}/creator/limits`, headers: auth(a) })).json();
    expect(lim.can_publish).toBe('account too new');
    expect(lim.max_uploads_per_day).toBe(3);
  });

  it('deleting an account removes its mods, files and fabricator rows but not other libraries', async () => {
    const a = await claim('Baird');
    const b = await claim('Cole');
    const { mod_id } = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 2300));
    const other = await publish(b, 'Gamma', await modZip('[Mod]\nName=G\n', 500));
    for (const [who, id] of [[a, other.mod_id], [b, mod_id]] as const) {
      await app.inject({ method: 'POST', url: `${API}/library/${id}`, headers: auth(who) });
      await app.inject({ method: 'POST', url: `${API}/mods/${id}/download`, headers: auth(who) });
      await app.inject({ method: 'POST', url: `${API}/mods/${id}/rating`, headers: auth(who), payload: { stars: 4 } });
      await app.inject({ method: 'POST', url: `${API}/mods/${id}/report`, headers: auth(who), payload: { reason: 'broken' } });
    }
    const open = await app.inject({ method: 'POST', url: `${API}/uploads`, headers: auth(a), payload: { meta: meta({ name: 'Open' }), size: 10, sha256: 'a'.repeat(64) } });
    expect(open.statusCode).toBe(200);
    expect(filesOnDisk()).toHaveLength(2);

    expect((await app.inject({ method: 'DELETE', url: '/api/v1/auth/account', headers: auth(a) })).statusCode).toBe(200);

    expect(rows('SELECT state FROM fab_mods WHERE id = ?', mod_id)[0].state).toBe('removed');
    expect(filesOnDisk()).toHaveLength(1);
    for (const t of ['fab_ratings', 'fab_reports', 'fab_library', 'fab_downloads']) {
      expect(rows(`SELECT 1 FROM ${t} WHERE ${t === 'fab_reports' ? 'reporter' : 'account'} = ?`, a.uuid), t).toHaveLength(0);
    }
    expect(rows('SELECT 1 FROM fab_creators WHERE account = ?', a.uuid)).toHaveLength(0);
    expect(rows('SELECT 1 FROM fab_uploads WHERE account = ?', a.uuid)).toHaveLength(0);
    expect(rows('SELECT 1 FROM fab_mods_fts WHERE rowid = ?', mod_id)).toHaveLength(0);
    const lib = (await app.inject({ method: 'GET', url: `${API}/library`, headers: auth(b) })).json();
    expect(lib.mods.find((m: any) => m.id === mod_id)).toMatchObject({ state: 'removed' });
    expect(rows('SELECT 1 FROM fab_ratings WHERE mod_id = ?', other.mod_id)).toHaveLength(0);
  });
});
