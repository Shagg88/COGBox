import crypto from 'node:crypto';
import { DatabaseInstance } from './db.js';

// A client's signaling token: 32 random bytes as hex, handed out once at claim and stored only as its SHA-256.
export function newToken(): string {
  return crypto.randomBytes(32).toString('hex');
}

export function hashToken(token: string): string {
  return crypto.createHash('sha256').update(token).digest('hex');
}

export interface AuthedUser {
  uuid: string;
  xuid: string;
  display_name: string;
  discriminator: string;
}

export function checkToken(db: DatabaseInstance, uuid: unknown, token: unknown): AuthedUser | null {
  if (typeof uuid !== 'string' || typeof token !== 'string' || !/^[0-9a-f]{64}$/.test(token)) {
    return null;
  }
  const row = db.prepare('SELECT uuid, xuid, display_name, discriminator, token_hash FROM users WHERE uuid = ?')
    .get(uuid) as any;
  if (!row) {
    return null;
  }
  const a = Buffer.from(row.token_hash, 'hex');
  const b = Buffer.from(hashToken(token), 'hex');
  if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) {
    return null;
  }
  return { uuid: row.uuid, xuid: row.xuid, display_name: row.display_name, discriminator: row.discriminator };
}

const recentHeaders = new WeakMap<DatabaseInstance, Map<string, { user: AuthedUser; until: number }>>();

// A deleted account's header must stop working at once, not when its 1 s memory runs out.
export function forgetAuth(db: DatabaseInstance, uuid: string): void {
  const known = recentHeaders.get(db);
  if (!known) return;
  for (const [header, entry] of known) if (entry.user.uuid === uuid) known.delete(header);
}

// "Authorization: COGBox <uuid>:<token>"
export function checkAuthHeader(db: DatabaseInstance, header: unknown): AuthedUser | null {
  if (typeof header !== 'string') {
    return null;
  }
  // One request checks its header up to three times (rate limit, presence, route): remember a good answer for 1 s.
  let known = recentHeaders.get(db);
  if (!known) recentHeaders.set(db, known = new Map());
  const now = Date.now();
  const hit = known.get(header);
  if (hit && hit.until > now) return hit.user;
  const m = /^COGBox ([^:\s]+):([0-9a-f]{64})$/.exec(header.trim());
  const user = m ? checkToken(db, m[1], m[2]) : null;
  if (user) {
    if (known.size >= 50000) known.clear();
    known.set(header, { user, until: now + 1000 });
  }
  return user;
}
