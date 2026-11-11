import { describe, it, expect } from 'vitest';
import { openDb } from '../src/db.js';
import { addUser, removeUser, resetUser, setPassword } from '../src/users.js';
import { verifyPassword } from '../src/crypto.js';

const row = (db: any, name: string) => db.prepare('SELECT * FROM users WHERE name = ?').get(name);

describe('users', () => {
  it('addUser then resetUser changes secret and unlocks', async () => {
    const db = openDb(':memory:');
    const a = await addUser(db, 'alice');
    expect(a.password).toHaveLength(16);
    const first = row(db, 'alice');
    expect(first.must_change).toBe(1);
    expect(await verifyPassword(first.pw_hash, a.password)).toBe(true);

    db.prepare('UPDATE users SET failed = 5, locked_until = 99999999999999, must_change = 0 WHERE name = ?').run('alice');
    const r = await resetUser(db, 'alice');
    const second = row(db, 'alice');
    expect(second.totp_secret).not.toBe(first.totp_secret);
    expect(second.failed).toBe(0);
    expect(second.locked_until).toBe(0);
    expect(second.must_change).toBe(1);
    expect(await verifyPassword(second.pw_hash, r.password)).toBe(true);
    expect(r.otpauth).toContain(second.totp_secret);

    expect(removeUser(db, 'alice')).toBe(true);
    expect(removeUser(db, 'alice')).toBe(false);
  });

  it('setPassword refuses under 12', async () => {
    const db = openDb(':memory:');
    await addUser(db, 'bob');
    await expect(setPassword(db, 'bob', 'short')).rejects.toThrow();
    await setPassword(db, 'bob', 'twelve chars!');
    const r = row(db, 'bob');
    expect(await verifyPassword(r.pw_hash, 'twelve chars!')).toBe(true);
    expect(r.must_change).toBe(0);
  });
});
