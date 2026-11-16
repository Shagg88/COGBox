import type { FastifyPluginAsync, FastifyReply, FastifyRequest } from 'fastify';
import type { DatabaseInstance } from '../db.js';
import { checkAuthHeader, type AuthedUser } from '../auth.js';
import type { FabSettings } from './settings.js';
import type { FabStore } from './store.js';
import fs from 'node:fs';
import { Transform, pipeline } from 'node:stream';
import { listMods, modPage, type SortKey } from './catalog.js';
import {
  canPublish, cancelUpload, creatorLimits, creatorMods, removeOwnMod, expireUploads, getUpload, normalizeMeta, ownLiveMod,
  putChunk, putScreen, recoverInterruptedFinishes, startFinish, startUpload, uploadProblems, uploadStatus, MAX_SCREENS,
  UPLOAD_BUSY, UPLOAD_FINISHED, type UploadRow,
} from './publish.js';
import { addToLibrary, DownloadTokens, issueDownload, library, modIsLive, rate, removeFromLibrary, report, REPORT_TEXT_MAX } from './player.js';
import { addTraffic } from './moderation.js';

export interface FabRouteOptions { db: DatabaseInstance; store: FabStore; settings: FabSettings; now: () => number }

const UPLOAD_ID = /^[A-Za-z0-9_-]{8,64}$/;
const SHA = /^[0-9a-f]{64}$/;
const SCREEN_LIMIT = 4 * 1024 * 1024 + 1024;
const EXPIRE_EVERY_MS = 10 * 60 * 1000;
const SORTS = new Set<string>(['popular', 'rating', 'new', 'updated', 'name']);
const IMAGE_KEY = /^[0-9a-f]{64}\.png$/;
const AUTH_ERROR = { error: 'Authorization: COGBox <uuid>:<token> required' };
const TOKEN = /^[0-9a-f]{64}$/;
const MOD_ID = /^\d{1,9}$/;

// One byte range from a Range header against a file of `size` bytes: { start, end } (inclusive), null when there is
// no header, or 'bad' for anything unsatisfiable or not a single bytes range (bytes=a-, bytes=a-b, bytes=-n).
export function parseRange(header: string | undefined, size: number): { start: number; end: number } | null | 'bad' {
  if (header === undefined) return null;
  const m = /^bytes=(\d{0,15})-(\d{0,15})$/.exec(header.trim());
  if (!m || (m[1] === '' && m[2] === '')) return 'bad';
  if (m[1] === '') {
    const n = Number(m[2]);
    if (n === 0 || size === 0) return 'bad';
    return { start: Math.max(0, size - n), end: size - 1 };
  }
  const start = Number(m[1]);
  const end = m[2] === '' ? size - 1 : Math.min(Number(m[2]), size - 1);
  if (start >= size || start > end) return 'bad';
  return { start, end };
}

