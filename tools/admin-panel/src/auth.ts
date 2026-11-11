import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';
import type { FastifyReply, FastifyRequest } from 'fastify';
import type { Db } from './db.js';
import { checkTotp, hashPassword, verifyPassword } from './crypto.js';

export const COOKIE = 'cogbox_admin';
export const ABSOLUTE_MS = 12 * 60 * 60_000;
export const IDLE_MS = 30 * 60_000;
export const MAX_FAILED = 5;
export const LOCK_MS = 15 * 60_000;
export const BAD_LOGIN = 'Wrong name, password or code.';
// The typed name of an unknown account is not stored: it may be a password typed in the wrong field.
export const UNKNOWN_NAME = '(unknown name)';
export const LOCKED = 'Locked for 15 minutes after 5 failed sign-ins.';

// Routes a session with must_change set may still use.
const MUST_CHANGE_OK = new Set(['/admin/api/me', '/admin/api/password', '/admin/api/logout']);

export interface Session { name: string; csrf: string; must_change: boolean }

declare module 'fastify' {
  interface FastifyRequest { session?: Session; sessionKey?: string }
}

interface UserRow { name: string; pw_hash: string; totp_secret: string; must_change: number; failed: number; locked_until: number; last_totp_step: number }

// The table holds only a hash of the id, so a leaked database can't be replayed as cookies.
export const sessionKey = (id: string) => createHash('sha256').update(id).digest('hex');

function sameText(a: string, b: string): boolean {
  const x = Buffer.from(a), y = Buffer.from(b);
  return x.length === y.length && timingSafeEqual(x, y);
}

export type LoginResult =
  | { ok: true; id: string; must_change: boolean }
  | { ok: false; locked: boolean; name: string }; // name is UNKNOWN_NAME for an unknown account

// A hash to verify against for unknown names, so they take as long as known ones.
let dummyHash: Promise<string> | undefined;

export async function login(db: Db, body: unknown, now: number): Promise<LoginResult> {
  const b = (body ?? {}) as Record<string, unknown>;
  const str = (v: unknown) => (typeof v === 'string' ? v : '');
  const name = str(b.name).slice(0, 64), password = str(b.password), code = str(b.code);
  const user = db.prepare('SELECT * FROM users WHERE name = ?').get(name) as UserRow | undefined;

  if (user && user.locked_until > now) return { ok: false, locked: true, name };

  dummyHash ??= hashPassword(randomBytes(16).toString('hex'));
  const pwOk = await verifyPassword(user ? user.pw_hash : await dummyHash, password);
  const step = user ? checkTotp(user.totp_secret, code, now, user.last_totp_step) : null;

  // Claim the step in one statement: the row read above is stale after the await, and a concurrent
  // login with the same code (or a lock set meanwhile) must make this one fail.
  const claimed = user && pwOk && step !== null && password && code &&
    db.prepare('UPDATE users SET failed = 0, locked_until = 0, last_totp_step = ? WHERE name = ? AND last_totp_step < ? AND locked_until <= ?')
      .run(step, name, step, now).changes === 1;

  if (!claimed) {
    if (user) {
      // Counted in SQL so parallel failures all count.
      const { failed } = db.prepare('UPDATE users SET failed = failed + 1 WHERE name = ? RETURNING failed').get(name) as { failed: number };
      if (failed >= MAX_FAILED) db.prepare('UPDATE users SET failed = 0, locked_until = ? WHERE name = ?').run(now + LOCK_MS, name);
    }
    return { ok: false, locked: false, name: user ? name : UNKNOWN_NAME };
  }

  const id = randomBytes(32).toString('hex');
  db.prepare('INSERT INTO sessions (id, name, csrf, created, seen) VALUES (?, ?, ?, ?, ?)')
    .run(sessionKey(id), name, randomBytes(32).toString('hex'), now, now);
  return { ok: true, id, must_change: user.must_change === 1 };
}

export function makeRequireSession(db: Db, now: () => number) {
  return async function requireSession(req: FastifyRequest, reply: FastifyReply) {
    const id = req.cookies[COOKIE];
    if (!id) return reply.status(401).send({ error: 'Not signed in.' });
    const key = sessionKey(id);
    const s = db.prepare(`SELECT sessions.name, sessions.csrf, sessions.created, sessions.seen, users.must_change
      FROM sessions JOIN users ON users.name = sessions.name WHERE sessions.id = ?`).get(key) as
      { name: string; csrf: string; created: number; seen: number; must_change: number } | undefined;
    const t = now();
    if (!s || t - s.created > ABSOLUTE_MS || t - s.seen > IDLE_MS) {
      if (s) db.prepare('DELETE FROM sessions WHERE id = ?').run(key);
      return reply.status(401).send({ error: 'Not signed in.' });
    }
    if (req.method !== 'GET' && req.method !== 'HEAD' && !sameText(String(req.headers['x-csrf-token'] ?? ''), s.csrf)) {
      return reply.status(403).send({ error: 'Missing or wrong CSRF token.' });
    }
    const path = req.routeOptions.url ?? req.url.split('?')[0];
    if (s.must_change && !MUST_CHANGE_OK.has(path)) {
      return reply.status(403).send({ error: 'Change your password first.' });
    }
    db.prepare('UPDATE sessions SET seen = ? WHERE id = ?').run(t, key);
    req.session = { name: s.name, csrf: s.csrf, must_change: s.must_change === 1 };
    req.sessionKey = key;
  };
}
