import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import Database from 'better-sqlite3';
import type { FastifyInstance } from 'fastify';
import { buildServer } from '../src/server.js';
import { modZip, png, meta, sha256, awaitFinish } from './fabricator_helpers.js';

const DAY = 24 * 3600 * 1000;
const MIN = 60_000;
const API = '/api/v1/fabricator';
type Claimed = { uuid: string; token: string };

describe('fabricator library and download routes', () => {
  let dir: string;
  let dbPath: string;
  let app: FastifyInstance;
  let clock: number;
  let zip: Buffer;
  let modId: number;
  let versionId: number;
  let player: Claimed;

  async function claim(display_name: string): Promise<Claimed> {
    const res = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name } });
    return res.json();
  }
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });

  // Publishes `zip` through the upload routes (1 KiB chunks) and returns its ids.
  async function publish(c: Claimed, file: Buffer): Promise<{ mod_id: number; version_id: number }> {
    const s = await app.inject({ method: 'POST', url: `${API}/uploads`, headers: auth(c), payload: { meta: meta(), size: file.length, sha256: sha256(file) } });
    expect(s.statusCode).toBe(200);
    const { id, chunks } = s.json();
    for (let n = 0; n < chunks; n++) {
      const body = file.subarray(n * 1024, (n + 1) * 1024);
      const r = await app.inject({
        method: 'PUT', url: `${API}/uploads/${id}/chunks/${n}`, payload: body,
        headers: { ...auth(c), 'content-type': 'application/octet-stream', 'x-chunk-sha256': sha256(body) },
      });
      expect(r.statusCode).toBe(204);
    }
    const sc = await app.inject({ method: 'PUT', url: `${API}/uploads/${id}/screens/0`, payload: await png(), headers: { ...auth(c), 'content-type': 'application/octet-stream' } });
    expect(sc.statusCode).toBe(204);
    const f = await app.inject({ method: 'POST', url: `${API}/uploads/${id}/finish`, headers: auth(c) });
    expect(f.statusCode).toBe(202);
    const st = await awaitFinish(async () => (await app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(c) })).json());
    expect(st.state).toBe('done');
    return { mod_id: st.mod_id, version_id: st.version_id };
  }
  const download = (c: Claimed, id: number | string = modId) => app.inject({ method: 'POST', url: `${API}/mods/${id}/download`, headers: auth(c) });
  const getFile = (url: string, range?: string) => app.inject({ method: 'GET', url, headers: range ? { range } : {} });
  const sql = (q: string, ...args: unknown[]) => { const d = new Database(dbPath); try { d.prepare(q).run(...args); } finally { d.close(); } };

  beforeEach(async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabdl-'));
    dbPath = path.join(dir, 'master.db');
    clock = Date.now() + 4 * DAY;   // accounts claimed now are old enough to publish
    app = await buildServer({ dbPath, rateLimitMax: 10000, now: () => clock, fabricator: { chunkBytes: 1024, diskFullFraction: 1 } });
    const creator = await claim('Baird');
    zip = await modZip('[Mod]\nName=Horde Bank\n', 2300);
    ({ mod_id: modId, version_id: versionId } = await publish(creator, zip));
    player = await claim('Cole');
  });
  afterEach(async () => {
    await app.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  it('needs Authorization for the library, downloads, ratings and reports', async () => {
    for (const [method, url] of [['GET', '/library'], ['POST', `/library/${1}`], ['DELETE', '/library/1'], ['POST', '/mods/1/download'],
      ['POST', '/mods/1/rating'], ['POST', '/mods/1/report']] as const) {
      const r = await app.inject({ method, url: API + url });
      expect(r.statusCode).toBe(401);
      expect(r.json()).toEqual({ error: 'Authorization: COGBox <uuid>:<token> required' });
    }
  });

  it('range requests', async () => {
    const d = await download(player);
    expect(d.statusCode).toBe(200);
    const { url, sha256: hash, size, version_id } = d.json();
    expect({ hash, size, version_id }).toEqual({ hash: sha256(zip), size: zip.length, version_id: versionId });
    expect(url).toMatch(/^\/api\/v1\/fabricator\/files\/[0-9a-f]{64}$/);

    const full = await getFile(url);
    expect(full.statusCode).toBe(200);
    expect(full.headers['content-type']).toBe('application/zip');
    expect(full.headers['accept-ranges']).toBe('bytes');
    expect(full.headers['content-disposition']).toBe('attachment; filename="mod.zip"');
    expect(Number(full.headers['content-length'])).toBe(zip.length);
    expect(full.rawPayload.equals(zip)).toBe(true);

    const from10 = await getFile(url, 'bytes=10-');
    expect(from10.statusCode).toBe(206);
    expect(from10.headers['content-range']).toBe(`bytes 10-${zip.length - 1}/${zip.length}`);
    expect(Number(from10.headers['content-length'])).toBe(zip.length - 10);
    expect(from10.rawPayload.equals(zip.subarray(10))).toBe(true);

    const mid = await getFile(url, 'bytes=5-9');
    expect(mid.statusCode).toBe(206);
    expect(mid.headers['content-range']).toBe(`bytes 5-9/${zip.length}`);
    expect(mid.rawPayload.equals(zip.subarray(5, 10))).toBe(true);

    const tail = await getFile(url, 'bytes=-4');
    expect(tail.statusCode).toBe(206);
    expect(tail.rawPayload.equals(zip.subarray(zip.length - 4))).toBe(true);

    for (const bad of [`bytes=${zip.length}-`, 'bytes=9-5', 'bytes=0-1,4-5', 'bytes=x-', 'items=0-1', 'bytes=-0']) {
      const r = await getFile(url, bad);
      expect(r.statusCode, bad).toBe(416);
      expect(r.headers['content-range']).toBe(`bytes */${zip.length}`);
    }

    expect((await getFile(`${API}/files/${'0'.repeat(64)}`)).statusCode).toBe(410);
    expect((await getFile(`${API}/files/not-a-token`)).statusCode).toBe(404);
    clock += 16 * MIN;
    expect((await getFile(url)).statusCode).toBe(410);
  });

  it('library add, list and remove; downloading adds to the library', async () => {
    expect((await app.inject({ method: 'GET', url: `${API}/library`, headers: auth(player) })).json()).toEqual({ mods: [] });
    expect((await app.inject({ method: 'POST', url: `${API}/library/${modId}`, headers: auth(player) })).statusCode).toBe(204);
    expect((await app.inject({ method: 'POST', url: `${API}/library/99999`, headers: auth(player) })).statusCode).toBe(404);
    const lib = (await app.inject({ method: 'GET', url: `${API}/library`, headers: auth(player) })).json();
    expect(lib.mods).toEqual([expect.objectContaining({
      id: modId, name: 'Horde Bank', state: 'live', version: '1.0', version_id: versionId, sha256: sha256(zip), size: zip.length,
      screen: expect.stringMatching(/^\/api\/v1\/fabricator\/images\/[0-9a-f]{64}\.png$/),
    })]);
    expect((await app.inject({ method: 'DELETE', url: `${API}/library/${modId}`, headers: auth(player) })).statusCode).toBe(204);
    expect((await app.inject({ method: 'GET', url: `${API}/library`, headers: auth(player) })).json()).toEqual({ mods: [] });
    await download(player);
    expect((await app.inject({ method: 'GET', url: `${API}/library`, headers: auth(player) })).json().mods).toHaveLength(1);
  });

  it('ratings need the mod in the library; reports once per account', async () => {
    const rate = (stars: unknown) => app.inject({ method: 'POST', url: `${API}/mods/${modId}/rating`, headers: auth(player), payload: { stars } });
    expect((await rate(5)).statusCode).toBe(403);
    expect((await rate(5)).json()).toEqual({ error: 'not_in_library' });
    await download(player);
    expect((await rate(9)).statusCode).toBe(400);
    expect((await rate('5')).statusCode).toBe(400);
    expect((await rate(4)).statusCode).toBe(204);
    expect((await app.inject({ method: 'GET', url: `${API}/mods/${modId}` })).json()).toMatchObject({ ratings: 1, downloads: 1 });

    const rep = (payload: unknown, id: number | string = modId) => app.inject({ method: 'POST', url: `${API}/mods/${id}/report`, headers: auth(player), payload: payload as object });
    expect((await rep({ reason: 'spam' })).statusCode).toBe(400);
    expect((await rep({ reason: 'broken', text: 'x'.repeat(501) })).statusCode).toBe(400);
    expect((await rep({ reason: 'broken' }, 99999)).statusCode).toBe(404);
    expect((await rep({ reason: 'broken', text: 'crashes at the first wave' })).statusCode).toBe(204);
    expect((await rep({ reason: 'malware' })).statusCode).toBe(409);
  });

  it('rate limits download addresses', async () => {
    for (let i = 0; i < 30; i++) expect((await download(player)).statusCode).toBe(200);
    expect((await download(player)).statusCode).toBe(429);
    expect((await download(player, 99999)).statusCode).toBe(404);
  });

  it('taken-down mods', async () => {
    const before = (await download(player)).json().url;
    sql("UPDATE fab_mods SET state = 'taken_down' WHERE id = ?", modId);
    expect((await download(player)).statusCode).toBe(410);
    expect((await getFile(before)).statusCode).toBe(410);   // an address issued before the takedown stops working
    const list = (await app.inject({ method: 'GET', url: `${API}/mods?search=horde` })).json();
    expect(list.mods.map((m: { id: number }) => m.id)).not.toContain(modId);
    const lib = (await app.inject({ method: 'GET', url: `${API}/library`, headers: auth(player) })).json();
    expect(lib.mods).toEqual([expect.objectContaining({ id: modId, state: 'taken_down' })]);
  });
});
