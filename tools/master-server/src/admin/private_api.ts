import Fastify, { FastifyInstance } from 'fastify';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { DatabaseInstance } from '../db.js';
import { Registry } from '../social/registry.js';
import { ErrorRing } from './error_ring.js';

export interface PrivateApiDeps {
  db: DatabaseInstance;
  registry: Registry;
  hostChanged: (uuid: string) => void;
  pendingInvites: () => number;
  errors: ErrorRing;
  dataDir: string;
  secret: string;
  startedAt: number;
}

const KEEP_BACKUPS = 14;
const BACKUP_NAME = /^master-\d{14}\.db$/;

// read from the working directory (the server starts in its own folder)
const version = (): string => {
  try {
    return JSON.parse(fs.readFileSync(path.resolve('package.json'), 'utf8')).version;
  } catch {
    return 'unknown';
  }
};

const stamp = (t: Date): string => t.toISOString().slice(0, 19).replace(/[-T:]/g, '');

// The admin panel's side door: only the panel's own process reaches it (the port is not published), and it must
// still know the shared secret. It never returns a uuid, xuid or token: players show as tags.
export async function buildPrivateApi(deps: PrivateApiDeps): Promise<FastifyInstance> {
  const { db, registry, errors, dataDir } = deps;
  const app = Fastify({ logger: false });
  const secret = Buffer.from(deps.secret);
  const backupDir = path.join(dataDir, 'backups');

  app.addHook('onRequest', async (req, reply) => {
    const given = Buffer.from(String(req.headers['x-admin-secret'] ?? ''));
    if (given.length !== secret.length || !crypto.timingSafeEqual(given, secret)) {
      return reply.status(401).send({ error: 'unauthorized' });
    }
  });

  const backups = () => {
    if (!fs.existsSync(backupDir)) return [];
    return fs.readdirSync(backupDir).filter((n) => BACKUP_NAME.test(n)).sort().reverse().map((name) => {
      const st = fs.statSync(path.join(backupDir, name));
      return { name, size: st.size, mtime: st.mtimeMs };
    });
  };

  app.get('/status', async () => {
    const by_title = registry.titleCounts();
    return {
      uptime_s: Math.floor((Date.now() - deps.startedAt) / 1000),
      version: version(),
      players: { total: Object.values(by_title).reduce((a, b) => a + b, 0), by_title, by_region: registry.regionCounts() },
      lobbies: (db.prepare('SELECT count(*) AS n FROM lobbies').get() as { n: number }).n,
      pending_invites: deps.pendingInvites(),
      errors: errors.last(20)
    };
  });

  app.get('/lobbies', async () => {
    const rows = db.prepare(`
      SELECT lobbies.lobby_id, lobbies.room_code, lobbies.title_id, lobbies.is_public, lobbies.created_at,
             users.display_name, users.discriminator
      FROM lobbies LEFT JOIN users ON users.uuid = lobbies.host_uuid ORDER BY lobbies.created_at DESC`).all() as any[];
    return {
      lobbies: rows.map((r) => ({
        lobby_id: r.lobby_id, room_code: r.room_code, title_id: r.title_id,
        host_tag: r.display_name ? `${r.display_name}#${r.discriminator}` : '',
        is_public: Boolean(r.is_public), created_at: r.created_at
      }))
    };
  });

  app.post<{ Params: { id: string } }>('/lobbies/:id/close', async (req) => {
    const row = db.prepare('SELECT host_uuid FROM lobbies WHERE lobby_id = ?').get(req.params.id) as { host_uuid: string } | undefined;
    if (!row) return { closed: false };
    db.prepare('DELETE FROM lobbies WHERE lobby_id = ?').run(req.params.id);
    deps.hostChanged(row.host_uuid);   // as when the host deletes it: the host's friends see the party gone
    return { closed: true };
  });

  app.post('/backup', async () => {
    fs.mkdirSync(backupDir, { recursive: true });
    // a name per second, always later than the newest one (pruning and the list sort by name)
    let t = Date.now();
    const newest = backups()[0]?.name.match(/\d{14}/)?.[0];
    if (newest) {
      const n = Date.UTC(+newest.slice(0, 4), +newest.slice(4, 6) - 1, +newest.slice(6, 8), +newest.slice(8, 10), +newest.slice(10, 12), +newest.slice(12, 14));
      if (n >= t - (t % 1000)) t = n + 1000;
    }
    const name = `master-${stamp(new Date(t))}.db`;
    await db.backup(path.join(backupDir, name));
    for (const old of backups().slice(KEEP_BACKUPS)) fs.rmSync(path.join(backupDir, old.name), { force: true });
    return { name, size: fs.statSync(path.join(backupDir, name)).size };
  });

  app.get('/backups', async () => ({ backups: backups() }));

  return app;
}
