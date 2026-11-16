import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import type { FastifyInstance } from 'fastify';
import { buildServer } from '../src/server.js';
import { modZip, png, meta, sha256 } from './fabricator_helpers.js';

const DAY = 24 * 3600 * 1000;
const API = '/api/v1/fabricator';
type Claimed = { uuid: string; token: string };

describe('fabricator upload routes', () => {
  let dir: string;
  let dbPath: string;
  let app: FastifyInstance;
  let clock: number;

  const start = () => buildServer({
    dbPath, rateLimitMax: 10000, now: () => clock,
    fabricator: { chunkBytes: 1024, creatorUploadsPerDay: 100, diskFullFraction: 1 },   // the test machine's disk may be full
  });
  async function claim(display_name: string): Promise<Claimed> {
    const res = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name } });
    return res.json();
  }
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });
  const chunkOf = (zip: Buffer, n: number) => zip.subarray(n * 1024, (n + 1) * 1024);
  const putChunk = (c: Claimed, id: string, n: number, body: Buffer, hash = sha256(body)) => app.inject({
    method: 'PUT', url: `${API}/uploads/${id}/chunks/${n}`, payload: body,
    headers: { ...auth(c), 'content-type': 'application/octet-stream', 'x-chunk-sha256': hash },
  });
  const putScreen = async (c: Claimed, id: string, pos = 0, body?: Buffer) => app.inject({
    method: 'PUT', url: `${API}/uploads/${id}/screens/${pos}`, payload: body ?? await png(),
    headers: { ...auth(c), 'content-type': 'application/octet-stream' },
  });
  const begin = (c: Claimed, zip: Buffer, extra: Record<string, unknown> = {}) => app.inject({
    method: 'POST', url: `${API}/uploads`, headers: auth(c),
    payload: { meta: meta(), size: zip.length, sha256: sha256(zip), ...extra },
  });
  const finish = (c: Claimed, id: string) => app.inject({ method: 'POST', url: `${API}/uploads/${id}/finish`, headers: auth(c) });
  const threeChunkZip = async () => {
    const zip = await modZip('[Mod]\nName=Horde Bank\n', 2300);
    expect(Math.ceil(zip.length / 1024)).toBe(3);
    return zip;
  };

  beforeEach(async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabroutes-'));
    dbPath = path.join(dir, 'master.db');
    clock = Date.now() + 4 * DAY;   // accounts claimed now are old enough to publish
    app = await start();
  });
  afterEach(async () => {
    await app.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  async function fullUpload(c: Claimed, zip: Buffer, extra: Record<string, unknown> = {}) {
    const s = await begin(c, zip, extra);
    expect(s.statusCode).toBe(200);
    const { id, chunk_bytes, chunks } = s.json();
    expect(chunk_bytes).toBe(1024);
    for (let n = 0; n < chunks; n++) expect((await putChunk(c, id, n, chunkOf(zip, n))).statusCode).toBe(204);
    expect((await putScreen(c, id)).statusCode).toBe(204);
    return finish(c, id);
  }

  it('uploads a 3-chunk zip with a screenshot and publishes it', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const s = await begin(me, zip);
    expect(s.statusCode).toBe(200);
    expect(s.json()).toMatchObject({ chunk_bytes: 1024, chunks: 3 });
    const id = s.json().id;
    for (let n = 0; n < 3; n++) expect((await putChunk(me, id, n, chunkOf(zip, n))).statusCode).toBe(204);
    expect((await app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(me) })).json()).toEqual({ chunks_done: 3, chunks: 3 });
    expect((await putScreen(me, id)).statusCode).toBe(204);
    const f = await finish(me, id);
    expect(f.statusCode).toBe(200);
    expect(f.json()).toEqual({ mod_id: expect.any(Number), version_id: expect.any(Number) });
    expect((await app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(me) })).statusCode).toBe(404);
  });

  it('needs Authorization', async () => {
    const zip = await threeChunkZip();
    const r = await app.inject({ method: 'POST', url: `${API}/uploads`, payload: { meta: meta(), size: zip.length, sha256: sha256(zip) } });
    expect(r.statusCode).toBe(401);
    expect(r.json().error).toContain('Authorization');
    const g = await app.inject({ method: 'GET', url: `${API}/uploads/abcdefghij` });
    expect(g.statusCode).toBe(401);
  });

  it('refuses a chunk whose x-chunk-sha256 is wrong', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const id = (await begin(me, zip)).json().id;
    expect((await putChunk(me, id, 0, chunkOf(zip, 0), 'ab'.repeat(32))).statusCode).toBe(409);
    expect((await putChunk(me, id, 0, chunkOf(zip, 0))).statusCode).toBe(204);
    expect((await putChunk(me, id, 0, chunkOf(zip, 0))).statusCode).toBe(204);   // a resent last chunk is ignored
  });

  it('refuses chunk 2 before chunk 1', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const id = (await begin(me, zip)).json().id;
    expect((await putChunk(me, id, 0, chunkOf(zip, 0))).statusCode).toBe(204);
    expect((await putChunk(me, id, 2, chunkOf(zip, 2))).statusCode).toBe(409);
  });

  it('resumes after a restart', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const id = (await begin(me, zip)).json().id;
    expect((await putChunk(me, id, 0, chunkOf(zip, 0))).statusCode).toBe(204);
    await app.close();
    app = await start();
    const g = await app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(me) });
    expect(g.statusCode).toBe(200);
    expect(g.json()).toEqual({ chunks_done: 1, chunks: 3 });
    for (let n = 1; n < 3; n++) expect((await putChunk(me, id, n, chunkOf(zip, n))).statusCode).toBe(204);
    expect((await putScreen(me, id)).statusCode).toBe(204);
    expect((await finish(me, id)).statusCode).toBe(200);
  });

  it('finish without a screenshot is 422 with screenshot', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const id = (await begin(me, zip)).json().id;
    for (let n = 0; n < 3; n++) await putChunk(me, id, n, chunkOf(zip, n));
    const f = await finish(me, id);
    expect(f.statusCode).toBe(422);
    expect(f.json().problems.join(' | ')).toContain('screenshot');
  });

  it('refuses a new version of another account\'s mod', async () => {
    const a = await claim('Baird');
    const b = await claim('Cole');
    const zip = await threeChunkZip();
    const pub = await fullUpload(a, zip);
    expect(pub.statusCode).toBe(200);
    const r = await begin(b, zip, { mod_id: pub.json().mod_id });
    expect(r.statusCode).toBe(403);
    expect((await begin(a, zip, { mod_id: pub.json().mod_id })).statusCode).toBe(200);
    const g = await begin(a, zip, { mod_id: pub.json().mod_id, meta: meta({ game: 'gears1' }) });
    expect(g.statusCode).toBe(400);
    expect(g.json().problems.join()).toContain('game');
  });

  it('refuses a bad screenshot, bad metadata and a new account', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const id = (await begin(me, zip)).json().id;
    const bad = await putScreen(me, id, 0, Buffer.from('not an image'));
    expect(bad.statusCode).toBe(400);
    expect(bad.json().problem).toContain('image');
    expect((await putScreen(me, id, 6)).statusCode).toBe(400);
    const m = await begin(me, zip, { meta: meta({ name: 'x' }) });
    expect(m.statusCode).toBe(400);
    expect(m.json().problems.join()).toContain('name');
    clock = Date.now();
    const young = await begin(me, zip);
    expect(young.statusCode).toBe(403);
    expect(young.json().error).toBe('account too new');
  });

  it('only the owner sees a session; DELETE ends it; an expired one is 410', async () => {
    const a = await claim('Baird');
    const b = await claim('Cole');
    const zip = await threeChunkZip();
    const id = (await begin(a, zip)).json().id;
    expect((await app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(b) })).statusCode).toBe(404);
    expect((await app.inject({ method: 'DELETE', url: `${API}/uploads/${id}`, headers: auth(a) })).statusCode).toBe(204);
    expect((await app.inject({ method: 'GET', url: `${API}/uploads/${id}`, headers: auth(a) })).statusCode).toBe(404);
    const id2 = (await begin(a, zip)).json().id;
    clock += DAY + 1;
    expect((await app.inject({ method: 'GET', url: `${API}/uploads/${id2}`, headers: auth(a) })).statusCode).toBe(410);
    expect((await putChunk(a, id2, 0, chunkOf(zip, 0))).statusCode).toBe(410);
    expect((await finish(a, id2)).statusCode).toBe(410);
  });

  it('refuses a chunk over chunk_bytes', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const id = (await begin(me, zip)).json().id;
    const big = Buffer.alloc(1024 + 2048, 1);
    expect((await putChunk(me, id, 0, big)).statusCode).toBe(413);
  });
  it('serves the public store without sign-in: listing, page, image', async () => {
    const me = await claim('Baird');
    const zip = await threeChunkZip();
    const r = await fullUpload(me, zip);
    expect(r.statusCode).toBe(200);
    const list = await app.inject({ method: 'GET', url: `${API}/mods?search=horde&sort=bogus` });
    expect(list.statusCode).toBe(200);
    expect(list.headers['cache-control']).toBe('public, max-age=60');
    const { mods, total } = list.json();
    expect(total).toBe(1);
    expect(mods[0].author).toMatch(/^Baird#\d+$/);
    expect(JSON.stringify(list.json())).not.toContain(me.uuid);
    const page = await app.inject({ method: 'GET', url: `${API}/mods/${mods[0].id}` });
    expect(page.statusCode).toBe(200);
    expect(page.json().versions.length).toBe(1);
    expect((await app.inject({ method: 'GET', url: `${API}/mods/99999` })).statusCode).toBe(404);
    expect((await app.inject({ method: 'GET', url: `${API}/mods/abc` })).statusCode).toBe(404);
    const img = await app.inject({ method: 'GET', url: mods[0].screen });
    expect(img.statusCode).toBe(200);
    expect(img.headers['content-type']).toBe('image/png');
    expect(img.headers['cache-control']).toBe('public, max-age=86400');
    expect((await app.inject({ method: 'GET', url: `${API}/images/..%2F..%2Fmaster.db` })).statusCode).toBe(404);
    expect((await app.inject({ method: 'GET', url: `${API}/images/${'0'.repeat(64)}.png` })).statusCode).toBe(404);
  });
});
