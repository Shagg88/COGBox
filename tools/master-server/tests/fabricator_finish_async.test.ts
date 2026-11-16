import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import Database from 'better-sqlite3';
import type { FastifyInstance } from 'fastify';
import { buildServer } from '../src/server.js';
import { FabStore } from '../src/fabricator/store.js';
import { modZip, png, meta, sha256, awaitFinish } from './fabricator_helpers.js';

// The asynchronous finish (POST /finish 202, GET /uploads/:id for the outcome), the upload and picture rate limits,
// the download byte count and rating one's own mod, all through the routes.
const DAY = 24 * 3600 * 1000;
const API = '/api/v1/fabricator';
type Claimed = { uuid: string; token: string; discriminator: string };

describe('fabricator: asynchronous finish and route limits', () => {
  let dir: string;
  let dbPath: string;
  let app: FastifyInstance & { db: any };
  let clock: number;

  const start = (o: { rateLimitMax?: number; fabricator?: Record<string, number> } = {}) => buildServer({
    dbPath, rateLimitMax: o.rateLimitMax ?? 10000, now: () => clock,
    fabricator: { chunkBytes: 1024, creatorUploadsPerDay: 100, diskFullFraction: 1, ...(o.fabricator ?? {}) },
  }) as Promise<FastifyInstance & { db: any }>;
  const claim = async (display_name: string): Promise<Claimed> =>
    (await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name } })).json();
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });
  const status = (c: Claimed, id: string) => app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(c) });
  const finish = (c: Claimed, id: string) => app.inject({ method: 'POST', url: `${API}/uploads/${id}/finish`, headers: auth(c) });
  const putChunk = (c: Claimed, id: string, n: number, body: Buffer) => app.inject({
    method: 'PUT', url: `${API}/uploads/${id}/chunks/${n}`, payload: body,
    headers: { ...auth(c), 'content-type': 'application/octet-stream', 'x-chunk-sha256': sha256(body) },
  });
  const putScreen = async (c: Claimed, id: string) => app.inject({
    method: 'PUT', url: `${API}/uploads/${id}/screens/0`, payload: await png(), headers: { ...auth(c), 'content-type': 'application/octet-stream' },
  });
  // A session with every chunk and a screenshot, ready to finish.
  async function ready(c: Claimed, name = 'Horde Bank', bulk = 2300): Promise<{ id: string; zip: Buffer }> {
    const zip = await modZip(`[Mod]\nName=${name}\n`, bulk);
    const s = await app.inject({ method: 'POST', url: `${API}/uploads`, headers: auth(c), payload: { meta: meta({ name }), size: zip.length, sha256: sha256(zip) } });
    expect(s.statusCode).toBe(200);
    const { id, chunks } = s.json();
    for (let n = 0; n < chunks; n++) expect((await putChunk(c, id, n, zip.subarray(n * 1024, (n + 1) * 1024))).statusCode).toBe(204);
    expect((await putScreen(c, id)).statusCode).toBe(204);
    return { id, zip };
  }
  // Holds every finish inside assemble until release() is called.
  function holdFinish(): { release: () => void; entered: Promise<void> } {
    const orig = FabStore.prototype.assemble;
    let release!: () => void;
    let entered!: () => void;
    const gate = new Promise<void>((r) => { release = r; });
    const inside = new Promise<void>((r) => { entered = r; });
    vi.spyOn(FabStore.prototype, 'assemble').mockImplementation(async function (this: FabStore, ...a: [string, number]) {
      entered();
      await gate;
      return orig.apply(this, a);
    });
    return { release, entered: inside };
  }

  beforeEach(async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabasync-'));
    dbPath = path.join(dir, 'master.db');
    clock = Date.now() + 4 * DAY;
    app = await start();
  });
  afterEach(async () => {
    vi.restoreAllMocks();
    await app.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  it('answers 202 at once; while finishing a second finish is 409 finishing, screenshots and DELETE are 409', async () => {
    const me = await claim('Baird');
    const { id } = await ready(me);
    const hold = holdFinish();
    const f = await finish(me, id);
    expect(f.statusCode).toBe(202);
    expect(f.json()).toEqual({ state: 'finishing' });
    await hold.entered;
    expect((await status(me, id)).json()).toEqual({ state: 'finishing', chunks: 3, chunks_done: 3 });
    const again = await finish(me, id);
    expect(again.statusCode).toBe(409);
    expect(again.json()).toEqual({ error: 'finishing' });
    const shot = await putScreen(me, id);
    expect(shot.statusCode).toBe(409);
    expect((await app.inject({ method: 'DELETE', url: `${API}/uploads/${id}`, headers: auth(me) })).statusCode).toBe(409);
    hold.release();
    const done = await awaitFinish(async () => (await status(me, id)).json());
    expect(done).toEqual({ state: 'done', chunks: 3, chunks_done: 3, mod_id: expect.any(Number), version_id: expect.any(Number) });
    expect((await app.inject({ method: 'GET', url: `${API}/mods/${done.mod_id}` })).statusCode).toBe(200);
  });

  it('a done session counts once against the daily limit and not against the quota', async () => {
    const me = await claim('Baird');
    const { id, zip } = await ready(me);
    expect((await finish(me, id)).statusCode).toBe(202);
    expect((await awaitFinish(async () => (await status(me, id)).json())).state).toBe('done');
    const lim = (await app.inject({ method: 'GET', url: `${API}/creator/limits`, headers: auth(me) })).json();
    expect(lim.uploads_today).toBe(1);
    expect(lim.bytes_used).toBe(zip.length);
  });

  it('a done outcome stays readable until the session would have expired, then the session is gone', async () => {
    const me = await claim('Baird');
    const { id } = await ready(me);
    await finish(me, id);
    expect((await awaitFinish(async () => (await status(me, id)).json())).state).toBe('done');
    clock += DAY + 3600 * 1000 + 1;
    expect((await status(me, id)).statusCode).toBe(410);
  });

  it('a restart while finishing marks the session failed, with server restarted', async () => {
    const me = await claim('Baird');
    const { id } = await ready(me);
    await app.close();
    // what a crash leaves behind: the row says finishing, no finish runs any more
    const d = new Database(dbPath);
    d.prepare("UPDATE fab_uploads SET state = 'finishing' WHERE id = ?").run(id);
    d.close();
    app = await start();
    const s = (await status(me, id)).json();
    expect(s.state).toBe('failed');
    expect(s.problems.join()).toContain('server restarted');
    // the creator can finish again (or DELETE it)
    await finish(me, id);
    expect((await awaitFinish(async () => (await status(me, id)).json())).state).toBe('done');
  });

  it('a server error during the finish is a failed state, not a stuck one', async () => {
    const me = await claim('Baird');
    const { id } = await ready(me);
    vi.spyOn(FabStore.prototype, 'commit').mockImplementation(() => { throw new Error('disk gone'); });
    expect((await finish(me, id)).statusCode).toBe(202);
    const s = await awaitFinish(async () => (await status(me, id)).json());
    expect(s.state).toBe('failed');
    expect(s.problems.join()).toContain('server');
  });

  it('chunk and screenshot PUTs have their own per-account limit, other routes keep the shared one', async () => {
    await app.close();
    app = await start({ rateLimitMax: 4 });
    const me = await claim('Baird');
    const zip = await modZip('[Mod]\nName=Horde Bank\n', 9000);
    const s = await app.inject({ method: 'POST', url: `${API}/uploads`, headers: auth(me), payload: { meta: meta(), size: zip.length, sha256: sha256(zip) } });
    const { id, chunks } = s.json();
    expect(chunks).toBeGreaterThan(8);
    for (let n = 0; n < chunks; n++) expect((await putChunk(me, id, n, zip.subarray(n * 1024, (n + 1) * 1024))).statusCode).toBe(204);
    for (let i = 0; i < 6; i++) expect((await putScreen(me, id)).statusCode).toBe(204);
    let limited = 0;
    for (let i = 0; i < 6; i++) if ((await app.inject({ method: 'GET', url: `${API}/creator/limits`, headers: auth(me) })).statusCode === 429) limited++;
    expect(limited).toBeGreaterThan(0);
  });

  it('the upload limit answers 429 with Retry-After', async () => {
    await app.close();
    app = await start({ fabricator: { uploadRequestsPerMinute: 2 } });
    const me = await claim('Baird');
    const zip = await modZip('[Mod]\nName=Horde Bank\n', 2300);
    const { id } = (await app.inject({ method: 'POST', url: `${API}/uploads`, headers: auth(me), payload: { meta: meta(), size: zip.length, sha256: sha256(zip) } })).json();
    expect((await putChunk(me, id, 0, zip.subarray(0, 1024))).statusCode).toBe(204);
    expect((await putChunk(me, id, 1, zip.subarray(1024, 2048))).statusCode).toBe(204);
    const r = await putChunk(me, id, 2, zip.subarray(2048, 3072));
    expect(r.statusCode).toBe(429);
    expect(Number(r.headers['retry-after'])).toBeGreaterThan(0);
  });

  it('store pictures have a generous per-address limit of their own', async () => {
    await app.close();
    app = await start({ rateLimitMax: 3 });
    for (let i = 0; i < 10; i++) {
      expect((await app.inject({ method: 'GET', url: `${API}/images/${'0'.repeat(64)}.png` })).statusCode).toBe(404);
    }
    let limited = 0;
    for (let i = 0; i < 5; i++) if ((await app.inject({ method: 'GET', url: `${API}/mods` })).statusCode === 429) limited++;
    expect(limited).toBeGreaterThan(0);
  });

  it('counts the bytes downloads really send, per UTC day', async () => {
    const me = await claim('Baird');
    const player = await claim('Cole');
    const { id, zip } = await ready(me);
    await finish(me, id);
    const { mod_id } = await awaitFinish(async () => (await status(me, id)).json());
    const grant = (await app.inject({ method: 'POST', url: `${API}/mods/${mod_id}/download`, headers: auth(player) })).json();
    expect((await app.inject({ method: 'GET', url: grant.url })).statusCode).toBe(200);
    expect((await app.inject({ method: 'GET', url: grant.url, headers: { range: 'bytes=0-99' } })).statusCode).toBe(206);
    const day = new Date(clock).toISOString().slice(0, 10);
    await vi.waitFor(() => {
      expect(app.db.prepare('SELECT bytes FROM fab_traffic WHERE day = ?').get(day)).toEqual({ bytes: zip.length + 100 });
    });
  });

  it("a creator cannot rate their own mod", async () => {
    const me = await claim('Baird');
    const { id } = await ready(me);
    await finish(me, id);
    const { mod_id } = await awaitFinish(async () => (await status(me, id)).json());
    await app.inject({ method: 'POST', url: `${API}/library/${mod_id}`, headers: auth(me) });
    const r = await app.inject({ method: 'POST', url: `${API}/mods/${mod_id}/rating`, headers: auth(me), payload: { stars: 5 } });
    expect(r.statusCode).toBe(403);
    expect(r.json()).toEqual({ error: 'own mod' });
  });
});
