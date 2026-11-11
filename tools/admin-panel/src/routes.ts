import type { FastifyInstance } from 'fastify';
import type { Db } from './db.js';
import type { DockerClient, MasterClient } from './types.js';
import { audit } from './audit.js';
import { logSafeText } from './scrub.js';

type Svc = 'master-server' | 'coturn';
const isSvc = (s: unknown): s is Svc => s === 'master-server' || s === 'coturn';
const LOG_LINES = 300;

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

  api.get('/audit', async () =>
    db.prepare('SELECT id, time, name, action, detail FROM audit ORDER BY id DESC LIMIT 200').all());
}
