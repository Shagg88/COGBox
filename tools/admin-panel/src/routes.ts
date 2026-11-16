import type { FastifyInstance } from 'fastify';
import type { Db } from './db.js';
import { FAB_STATES, type DockerClient, type FabAction, type MasterClient } from './types.js';
import { audit } from './audit.js';
import { logSafeText } from './scrub.js';

type Svc = 'master-server' | 'coturn';
const isSvc = (s: unknown): s is Svc => s === 'master-server' || s === 'coturn';
const LOG_LINES = 300;
const FAB_AUDIT = { takedown: 'fab_takedown', restore: 'fab_restore', 'delete-files': 'fab_delete_files' } as const;
const isFabAction = (a: string): a is FabAction => Object.hasOwn(FAB_AUDIT, a);

// The session, CSRF and must_change checks are the hook in buildApp; these routes only do the work.
export function registerRoutes(api: FastifyInstance, o: { db: Db; now: () => number; master: MasterClient; docker: DockerClient }): void {
  const { db, now, master, docker } = o;

  api.get('/overview', async () => {
    // either side may be down; the page shows that instead of failing
    const [health, status] = await Promise.all([
      docker.health().catch(() => null),
      master.status().catch(() => null),
    ]);
    const last_action = db.prepare(
      "SELECT id, time, name, action, detail FROM audit WHERE action IN ('restart','backup','lobby_close') ORDER BY id DESC LIMIT 1",
    ).get() ?? null;
    return { health, status, last_action };
  });

  api.get('/lobbies', async () => master.lobbies());

  api.post('/lobbies/:id/close', async (req, reply) => {
    const { id } = req.params as { id: string };
    if ((req.body as { confirm?: unknown } | undefined)?.confirm !== true) return reply.status(400).send({ error: 'Confirm the close.' });
    const lobby = (await master.lobbies()).find((l) => l.lobby_id === id);
    if (!lobby) return reply.status(404).send({ error: 'No such lobby.' });
    if (!(await master.closeLobby(id))) return reply.status(404).send({ error: 'No such lobby.' });
    audit(db, req.session!.name, 'lobby_close', lobby.room_code, now());
    return { ok: true };
  });

  api.get('/logs', async (req, reply) => {
    const service = (req.query as { service?: unknown }).service;
    if (!isSvc(service)) return reply.status(400).send({ error: 'Unknown service.' });
    try {
      // coturn prints player addresses and the master's raw error text can hold ids: scrub before showing.
      return { lines: (await docker.logs(service, LOG_LINES)).map(logSafeText) };
    } catch (e) {
      return reply.status(502).send({ error: (e as Error).message });
    }
  });

  api.post('/restart', async (req, reply) => {
    const b = (req.body ?? {}) as { service?: unknown; confirm?: unknown };
    if (!isSvc(b.service)) return reply.status(400).send({ error: 'Unknown service.' });
    if (b.confirm !== 'RESTART') return reply.status(400).send({ error: 'Type RESTART to confirm.' });
    const name = req.session!.name;
    try {
      await docker.restart(b.service);
    } catch (e) {
      const reason = (e as Error).message;
      audit(db, name, 'restart', `${b.service} failed: ${reason}`, now());
      return reply.status(502).send({ error: reason });
    }
    audit(db, name, 'restart', b.service, now());
    return { ok: true };
  });

  api.post('/backup', async (req, reply) => {
    try {
      const r = await master.backup();
      audit(db, req.session!.name, 'backup', r.name, now());
      return r;
    } catch (e) {
      return reply.status(502).send({ error: (e as Error).message });
    }
  });

  api.get('/backups', async () => master.backups());

  // ---- Fabricator moderation: the master does the work, `who` is the signed-in admin ----
  const masterDown = (reply: { status(n: number): { send(b: unknown): unknown } }, e: unknown) =>
    reply.status(502).send({ error: logSafeText((e as Error).message) });

  api.get('/fabricator/reports', async (_req, reply) => {
    try { return await master.fabReports(); } catch (e) { return masterDown(reply, e); }
  });

  api.get('/fabricator/stats', async (_req, reply) => {
    try { return await master.fabStats(); } catch (e) { return masterDown(reply, e); }
  });

  // The mods view: a state filter and a piece of the name (or the id), then one mod's details.
  api.get('/fabricator/mods', async (req, reply) => {
    const { state, q } = req.query as { state?: unknown; q?: unknown };
    if (state !== undefined && state !== '' && !(FAB_STATES as readonly unknown[]).includes(state)) return reply.status(400).send({ error: 'Unknown state.' });
    try {
      return await master.fabMods({ state: typeof state === 'string' && state !== '' ? state : undefined, q: typeof q === 'string' ? q.slice(0, 100) : '' });
    } catch (e) { return masterDown(reply, e); }
  });

  api.get('/fabricator/mods/:id', async (req, reply) => {
    const { id } = req.params as { id: string };
    if (!/^\d{1,9}$/.test(id)) return reply.status(404).send({ error: 'No such mod.' });
    let m;
    try { m = await master.fabMod(Number(id)); } catch (e) { return masterDown(reply, e); }
    if (!m) return reply.status(404).send({ error: 'No such mod.' });
    return m;
  });

  api.post('/fabricator/mods/:id/:action', async (req, reply) => {
    const { id, action } = req.params as { id: string; action: string };
    const modId = Number(id);
    if (!isFabAction(action) || !Number.isSafeInteger(modId) || modId < 1) return reply.status(400).send({ error: 'Unknown action.' });
    if ((req.body as { confirm?: unknown } | undefined)?.confirm !== true) return reply.status(400).send({ error: 'Confirm the action.' });
    const name = req.session!.name;
    let ok: boolean | 'no files';
    try { ok = await master.fabAction(modId, action, name); } catch (e) { return masterDown(reply, e); }
    if (ok === 'no files') return reply.status(409).send({ error: 'No files left: this mod cannot be restored.' });
    if (!ok) return reply.status(404).send({ error: 'No such mod, or it is not in a state for that.' });
    audit(db, name, FAB_AUDIT[action], `mod ${modId}`, now());
    return { ok: true };
  });

  api.post('/fabricator/creators/:tag/publishing', async (req, reply) => {
    const { tag } = req.params as { tag: string };
    const b = (req.body ?? {}) as { allowed?: unknown; confirm?: unknown };
    if (typeof b.allowed !== 'boolean') return reply.status(400).send({ error: 'Say whether publishing is allowed.' });
    if (b.confirm !== true) return reply.status(400).send({ error: 'Confirm the action.' });
    const name = req.session!.name;
    let ok: boolean;
    try { ok = await master.fabPublishing(tag, b.allowed, name); } catch (e) { return masterDown(reply, e); }
    if (!ok) return reply.status(404).send({ error: 'No account has that tag.' });
    audit(db, name, 'fab_publishing', `${tag} ${b.allowed ? 'allowed' : 'stopped'}`, now());
    return { ok: true };
  });

  api.get('/audit', async () =>
    db.prepare('SELECT id, time, name, action, detail FROM audit ORDER BY id DESC LIMIT 200').all());
}
