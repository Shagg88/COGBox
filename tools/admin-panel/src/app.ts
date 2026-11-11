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
  await app.register(fastifyStatic, { root: join(__dirname, '..', 'public'), prefix: '/admin/' });
  app.get('/admin', (_req, reply) => reply.redirect('/admin/'));

  const requireSession = makeRequireSession(db, now);
  // Default-deny: every /admin/api route except login needs a session, including ones added later.
  app.addHook('preHandler', async (req, reply) => {
    const path = req.routeOptions.url ?? req.url.split('?')[0];
    if (!path.startsWith('/admin/api/') || path === '/admin/api/login') return;
    return requireSession(req, reply);
  });

  app.post('/admin/api/login', { config: { rateLimit: { max: 10, timeWindow: '1 minute' } } }, async (req, reply) => {
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

  app.get('/admin/api/me', async (req) => {
    const s = req.session!;
    return { name: s.name, csrf: s.csrf, must_change: s.must_change };
  });

  app.post('/admin/api/password', async (req, reply) => {
    const pw = (req.body as { password?: unknown } | undefined)?.password;
    if (typeof pw !== 'string' || pw.length < 12) return reply.status(400).send({ error: 'The password must be at least 12 characters.' });
    const s = req.session!;
    await setPassword(db, s.name, pw);
    // other sessions of this user end; this one stays
    db.prepare('DELETE FROM sessions WHERE name = ? AND id != ?').run(s.name, req.sessionKey);
    audit(db, s.name, 'password_changed', '', now());
    return { ok: true };
  });

  app.post('/admin/api/logout', async (req, reply) => {
    db.prepare('DELETE FROM sessions WHERE id = ?').run(req.sessionKey);
    audit(db, req.session!.name, 'logout', '', now());
    reply.clearCookie(COOKIE, cookieOpts);
    return { ok: true };
  });

  registerRoutes(app, { db, now, master: opts.master, docker: opts.docker });
  return app;
}
