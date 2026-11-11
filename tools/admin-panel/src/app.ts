import Fastify, { type FastifyInstance } from 'fastify';
import cookie from '@fastify/cookie';
import rateLimit from '@fastify/rate-limit';
import fastifyStatic from '@fastify/static';
import { join } from 'node:path';
import type { Db } from './db.js';
import type { DockerClient, MasterClient } from './types.js';
import { audit } from './audit.js';
import { setPassword } from './users.js';
import { registerRoutes } from './routes.js';
import { ABSOLUTE_MS, BAD_LOGIN, COOKIE, LOCKED, login, makeRequireSession } from './auth.js';

export interface AppOptions {
  db: Db;
  now?: () => number;
  master: MasterClient;
  docker: DockerClient;
  secureCookie?: boolean;
}

export async function buildApp(opts: AppOptions): Promise<FastifyInstance> {
  const { db } = opts;
  const now = opts.now ?? Date.now;
  const cookieOpts = { path: '/admin', httpOnly: true, secure: opts.secureCookie ?? true, sameSite: 'strict' as const };

  // No logger: request logs would carry client addresses. One proxy hop (caddy): the address it adds is the
  // one used for rate limiting; anything further left in X-Forwarded-For is client-supplied.
  const app = Fastify({ logger: false, trustProxy: (_addr: string, hop: number) => hop < 1 });
  await app.register(cookie);
  // In-memory only; the address is the key and never stored or logged.
  await app.register(rateLimit, { global: false });

  // Pages: plain files, no inline script or style, so the CSP can stay at 'self'.
  app.addHook('onSend', async (_req, reply) => {
    reply.header('Content-Security-Policy', "default-src 'self'; frame-ancestors 'none'");
    reply.header('X-Content-Type-Options', 'nosniff');
  });
  await app.register(fastifyStatic, { root: join(__dirname, '..', 'public'), prefix: '/admin/',
    // never a page for anything that decodes to the API (e.g. /admin/api%2Faudit)
    allowedPath: (pathName: string) => !/^\/?api(\/|$)/i.test(pathName),
  });
  app.get('/admin', (_req, reply) => reply.redirect('/admin/'));

  const requireSession = makeRequireSession(db, now);
  // Every API route lives in this one encapsulated instance, so the session hook follows routing itself: a route
  // reached by any spelling of its path (percent-encoded or not) is still behind it. Login is the only exemption.
  await app.register(async (api) => {
    api.addHook('preHandler', async (req, reply) => {
      if ((req.routeOptions.config as { public?: boolean }).public === true) return;
      return requireSession(req, reply);
    });

    api.post('/login', { config: { public: true, rateLimit: { max: 10, timeWindow: '1 minute' } } }, async (req, reply) => {
      const t = now();
      const r = await login(db, req.body, t);
      if (!r.ok) {
        audit(db, r.name, 'login_failed', r.locked ? 'locked' : '', t);
        return r.locked ? reply.status(423).send({ error: LOCKED }) : reply.status(401).send({ error: BAD_LOGIN });
      }
      const name = (req.body as { name: string }).name;
      audit(db, name, 'login', '', t);
      reply.setCookie(COOKIE, r.id, { ...cookieOpts, maxAge: ABSOLUTE_MS / 1000 });
      return { must_change: r.must_change };
    });

    api.get('/me', async (req) => {
      const s = req.session!;
      return { name: s.name, csrf: s.csrf, must_change: s.must_change };
    });

    api.post('/password', async (req, reply) => {
      const pw = (req.body as { password?: unknown } | undefined)?.password;
      if (typeof pw !== 'string' || pw.length < 12) return reply.status(400).send({ error: 'The password must be at least 12 characters.' });
      const s = req.session!;
      await setPassword(db, s.name, pw);
      // other sessions of this user end; this one stays
      db.prepare('DELETE FROM sessions WHERE name = ? AND id != ?').run(s.name, req.sessionKey);
      audit(db, s.name, 'password_changed', '', now());
      return { ok: true };
    });

    api.post('/logout', async (req, reply) => {
      db.prepare('DELETE FROM sessions WHERE id = ?').run(req.sessionKey);
      audit(db, req.session!.name, 'logout', '', now());
      reply.clearCookie(COOKIE, cookieOpts);
      return { ok: true };
    });

    registerRoutes(api, { db, now, master: opts.master, docker: opts.docker });
    // unknown API paths stop here (401 without a session, from the hook) instead of reaching the static files
    api.all('/*', async (_req, reply) => reply.status(404).send({ error: 'Not found.' }));
  }, { prefix: '/admin/api' });
  return app;
}