// The Fabricator API (mounted at /api/v1/fabricator). Log lines never carry tokens, uuids, addresses or paths.
export const fabricatorRoutes: FastifyPluginAsync<FabRouteOptions> = async (fastify, opts) => {
  const { db, store, settings: s, now } = opts;
  const tokens = new DownloadTokens();
  // A finish the last run of the server left 'finishing' never ends now: it is marked failed (see the finish protocol).
  if (recoverInterruptedFinishes(db) > 0) fastify.log.warn('fabricator: upload finishes cut short by a restart were marked failed');
  const perMinute = (max: number) => ({ rateLimit: { max: Math.max(1, Math.floor(max)), timeWindow: '1 minute' } });

  let timer: NodeJS.Timeout | null = null;
  fastify.addHook('onReady', async () => {
    timer = setInterval(() => {
      try { expireUploads(db, store, now()); } catch { fastify.log.error('fabricator: expiring upload sessions failed'); }
    }, EXPIRE_EVERY_MS);
    timer.unref();
  });
  fastify.addHook('onClose', async () => { if (timer) clearInterval(timer); timer = null; });

  // The public store: no sign-in, cached for a minute (images for a day: their names are content hashes).
  const text = (v: unknown, max = 200): string | undefined => (typeof v === 'string' && v.length > 0 ? v.slice(0, max) : undefined);
  fastify.get<{ Querystring: Record<string, unknown> }>('/mods', async (req, reply) => {
    const q = req.query ?? {};
    const sort = typeof q.sort === 'string' && SORTS.has(q.sort) ? (q.sort as SortKey) : 'popular';
    const page = Number(q.page);
    const perPage = Number(q.per_page);
    reply.header('Cache-Control', 'public, max-age=60');
    return listMods(db, {
      search: text(q.search), game: text(q.game, 24), type: text(q.type, 24), sort,
      page: Number.isFinite(page) ? Math.min(10000, Math.max(1, Math.floor(page))) : 1, perPage: Number.isFinite(perPage) ? Math.min(48, Math.max(1, Math.floor(perPage))) : 24,
    }, now());
  });

  fastify.get<{ Params: { id: string } }>('/mods/:id', async (req, reply) => {
    const page = /^\d{1,9}$/.test(req.params.id) ? modPage(db, Number(req.params.id), now()) : null;
    if (!page) return reply.status(404).send({ error: 'mod not found' });
    reply.header('Cache-Control', 'public, max-age=60');
    return page;
  });

  // Pictures are content-hashed and cheap: a generous per-address limit of their own (a store page shows many).
  fastify.get<{ Params: { key: string } }>('/images/:key', { config: perMinute(s.imageRequestsPerMinute) }, async (req, reply) => {
    const key = req.params.key;
    if (!IMAGE_KEY.test(key)) return reply.status(404).send({ error: 'image not found' });
    const file = store.pathOf(key);
    if (!fs.existsSync(file)) return reply.status(404).send({ error: 'image not found' });
    reply.header('Cache-Control', 'public, max-age=86400');
    return reply.type('image/png').send(fs.createReadStream(file));
  });

  // A download address: no sign-in, the token is the key. Stops working when it expires or the mod is taken down.
  fastify.get<{ Params: { token: string } }>('/files/:token', async (req, reply) => {
    if (!TOKEN.test(req.params.token)) return reply.status(404).send({ error: 'download not found' });
    const grant = tokens.get(req.params.token, now());
    if (!grant) return reply.status(410).send({ error: 'download address expired: ask for a new one' });
    if (!modIsLive(db, grant.modId)) return reply.status(410).send({ error: 'mod taken down' });
    const file = store.pathOf(grant.key);
    let size: number;
    try { size = (await fs.promises.stat(file)).size; } catch { return reply.status(410).send({ error: 'file no longer available' }); }
    reply.header('Accept-Ranges', 'bytes').header('Cache-Control', 'no-store');
    const range = parseRange(req.headers.range, size);
    if (range === 'bad') return reply.status(416).header('Content-Range', `bytes */${size}`).send({ error: 'bad range' });
    reply.type('application/zip').header('Content-Disposition', 'attachment; filename="mod.zip"');
    // The bytes passed on to the response are added to the day's traffic once the response is over (finished or cut).
    let sent = 0;
    let counted = false;
    const count = () => {
      if (counted) return;
      counted = true;
      try { addTraffic(db, now(), sent); } catch { fastify.log.error('fabricator: counting download bytes failed'); }
    };
    reply.raw.once('finish', count);
    reply.raw.once('close', count);
    const counter = new Transform({ transform(b: Buffer, _enc, done) { sent += b.length; done(null, b); } });
    const src = range === null ? fs.createReadStream(file) : fs.createReadStream(file, { start: range.start, end: range.end });
    pipeline(src, counter, () => { /* errors end the response; fastify logs nothing with the path */ });
    if (range === null) {
      reply.header('Content-Length', size);
      return reply.send(counter);
    }
    reply.status(206).header('Content-Range', `bytes ${range.start}-${range.end}/${size}`).header('Content-Length', range.end - range.start + 1);
    return reply.send(counter);
  });

  // The library, download addresses, ratings and reports: a signed-in account.
  await fastify.register(async (pl) => {
    const users = new WeakMap<FastifyRequest, AuthedUser>();
    pl.addHook('onRequest', async (req, reply) => {
      const me = checkAuthHeader(db, req.headers.authorization);
      if (!me) return reply.status(401).send(AUTH_ERROR);
      users.set(req, me);
    });
    const me = (req: FastifyRequest) => (users.get(req) as AuthedUser).uuid;
    const modId = (id: string): number | null => (MOD_ID.test(id) ? Number(id) : null);
    const exists = (id: number) => !!db.prepare("SELECT 1 FROM fab_mods WHERE id = ? AND state != 'removed'").get(id);

    pl.get('/library', async (req) => ({ mods: library(db, me(req)) }));

    pl.post<{ Params: { id: string } }>('/library/:id', async (req, reply) => {
      const id = modId(req.params.id);
      if (id === null || !addToLibrary(db, me(req), id, now())) return reply.status(404).send({ error: 'mod not found' });
      return reply.status(204).send();
    });

    pl.delete<{ Params: { id: string } }>('/library/:id', async (req, reply) => {
      const id = modId(req.params.id);
      if (id !== null) removeFromLibrary(db, me(req), id);
      return reply.status(204).send();
    });

    pl.post<{ Params: { id: string } }>('/mods/:id/download', async (req, reply) => {
      const id = modId(req.params.id);
      const r = id === null ? { error: 'not_found' as const } : issueDownload(db, s, me(req), id, now(), tokens);
      if (!('error' in r)) return r;
      if (r.error === 'taken_down') return reply.status(410).send({ error: 'taken_down' });
      if (r.error === 'rate_limited') return reply.status(429).send({ error: 'rate_limited' });
      return reply.status(404).send({ error: 'not_found' });
    });

    pl.post<{ Params: { id: string }; Body: { stars?: unknown } }>('/mods/:id/rating', async (req, reply) => {
      const id = modId(req.params.id);
      const r = id === null ? 'not_in_library' : rate(db, me(req), id, req.body?.stars, now());
      if (r === 'bad_stars') return reply.status(400).send({ error: 'stars must be a whole number from 1 to 5' });
      if (r === 'not_in_library') return reply.status(403).send({ error: 'not_in_library' });
      if (r === 'own_mod') return reply.status(403).send({ error: 'own mod' });
      return reply.status(204).send();
    });

    pl.post<{ Params: { id: string }; Body: { reason?: unknown; text?: unknown } }>('/mods/:id/report', async (req, reply) => {
      const id = modId(req.params.id);
      if (id === null || !exists(id)) return reply.status(404).send({ error: 'mod not found' });
      const text = req.body?.text ?? '';
      if (typeof text !== 'string' || text.length > REPORT_TEXT_MAX) return reply.status(400).send({ error: `text must be up to ${REPORT_TEXT_MAX} characters` });
      const r = report(db, me(req), id, req.body?.reason, text, now());
      if (r === 'bad_reason') return reply.status(400).send({ error: 'reason must be malware, stolen, broken, offensive or other' });
      if (r === 'already') return reply.status(409).send({ error: 'already reported' });
      return reply.status(204).send();
    });
  });

  // Upload sessions: every route needs a signed-in account, checked before any body is read.
  // The creator's own pages: their mods, removing one, and what they may still publish.
  await fastify.register(async (cr) => {
    const users = new WeakMap<FastifyRequest, AuthedUser>();
    cr.addHook('onRequest', async (req, reply) => {
      const me = checkAuthHeader(db, req.headers.authorization);
      if (!me) return reply.status(401).send(AUTH_ERROR);
      users.set(req, me);
    });
    const me = (req: FastifyRequest) => (users.get(req) as AuthedUser).uuid;

    cr.get('/creator/mods', async (req) => ({ mods: creatorMods(db, me(req)) }));

    cr.delete<{ Params: { id: string } }>('/creator/mods/:id', async (req, reply) => {
      const id = MOD_ID.test(req.params.id) ? Number(req.params.id) : null;
      if (id === null || !removeOwnMod(db, store, me(req), id)) return reply.status(404).send({ error: 'mod not found' });
      return reply.status(204).send();
    });

    cr.get('/creator/limits', async (req) => creatorLimits(db, s, me(req), now(), await store.disk()));
  });

  await fastify.register(async (up) => {
    const users = new WeakMap<FastifyRequest, AuthedUser>();
    up.addHook('onRequest', async (req, reply) => {
      const me = checkAuthHeader(db, req.headers.authorization);
      if (!me) return reply.status(401).send(AUTH_ERROR);
      users.set(req, me);
    });
    const me = (req: FastifyRequest) => users.get(req) as AuthedUser;
    const raw = (_req: FastifyRequest, body: Buffer, done: (err: Error | null, body?: Buffer) => void) => done(null, body);
    up.addContentTypeParser(['application/octet-stream', 'image/png', 'image/jpeg'], { parseAs: 'buffer' }, raw);

    // The caller's live session, or the answer already sent (404 not theirs / unknown, 410 expired).
    const session = (req: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply): UploadRow | null => {
      const id = req.params.id;
      const row = UPLOAD_ID.test(id) ? getUpload(db, id) : undefined;
      if (!row || row.account !== me(req).uuid) { reply.status(404).send({ error: 'upload not found' }); return null; }
      if (row.expires_at < now()) { reply.status(410).send({ error: 'upload expired' }); return null; }
      return row;
    };

    up.post<{ Body: { meta?: unknown; mod_id?: unknown; size?: unknown; sha256?: unknown } }>('/uploads', async (req, reply) => {
      const b = req.body ?? {};
      if (!b.meta || typeof b.meta !== 'object' || Array.isArray(b.meta)) return reply.status(400).send({ problems: ['meta is required'] });
      const modId = b.mod_id === undefined || b.mod_id === null ? null : b.mod_id;
      if (modId !== null && !Number.isSafeInteger(modId)) return reply.status(400).send({ problems: ['mod_id must be a mod id'] });
      const account = me(req).uuid;
      if (modId !== null && !ownLiveMod(db, account, modId as number)) return reply.status(403).send({ error: 'not your mod' });
      const size = typeof b.size === 'number' ? b.size : 0;
      const t = now();
      const reason = canPublish(db, s, account, size, t, await store.disk());
      if (reason) return reply.status(403).send({ error: reason });
      const meta = normalizeMeta(b.meta);
      const problems = uploadProblems(db, account, meta, modId as number | null, b.size, b.sha256);
      if (problems.length > 0) return reply.status(400).send({ problems });
      const r = startUpload(db, account, meta, modId as number | null, size, b.sha256 as string, t, s);
      return { id: r.id, chunk_bytes: r.chunkBytes, chunks: r.chunks };
    });

    // ---- The finish protocol (the launcher's side is fabricator_net.cpp) ----
    // POST /uploads/:id/finish
    //   202 {state:'finishing'}   the checks and publishing now run in the background
    //   200 {state:'done', ...}   it was already published: the same body as GET (a repeat after a lost answer)
    //   409 {error:'finishing'}   a finish of this session is running: ask GET until it ends
    //   409 {error:'upload busy: try again'}   a chunk or screenshot request is running on it
    //   422 {problems:[...]}      refused at once: not every chunk is uploaded (the session stays open)
    //   404 not the caller's / unknown, 410 expired, 500 the server could not start it
    // GET /uploads/:id -> {state, chunks, chunks_done, mod_id?, version_id?, problems?}
    //   'open'       chunks and screenshots may be sent, then a finish
    //   'finishing'  checking and publishing: ask again in a few seconds
    //   'done'       published as mod_id / version_id. The session stays readable until it expires (at least an hour
    //                after the finish) so a client that lost the answer learns it; chunk and screenshot PUTs are 409.
    //                A DELETE forgets it (the mod stays published).
    //   'failed'     problems says why; the session and its chunks are kept (as an open one): the client puts it right
    //                and finishes again, or DELETEs it. A server restart during a finish ends there too
    //                ('the server restarted ...'), never stuck in 'finishing'.
    up.get<{ Params: { id: string } }>('/uploads/:id', async (req, reply) => {
      const row = session(req, reply);
      if (!row) return reply;
      return uploadStatus(row);
    });

    // Chunks and screenshots have their own per-account limit: a fast uplink sends far more than the shared 120 a minute,
    // and each session already allows one request at a time.
    const uploadLimit = perMinute(s.uploadRequestsPerMinute);
    const busyError = (id: string) => (getUpload(db, id)?.state === 'finishing' ? 'finishing' : UPLOAD_BUSY);

    up.put<{ Params: { id: string; n: string } }>('/uploads/:id/chunks/:n', { bodyLimit: s.chunkBytes + 1024, config: uploadLimit }, async (req, reply) => {
      const row = session(req, reply);
      if (!row) return reply;
      const hash = req.headers['x-chunk-sha256'];
      if (typeof hash !== 'string' || !SHA.test(hash)) return reply.status(400).send({ error: 'x-chunk-sha256 must be the chunk\'s SHA-256 (64 lowercase hex)' });
      if (!/^\d{1,9}$/.test(req.params.n)) return reply.status(400).send({ error: 'bad chunk number' });
      if (!Buffer.isBuffer(req.body)) return reply.status(415).send({ error: 'send the chunk as application/octet-stream' });
      const r = await putChunk(db, store, row.id, Number(req.params.n), req.body, hash);
      switch (r) {
        case 'ok': case 'repeat': return reply.status(204).send();
        case 'hash': return reply.status(409).send({ error: 'chunk SHA-256 does not match' });
        case 'order': return reply.status(409).send({ error: 'chunks go in order', chunks_done: getUpload(db, row.id)?.chunks_done ?? 0 });
        case 'busy': return reply.status(409).send({ error: busyError(row.id) });
        case 'finished': return reply.status(409).send({ error: UPLOAD_FINISHED });
        case 'size': return reply.status(400).send({ error: 'chunk has the wrong size' });
        default: return reply.status(404).send({ error: 'upload not found' });
      }
    });

    up.put<{ Params: { id: string; pos: string } }>('/uploads/:id/screens/:pos', { bodyLimit: SCREEN_LIMIT, config: uploadLimit }, async (req, reply) => {
      const row = session(req, reply);
      if (!row) return reply;
      const pos = /^\d$/.test(req.params.pos) ? Number(req.params.pos) : -1;
      if (pos < 0 || pos >= MAX_SCREENS) return reply.status(400).send({ problem: `screenshot position must be 0 to ${MAX_SCREENS - 1}` });
      if (!Buffer.isBuffer(req.body)) return reply.status(415).send({ problem: 'send the image as application/octet-stream' });
      const problem = await putScreen(db, store, row.id, pos, req.body);
      if (problem === UPLOAD_BUSY) return reply.status(409).send({ error: busyError(row.id) });
      if (problem === UPLOAD_FINISHED) return reply.status(409).send({ error: UPLOAD_FINISHED });
      if (problem) return reply.status(400).send({ problem });
      return reply.status(204).send();
    });

    up.post<{ Params: { id: string } }>('/uploads/:id/finish', async (req, reply) => {
      const row = session(req, reply);
      if (!row) return reply;
      try {
        const r = startFinish(db, store, s, row.id, row.account, now());
        if ('started' in r) {
          r.started.then((x) => { if (!x.ok && x.problems.some((p) => p.startsWith('publishing failed on the server'))) fastify.log.error('fabricator: publishing an upload failed'); });
          return reply.status(202).send({ state: 'finishing' });
        }
        if ('done' in r) return r.done;
        if ('problems' in r) return reply.status(422).send({ problems: r.problems });
        if (r.refused === 'finishing') return reply.status(409).send({ error: 'finishing' });
        if (r.refused === 'busy') return reply.status(409).send({ error: UPLOAD_BUSY });
        if (r.refused === 'expired') return reply.status(410).send({ error: 'upload expired' });
        return reply.status(404).send({ error: 'upload not found' });
      } catch {
        fastify.log.error('fabricator: starting an upload finish failed');
        return reply.status(500).send({ error: 'publishing failed on the server' });
      }
    });

    up.delete<{ Params: { id: string } }>('/uploads/:id', async (req, reply) => {
      const row = session(req, reply);
      if (!row) return reply;
      if (!cancelUpload(db, store, row.id)) return reply.status(409).send({ error: 'upload busy: try again' });
      return reply.status(204).send();
    });
  });
};
