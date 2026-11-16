import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import type { FastifyInstance } from 'fastify';
import { buildServer } from '../src/server.js';
import { buildPrivateApi } from '../src/admin/private_api.js';
import { ErrorRing } from '../src/admin/error_ring.js';
import { reportsQueue, takeDown, restore, deleteFiles, setPublishing, fabStats, adminMods, adminModDetail, addTraffic } from '../src/fabricator/moderation.js';
import { forgetFabricatorAccount } from '../src/fabricator/publish.js';
import { modZip, png, meta, sha256, awaitFinish } from './fabricator_helpers.js';

const DAY = 24 * 3600 * 1000;
const API = '/api/v1/fabricator';
const SECRET = 'test-secret-0123456789';
const H = { 'x-admin-secret': SECRET };
type Claimed = { uuid: string; token: string; xuid: string; discriminator: string };

describe('fabricator moderation', () => {
  let dir: string;
  let app: FastifyInstance & { db: any; adminDeps: any };
  let priv: FastifyInstance;
  let clock: number;

  const claim = async (display_name: string): Promise<Claimed> =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name } })).json();
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });

  async function publish(c: Claimed, name: string, file: Buffer, modId?: number): Promise<{ mod_id: number; version_id: number }> {
    const payload: any = { meta: meta({ name }), size: file.length, sha256: sha256(file) };
    if (modId !== undefined) payload.mod_id = modId;
    const s = await app.inject({ method: 'POST', url: `${API}/uploads`, headers: auth(c), payload });
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
    expect(f.statusCode).toBe(202);
    const st = await awaitFinish(async () => (await app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(c) })).json());
    expect(st.state).toBe('done');
    return { mod_id: st.mod_id, version_id: st.version_id };
  }
  const report = (modId: number, reporter: string, reason: string, text: string, at: number) =>
    app.db.prepare('INSERT INTO fab_reports (mod_id, reporter, reason, text, created_at) VALUES (?, ?, ?, ?, ?)').run(modId, reporter, reason, text, at);
  const log = () => app.db.prepare('SELECT who, action, mod_id FROM fab_admin_log ORDER BY id').all();
  const state = (id: number) => (app.db.prepare('SELECT state FROM fab_mods WHERE id = ?').get(id) as { state: string }).state;

  beforeEach(async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabmod-'));
    clock = Date.now() + 4 * DAY;
    app = await buildServer({ dbPath: path.join(dir, 'master.db'), rateLimitMax: 10000, now: () => clock,
      fabricator: { chunkBytes: 1024, diskFullFraction: 1 } }) as any;
    priv = await buildPrivateApi({ ...app.adminDeps, errors: new ErrorRing(), secret: SECRET, dataDir: dir });
  });
  afterEach(async () => { await priv.close(); await app.close(); fs.rmSync(dir, { recursive: true, force: true }); });

  it('take down then restore round-trips the state, logs both and handles the open reports', async () => {
    const a = await claim('Baird');
    const r = await claim('Cole');
    const { mod_id } = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 900));
    report(mod_id, r.uuid, 'broken', 'crashes', clock);
    const q = reportsQueue(app.db);
    expect(q).toHaveLength(1);
    expect(q[0].mod).toEqual({ id: mod_id, name: 'Alpha', game: 'gears3', author: `Baird#${a.discriminator}`, state: 'live', publishing: true });
    expect(q[0].reports[0]).toMatchObject({ reason: 'broken', text: 'crashes', reporter: `Cole#${r.discriminator}` });

    expect(takeDown(app.db, mod_id, 'alice', clock)).toBe(true);
    expect(state(mod_id)).toBe('taken_down');
    expect((await app.inject({ method: 'GET', url: `${API}/mods/${mod_id}` })).json().state).toBe('taken_down');
    expect(reportsQueue(app.db)).toEqual([]);
    expect(app.db.prepare('SELECT handled_by, outcome FROM fab_reports').get()).toEqual({ handled_by: 'alice', outcome: 'taken_down' });
    expect(takeDown(app.db, mod_id, 'alice', clock)).toBe(false);

    expect(restore(app.db, mod_id, 'bob', clock)).toBe(true);
    expect(state(mod_id)).toBe('live');
    expect(restore(app.db, mod_id, 'bob', clock)).toBe(false);
    const list = (await app.inject({ method: 'GET', url: `${API}/mods?search=Alpha` })).json();
    expect(list.mods.map((m: any) => m.id)).toEqual([mod_id]);
    expect(log()).toEqual([
      { who: 'alice', action: 'takedown', mod_id },
      { who: 'bob', action: 'restore', mod_id },
    ]);
  });

  it('delete-files removes the zips, marks versions files_deleted and gives the quota back', async () => {
    const a = await claim('Baird');
    const zip = await modZip('[Mod]\nName=A\n', 900);
    const { mod_id } = await publish(a, 'Alpha', zip);
    // another mod by someone else with the very same bytes keeps the shared file
    const b = await claim('Dom');
    const other = await publish(b, 'Beta', zip);
    const key = (app.db.prepare('SELECT file_key FROM fab_versions WHERE mod_id = ?').get(mod_id) as { file_key: string }).file_key;
    const file = app.adminDeps.store.pathOf(key);
    expect(fs.existsSync(file)).toBe(true);

    expect(deleteFiles(app.db, app.adminDeps.store, mod_id, 'alice', clock)).toBe(true);
    expect(app.db.prepare('SELECT state FROM fab_versions WHERE mod_id = ?').all(mod_id)).toEqual([{ state: 'files_deleted' }]);
    expect((app.db.prepare('SELECT bytes_used FROM fab_creators WHERE account = ?').get(a.uuid) as any).bytes_used).toBe(0);
    expect(fs.existsSync(file)).toBe(true);   // still Beta's

    expect(deleteFiles(app.db, app.adminDeps.store, other.mod_id, 'alice', clock)).toBe(true);
    expect(fs.existsSync(file)).toBe(false);
    expect(deleteFiles(app.db, app.adminDeps.store, 9999, 'alice', clock)).toBe(false);
    expect(log()).toEqual([
      { who: 'alice', action: 'delete-files', mod_id },
      { who: 'alice', action: 'delete-files', mod_id: other.mod_id },
    ]);
  });

  it('deleting the files of a live mod takes it down and hides it from listings', async () => {
    const a = await claim('Baird');
    const { mod_id } = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 900));
    expect((await app.inject({ method: 'GET', url: `${API}/mods` })).json().mods.map((m: any) => m.id)).toEqual([mod_id]);
    expect(deleteFiles(app.db, app.adminDeps.store, mod_id, 'alice', clock)).toBe(true);
    expect(state(mod_id)).toBe('taken_down');
    expect((await app.inject({ method: 'GET', url: `${API}/mods` })).json().mods).toEqual([]);
    expect(log()).toEqual([{ who: 'alice', action: 'delete-files', mod_id }]);
  });

  it('restore refuses a mod with no live version (409 from the private API)', async () => {
    const a = await claim('Baird');
    const { mod_id } = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 900));
    expect(takeDown(app.db, mod_id, 'alice', clock)).toBe(true);
    expect(deleteFiles(app.db, app.adminDeps.store, mod_id, 'alice', clock)).toBe(true);
    expect(restore(app.db, mod_id, 'alice', clock)).toBe('no files');
    const r = await priv.inject({ method: 'POST', url: `/fabricator/mods/${mod_id}/restore`, headers: H, payload: { who: 'alice' } });
    expect(r.statusCode).toBe(409);
    expect(r.json()).toEqual({ error: 'no files' });
    expect(state(mod_id)).toBe('taken_down');
    expect(log().map((l: any) => l.action)).toEqual(['takedown', 'delete-files']);
  });

  it('reports on a mod its owner removes, or whose account is deleted, are closed', async () => {
    const a = await claim('Baird');
    const r = await claim('Cole');
    const one = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 900));
    const two = await publish(a, 'Bravo', await modZip('[Mod]\nName=B\n', 900));
    report(one.mod_id, r.uuid, 'broken', '', clock);
    report(two.mod_id, r.uuid, 'broken', '', clock);
    expect((await app.inject({ method: 'DELETE', url: `${API}/creator/mods/${one.mod_id}`, headers: auth(a) })).statusCode).toBeLessThan(300);
    expect(reportsQueue(app.db).map((e) => e.mod.id)).toEqual([two.mod_id]);
    expect(app.db.prepare('SELECT handled_by, outcome FROM fab_reports WHERE mod_id = ?').get(one.mod_id)).toEqual({ handled_by: 'owner', outcome: 'removed' });
    forgetFabricatorAccount(app.db, app.adminDeps.store, a.uuid);
    expect(reportsQueue(app.db)).toEqual([]);
    expect((await fabStats(app.db, app.adminDeps.store, clock)).open_reports).toBe(0);
  });

  it('old open reports on removed mods are not in the queue or the stat', async () => {
    const a = await claim('Baird');
    const r = await claim('Cole');
    const { mod_id } = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 900));
    app.db.prepare("UPDATE fab_mods SET state = 'removed' WHERE id = ?").run(mod_id);
    report(mod_id, r.uuid, 'broken', '', clock);
    expect(reportsQueue(app.db)).toEqual([]);
    expect((await fabStats(app.db, app.adminDeps.store, clock)).open_reports).toBe(0);
  });

  it('setPublishing resolves a tag; an unknown tag is false', async () => {
    const a = await claim('Baird');
    expect(setPublishing(app.db, 'Nobody#0000', false, 'alice', clock)).toBe(false);
    expect(setPublishing(app.db, 'no-hash', false, 'alice', clock)).toBe(false);
    expect(setPublishing(app.db, `Baird#${a.discriminator}`, false, 'alice', clock)).toBe(true);
    expect((app.db.prepare('SELECT publishing FROM fab_creators WHERE account = ?').get(a.uuid) as any).publishing).toBe(0);
    expect(setPublishing(app.db, `Baird#${a.discriminator}`, true, 'alice', clock)).toBe(true);
    expect((app.db.prepare('SELECT publishing FROM fab_creators WHERE account = ?').get(a.uuid) as any).publishing).toBe(1);
    const l = app.db.prepare('SELECT who, action, mod_id, detail FROM fab_admin_log ORDER BY id').all() as any[];
    expect(l.map((x) => x.action)).toEqual(['publishing', 'publishing']);
    expect(JSON.stringify(l)).not.toContain(a.uuid);
  });

  it('fabStats counts mods, creators, open reports and this month', async () => {
    const a = await claim('Baird');
    const r = await claim('Cole');
    const zip = await modZip('[Mod]\nName=A\n', 900);
    const { mod_id } = await publish(a, 'Alpha', zip);
    report(mod_id, r.uuid, 'other', '', clock);
    const s = await fabStats(app.db, app.adminDeps.store, clock);
    expect(s.disk.total).toBeGreaterThan(0);
    expect(s).toMatchObject({ month_bytes: zip.length, mods: 1, creators: 1, open_reports: 1 });
    expect((await fabStats(app.db, app.adminDeps.store, clock + 31 * DAY)).month_bytes).toBe(0);
  });

  it('fabStats counts the bytes downloads sent in the last 30 days', async () => {
    addTraffic(app.db, clock, 1000);
    addTraffic(app.db, clock, 24);
    addTraffic(app.db, clock - 29 * DAY, 500);
    addTraffic(app.db, clock - 31 * DAY, 7);
    expect((await fabStats(app.db, app.adminDeps.store, clock)).month_bytes_out).toBe(1524);
  });

  it('lists mods by state and name, newest first, with their versions, screens and reports', async () => {
    const a = await claim('Baird');
    const r = await claim('Cole');
    const zip = await modZip('[Mod]\nName=A\n', 900);
    const one = await publish(a, 'Alpha', zip);
    clock += 1000;
    const two = await publish(a, 'Alpha Two', await modZip('[Mod]\nName=A2\n', 900));
    clock += 1000;
    const three = await publish(r, 'Bravo_100%', await modZip('[Mod]\nName=B\n', 900));
    report(one.mod_id, r.uuid, 'broken', '<i>x</i>', clock);
    takeDown(app.db, two.mod_id, 'alice', clock);
    setPublishing(app.db, `Baird#${a.discriminator}`, false, 'alice', clock);

    expect(adminMods(app.db, {}).map((m) => m.id)).toEqual([three.mod_id, two.mod_id, one.mod_id]);
    expect(adminMods(app.db, { state: 'taken_down' }).map((m) => m.id)).toEqual([two.mod_id]);
    expect(adminMods(app.db, { q: 'ALPHA' }).map((m) => m.id)).toEqual([two.mod_id, one.mod_id]);
    expect(adminMods(app.db, { q: '100%' }).map((m) => m.id)).toEqual([three.mod_id]);
    expect(adminMods(app.db, { q: '%' }).map((m) => m.id)).toEqual([three.mod_id]);   // wildcards are plain text
    expect(adminMods(app.db, { q: 'r_v' })).toEqual([]);
    expect(adminMods(app.db, { q: String(one.mod_id), state: 'live' }).map((m) => m.id)).toContain(one.mod_id);
    expect(adminMods(app.db, { q: 'alpha' })[1]).toEqual({ id: one.mod_id, name: 'Alpha', game: 'gears3', author: `Baird#${a.discriminator}`,
      state: 'live', versions: 1, open_reports: 1, publishing: false, created_at: expect.any(Number) });

    const d = adminModDetail(app.db, one.mod_id);
    expect(d).toMatchObject({ id: one.mod_id, name: 'Alpha', game: 'gears3', type: 'gameplay', author: `Baird#${a.discriminator}`, state: 'live',
      publishing: false, screens: 1 });
    expect(d?.versions).toEqual([{ id: one.version_id, version: '1.0', size: zip.length, sha256: sha256(zip), state: 'live', created_at: expect.any(Number) }]);
    expect(d?.reports).toEqual([{ id: expect.any(Number), reason: 'broken', text: '<i>x</i>', reporter: `Cole#${r.discriminator}`,
      created_at: clock, handled_by: null, outcome: null }]);
    expect(adminModDetail(app.db, 9999)).toBeNull();

    const list = await priv.inject({ url: '/fabricator/mods?state=taken_down', headers: H });
    expect(list.statusCode).toBe(200);
    expect(list.json().mods.map((m: any) => m.id)).toEqual([two.mod_id]);
    expect((await priv.inject({ url: '/fabricator/mods?q=bravo', headers: H })).json().mods.map((m: any) => m.id)).toEqual([three.mod_id]);
    expect((await priv.inject({ url: '/fabricator/mods?state=gone', headers: H })).statusCode).toBe(400);
    const det = await priv.inject({ url: `/fabricator/mods/${one.mod_id}`, headers: H });
    expect(det.statusCode).toBe(200);
    expect(det.json().versions).toHaveLength(1);
    expect((await priv.inject({ url: '/fabricator/mods/9999', headers: H })).statusCode).toBe(404);
    expect((await priv.inject({ url: '/fabricator/mods/abc', headers: H })).statusCode).toBe(404);
    for (const body of [list.body, det.body]) for (const x of [a.uuid, a.token, r.uuid, r.token]) expect(body).not.toContain(x);
    for (const url of ['/fabricator/mods', `/fabricator/mods/${one.mod_id}`]) {
      expect((await priv.inject({ url, headers: { 'x-admin-secret': 'wrong' } })).statusCode).toBe(401);
    }
  });

  it('private API refuses a wrong secret', async () => {
    for (const [method, url] of [['GET', '/fabricator/reports'], ['GET', '/fabricator/stats'], ['POST', '/fabricator/mods/1/takedown'],
      ['POST', '/fabricator/mods/1/restore'], ['POST', '/fabricator/mods/1/delete-files'], ['POST', '/fabricator/creators/a%231/publishing']] as const) {
      expect((await priv.inject({ method, url })).statusCode).toBe(401);
      expect((await priv.inject({ method, url, headers: { 'x-admin-secret': 'wrong' } })).statusCode).toBe(401);
    }
  });

  it('private API answers with tags and never a uuid or token', async () => {
    const a = await claim('Baird');
    const r = await claim('Cole');
    const { mod_id } = await publish(a, 'Alpha', await modZip('[Mod]\nName=A\n', 900));
    report(mod_id, r.uuid, 'stolen', '<b>mine</b>', clock);
    const rep = await priv.inject({ url: '/fabricator/reports', headers: H });
    expect(rep.statusCode).toBe(200);
    expect(rep.json().reports[0].mod.author).toBe(`Baird#${a.discriminator}`);
    const st = await priv.inject({ url: '/fabricator/stats', headers: H });
    expect(st.json().open_reports).toBe(1);
    for (const body of [rep.body, st.body]) for (const s of [a.uuid, a.token, r.uuid, r.token, a.xuid]) expect(body).not.toContain(s);

    expect((await priv.inject({ method: 'POST', url: `/fabricator/mods/${mod_id}/takedown`, headers: H, payload: {} })).statusCode).toBe(400);
    expect((await priv.inject({ method: 'POST', url: `/fabricator/mods/${mod_id}/explode`, headers: H, payload: { who: 'alice' } })).statusCode).toBe(404);
    expect((await priv.inject({ method: 'POST', url: `/fabricator/mods/${mod_id}/takedown`, headers: H, payload: { who: 'alice' } })).json()).toEqual({ ok: true });
    expect(state(mod_id)).toBe('taken_down');
    expect((await priv.inject({ method: 'POST', url: `/fabricator/mods/${mod_id}/restore`, headers: H, payload: { who: 'alice' } })).json()).toEqual({ ok: true });
    expect((await priv.inject({ method: 'POST', url: `/fabricator/mods/${mod_id}/delete-files`, headers: H, payload: { who: 'alice' } })).json()).toEqual({ ok: true });
    expect((await priv.inject({ method: 'POST', url: '/fabricator/mods/9999/takedown', headers: H, payload: { who: 'alice' } })).json()).toEqual({ ok: false });

    const tag = encodeURIComponent(`Baird#${a.discriminator}`);
    expect((await priv.inject({ method: 'POST', url: `/fabricator/creators/${tag}/publishing`, headers: H, payload: { who: 'alice' } })).statusCode).toBe(400);
    expect((await priv.inject({ method: 'POST', url: `/fabricator/creators/${tag}/publishing`, headers: H, payload: { allowed: false, who: 'alice' } })).json()).toEqual({ ok: true });
    expect((await priv.inject({ method: 'POST', url: `/fabricator/creators/${encodeURIComponent('X#1')}/publishing`, headers: H, payload: { allowed: false, who: 'alice' } })).json()).toEqual({ ok: false });
  });
});
