import type { Db } from './db.js';
import { hashPassword, newTotpSecret, otpauthUrl, randomPassword } from './crypto.js';

export interface Credentials { password: string; otpauth: string }

export async function addUser(db: Db, name: string): Promise<Credentials> {
  const password = randomPassword();
  const secret = newTotpSecret();
  db.prepare('INSERT INTO users (name, pw_hash, totp_secret, must_change, failed, locked_until, last_totp_step) VALUES (?, ?, ?, 1, 0, 0, 0)')
    .run(name, await hashPassword(password), secret);
  return { password, otpauth: otpauthUrl(name, secret) };
}

export function removeUser(db: Db, name: string): boolean {
  db.prepare('DELETE FROM sessions WHERE name = ?').run(name);
  return db.prepare('DELETE FROM users WHERE name = ?').run(name).changes > 0;
}

export async function resetUser(db: Db, name: string): Promise<Credentials> {
  const password = randomPassword();
  const secret = newTotpSecret();
  const r = db.prepare('UPDATE users SET pw_hash = ?, totp_secret = ?, must_change = 1, failed = 0, locked_until = 0, last_totp_step = 0 WHERE name = ?')
    .run(await hashPassword(password), secret, name);
  if (r.changes === 0) throw new Error(`no such user: ${name}`);
  db.prepare('DELETE FROM sessions WHERE name = ?').run(name);
  return { password, otpauth: otpauthUrl(name, secret) };
}

export async function setPassword(db: Db, name: string, pw: string): Promise<void> {
  if (pw.length < 12) throw new Error('password must be at least 12 characters');
  db.prepare('UPDATE users SET pw_hash = ?, must_change = 0 WHERE name = ?').run(await hashPassword(pw), name);
}
