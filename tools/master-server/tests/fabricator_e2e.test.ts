import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import type { AddressInfo } from 'node:net';
import type { FastifyInstance } from 'fastify';
import { buildServer } from '../src/server.js';
import { buildPrivateApi } from '../src/admin/private_api.js';
import { modZip, png, meta, sha256 } from './fabricator_helpers.js';

// The Fabricator end to end over real sockets: two accounts claimed through the auth routes, a chunked upload with a
// screenshot, the store's listing/search/page, the library, a download resumed with Range, a rating, a report, an
// admin takedown and restore through the private API, and the creator deleting their account.

const DAY = 24 * 3600 * 1000;
const SECRET = 'e2e-secret-0123456789';
const CHUNK = 1024;
type Claimed = { uuid: string; token: string; discriminator: string };

describe('fabricator end to end', () => {
  let dir: string;
  let fabDir: string;
  let app: FastifyInstance & { adminDeps: any };
  let priv: FastifyInstance;
  let base: string;
  let admin: string;
  let clock: number;

  beforeAll(async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabe2e-'));
    fabDir = path.join(dir, 'fabricator');
    clock = Date.now() + 4 * DAY;   // accounts claimed now are old enough to publish
    app = await buildServer({ dbPath: path.join(dir, 'master.db'), fabricatorDir: fabDir, rateLimitMax: 10000, now: () => clock,
      fabricator: { chunkBytes: CHUNK, diskFullFraction: 1 } }) as any;
    await app.listen({ port: 0, host: '127.0.0.1' });
    base = `http://127.0.0.1:${(app.server.address() as AddressInfo).port}`;
    priv = await buildPrivateApi({ ...app.adminDeps, secret: SECRET, dataDir: dir });
    await priv.listen({ port: 0, host: '127.0.0.1' });
    admin = `http://127.0.0.1:${(priv.server.address() as AddressInfo).port}`;
  });
  afterAll(async () => {
    await priv?.close();
    await app?.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });
  const api = (p: string, init: RequestInit = {}) => fetch(`${base}/api/v1/fabricator${p}`, init);
  const json = (body: unknown) => ({ 'content-type': 'application/json', body: JSON.stringify(body) });
  const post = (p: string, c: Claimed, body?: unknown) => api(p, body === undefined
    ? { method: 'POST', headers: auth(c) }
    : { method: 'POST', headers: { ...auth(c), 'content-type': 'application/json' }, body: JSON.stringify(body) });
  const adminPost = (p: string) => fetch(`${admin}/fabricator${p}`, {
    method: 'POST', headers: { 'x-admin-secret': SECRET, 'content-type': 'application/json' }, body: JSON.stringify({ who: 'e2e-admin' }) });
  const claim = async (display_name: string): Promise<Claimed> => {
    const { body } = json({ display_name });
    const r = await fetch(`${base}/api/v1/auth/claim`, { method: 'POST', headers: { 'content-type': 'application/json' }, body });
    expect(r.status).toBe(200);
    return r.json() as Promise<Claimed>;
  };
  const listed = async (q = '') => ((await (await api(`/mods${q}`)).json()) as { mods: { id: number }[] }).mods.map((m) => m.id);
  const storedFiles = () => fs.readdirSync(path.join(fabDir, 'files'), { recursive: true })
    .filter((n) => String(n).endsWith('.zip') || String(n).endsWith('.png'));
  const storedImages = () => fs.readdirSync(path.join(fabDir, 'images'));

  it('publish, find, get, rate, report, take down, restore and delete the account', async () => {
    const baird = await claim('Baird');
    const cole = await claim('Cole');
    const zip = await modZip('[Mod]\nName=Horde Bank\nGame=gears3\n', 5000);
    expect(zip.length).toBeGreaterThan(4 * CHUNK);

    // ---- upload: a session, a screenshot, the chunks in order (a status check part way), then finish
    const s = await post('/uploads', baird, { meta: meta(), size: zip.length, sha256: sha256(zip) });
    expect(s.status).toBe(200);
    const up = await s.json() as { id: string; chunk_bytes: number; chunks: number };
    expect(up.chunk_bytes).toBe(CHUNK);
    expect(up.chunks).toBe(Math.ceil(zip.length / CHUNK));
    const shot = await png(64, 36);
    expect((await api(`/uploads/${up.id}/screens/0`, { method: 'PUT', headers: { ...auth(baird), 'content-type': 'image/png' }, body: shot })).status).toBe(204);
    for (let n = 0; n < up.chunks; n++) {
      const body = zip.subarray(n * CHUNK, (n + 1) * CHUNK);
      const r = await api(`/uploads/${up.id}/chunks/${n}`, {
        method: 'PUT', body, headers: { ...auth(baird), 'content-type': 'application/octet-stream', 'x-chunk-sha256': sha256(body) } });
      expect(r.status).toBe(204);
      if (n === 1) {
        // what a resuming client asks first
        expect(await (await api(`/uploads/${up.id}`, { headers: auth(baird) })).json()).toEqual({ chunks_done: 2, chunks: up.chunks });
        // skipping ahead is refused with where to go on from
        const skip = zip.subarray(3 * CHUNK, 4 * CHUNK);
        const bad = await api(`/uploads/${up.id}/chunks/3`, { method: 'PUT', body: skip,
          headers: { ...auth(baird), 'content-type': 'application/octet-stream', 'x-chunk-sha256': sha256(skip) } });
        expect(bad.status).toBe(409);
        expect(((await bad.json()) as { chunks_done: number }).chunks_done).toBe(2);
      }
    }
    const fin = await post(`/uploads/${up.id}/finish`, baird);
    expect(fin.status).toBe(200);
    const { mod_id: modId } = await fin.json() as { mod_id: number; version_id: number };
    expect(fs.readdirSync(path.join(fabDir, 'chunks'))).toEqual([]);   // the session's chunks are gone
    expect(fs.readdirSync(path.join(fabDir, 'staging'))).toEqual([]);

    // ---- the public store: listing, search, filters, the page and its screenshot
    expect(await listed()).toEqual([modId]);
    expect(await listed('?search=horde')).toEqual([modId]);
    expect(await listed('?search=bank&game=gears3&type=gameplay')).toEqual([modId]);
    expect(await listed('?search=lancer')).toEqual([]);
    expect(await listed('?game=gears1')).toEqual([]);
    const page = await (await api(`/mods/${modId}`)).json() as any;
    expect(page).toMatchObject({ id: modId, name: 'Horde Bank', author: `Baird#${baird.discriminator}`, game: 'gears3', type: 'gameplay',
      tags: ['horde', 'economy'], ratings: 0, downloads: 0, size: zip.length });
    expect(page.versions).toHaveLength(1);
    expect(page.versions[0]).toMatchObject({ version: '1.0', sha256: sha256(zip), size: zip.length });
    expect(page.screens).toHaveLength(1);
    expect(JSON.stringify(page)).not.toContain(baird.uuid);
    const img = await fetch(base + page.screens[0]);
    expect(img.status).toBe(200);
    expect(img.headers.get('content-type')).toBe('image/png');
    const imgBytes = Buffer.from(await img.arrayBuffer());
    expect(imgBytes.subarray(1, 4).toString()).toBe('PNG');

    // ---- a player: rating needs the mod in the library; the download adds it
    expect((await post(`/mods/${modId}/rating`, cole, { stars: 5 })).status).toBe(403);
    const dl = await post(`/mods/${modId}/download`, cole);
    expect(dl.status).toBe(200);
    const grant = await dl.json() as { url: string; sha256: string; size: number };
    expect(grant).toMatchObject({ sha256: sha256(zip), size: zip.length });
    const lib = await (await api('/library', { headers: auth(cole) })).json() as { mods: any[] };
    expect(lib.mods).toHaveLength(1);
    expect(lib.mods[0]).toMatchObject({ id: modId, state: 'live', sha256: sha256(zip), size: zip.length });

    // the download: a first part cut short, then the rest with a Range resume
    const half = Math.floor(zip.length / 2);
    const first = await fetch(base + grant.url, { headers: { range: `bytes=0-${half - 1}` } });
    expect(first.status).toBe(206);
    expect(first.headers.get('content-range')).toBe(`bytes 0-${half - 1}/${zip.length}`);
    const part1 = Buffer.from(await first.arrayBuffer());
    const rest = await fetch(base + grant.url, { headers: { range: `bytes=${part1.length}-` } });
    expect(rest.status).toBe(206);
    expect(rest.headers.get('content-range')).toBe(`bytes ${half}-${zip.length - 1}/${zip.length}`);
    const got = Buffer.concat([part1, Buffer.from(await rest.arrayBuffer())]);
    expect(sha256(got)).toBe(sha256(zip));
    const whole = await fetch(base + grant.url);
    expect(whole.status).toBe(200);
    expect(sha256(Buffer.from(await whole.arrayBuffer()))).toBe(sha256(zip));

    // ---- rate and report
    expect((await post(`/mods/${modId}/rating`, cole, { stars: 5 })).status).toBe(204);
    expect(await (await api(`/mods/${modId}`)).json()).toMatchObject({ ratings: 1, downloads: 1 });
    expect((await post(`/mods/${modId}/report`, cole, { reason: 'broken', text: 'crashes on wave 3' })).status).toBe(204);
    expect((await post(`/mods/${modId}/report`, cole, { reason: 'broken' })).status).toBe(409);
    const queue = await (await fetch(`${admin}/fabricator/reports`, { headers: { 'x-admin-secret': SECRET } })).json() as { reports: any[] };
    expect(queue.reports).toHaveLength(1);
    expect(queue.reports[0].mod).toMatchObject({ id: modId, state: 'live' });
    expect(queue.reports[0].reports[0]).toMatchObject({ reason: 'broken', text: 'crashes on wave 3', reporter: `Cole#${cole.discriminator}` });
    expect((await fetch(`${admin}/fabricator/mods/${modId}/takedown`, { method: 'POST', headers: { 'x-admin-secret': 'wrong' } })).status).toBe(401);

    // ---- admin takedown: hidden from the store, the issued address and new ones answer 410
    const later = await post(`/mods/${modId}/download`, cole);
    expect(later.status).toBe(200);
    const laterUrl = (await later.json() as { url: string }).url;
    expect((await adminPost(`/mods/${modId}/takedown`)).status).toBe(200);
    expect(await listed()).toEqual([]);
    expect(await listed('?search=horde')).toEqual([]);
    expect(await (await api(`/mods/${modId}`)).json()).toEqual({ id: modId, name: 'Horde Bank', state: 'taken_down' });
    expect((await fetch(base + laterUrl)).status).toBe(410);
    expect((await post(`/mods/${modId}/download`, cole)).status).toBe(410);
    expect(((await (await api('/library', { headers: auth(cole) })).json()) as { mods: any[] }).mods[0].state).toBe('taken_down');

    // ---- restore: listed and downloadable again (with a new address), the rating kept
    expect(await (await adminPost(`/mods/${modId}/restore`)).json()).toEqual({ ok: true });
    expect(await listed()).toEqual([modId]);
    expect(await (await api(`/mods/${modId}`)).json()).toMatchObject({ ratings: 1 });
    const again = await post(`/mods/${modId}/download`, cole);
    expect(again.status).toBe(200);
    const againUrl = (await again.json() as { url: string }).url;
    expect(sha256(Buffer.from(await (await fetch(base + againUrl)).arrayBuffer()))).toBe(sha256(zip));
    expect(storedFiles()).toHaveLength(1);
    expect(storedImages()).toHaveLength(1);

    // ---- the creator deletes their account: their mod and its files go, the player's library shows it removed
    const del = await fetch(`${base}/api/v1/auth/account`, { method: 'DELETE', headers: auth(baird) });
    expect(del.status).toBe(200);
    expect(await del.json()).toEqual({ deleted: true });
    expect(storedFiles()).toEqual([]);
    expect(storedImages()).toEqual([]);
    expect(await listed()).toEqual([]);
    expect((await api(`/mods/${modId}`)).status).toBe(404);
    expect((await fetch(base + page.screens[0])).status).toBe(404);
    expect((await fetch(base + againUrl)).status).toBe(410);
    expect((await api('/creator/mods', { headers: auth(baird) })).status).toBe(401);   // the account is gone
    const after = await (await api('/library', { headers: auth(cole) })).json() as { mods: any[] };
    expect(after.mods).toHaveLength(1);
    expect(after.mods[0]).toMatchObject({ id: modId, state: 'removed' });
    expect((await post(`/mods/${modId}/download`, cole)).status).not.toBe(200);
  });
});
