import { describe, it, expect } from 'vitest';
import { checkTotp, hashPassword, newTotpSecret, otpauthUrl, totpCode, verifyPassword } from '../src/crypto.js';

const SECRET = 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'; // RFC 6238 test key "12345678901234567890"

describe('totp', () => {
  it('matches RFC 6238 vector', () => {
    expect(totpCode(SECRET, 1)).toBe('287082'); // t=59 s
  });

  it('accepts ±1 step only', () => {
    const now = 59 * 1000; // step 1
    expect(checkTotp(SECRET, totpCode(SECRET, 1), now, 0)).toBe(1);
    expect(checkTotp(SECRET, totpCode(SECRET, 2), now, 0)).toBe(2);
    expect(checkTotp(SECRET, totpCode(SECRET, 0), now, -1)).toBe(0);
    expect(checkTotp(SECRET, totpCode(SECRET, 3), now, 0)).toBeNull();
    expect(checkTotp(SECRET, totpCode(SECRET, 4), 6 * 30 * 1000, 0)).toBeNull(); // cur-2
    expect(checkTotp(SECRET, totpCode(SECRET, 5), 6 * 30 * 1000, 0)).toBe(5); // cur-1
    expect(checkTotp(SECRET, totpCode(SECRET, 4), 200 * 1000, 0)).toBeNull();
  });

  it('refuses a step at or before lastStep', () => {
    const now = 59 * 1000;
    expect(checkTotp(SECRET, totpCode(SECRET, 1), now, 1)).toBeNull();
    expect(checkTotp(SECRET, totpCode(SECRET, 0), now, 1)).toBeNull();
    expect(checkTotp(SECRET, totpCode(SECRET, 2), now, 1)).toBe(2);
  });

  it('makes a 20 byte base32 secret and an otpauth url', () => {
    const s = newTotpSecret();
    expect(s).toMatch(/^[A-Z2-7]{32}$/);
    const url = otpauthUrl('alice', s);
    expect(url).toContain('otpauth://totp/');
    expect(url).toContain('issuer=COGBox%20Server');
    expect(url).toContain(`secret=${s}`);
  });
});

describe('password', () => {
  it('round trip and wrong password', async () => {
    const h = await hashPassword('correct horse battery');
    expect(h).toContain('$argon2id$');
    expect(await verifyPassword(h, 'correct horse battery')).toBe(true);
    expect(await verifyPassword(h, 'wrong horse battery')).toBe(false);
  });
});
