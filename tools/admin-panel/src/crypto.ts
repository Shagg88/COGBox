import { createHmac, randomBytes, randomInt } from 'node:crypto';
import { hash, verify } from '@node-rs/argon2';

const ARGON2ID = 2; // @node-rs/argon2 const enum value; avoids importing a const enum under isolated modules
const B32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
const STEP_MS = 30_000;

export function hashPassword(pw: string): Promise<string> {
  return hash(pw, { algorithm: ARGON2ID });
}

export async function verifyPassword(h: string, pw: string): Promise<boolean> {
  try {
    return await verify(h, pw);
  } catch {
    return false;
  }
}

function b32encode(buf: Buffer): string {
  let bits = 0, value = 0, out = '';
  for (const b of buf) {
    value = (value << 8) | b;
    bits += 8;
    while (bits >= 5) {
      out += B32[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += B32[(value << (5 - bits)) & 31];
  return out;
}

function b32decode(s: string): Buffer {
  let bits = 0, value = 0;
  const out: number[] = [];
  for (const c of s.toUpperCase().replace(/=+$/, '')) {
    const i = B32.indexOf(c);
    if (i < 0) throw new Error('bad base32');
    value = (value << 5) | i;
    bits += 5;
    if (bits >= 8) {
      out.push((value >>> (bits - 8)) & 255);
      bits -= 8;
    }
  }
  return Buffer.from(out);
}

export function newTotpSecret(): string {
  return b32encode(randomBytes(20));
}

export function totpCode(secret: string, step: number): string {
  const counter = Buffer.alloc(8);
  counter.writeBigUInt64BE(BigInt(step));
  const h = createHmac('sha1', b32decode(secret)).update(counter).digest();
  const o = h[h.length - 1] & 15;
  const n = ((h[o] & 0x7f) << 24) | (h[o + 1] << 16) | (h[o + 2] << 8) | h[o + 3];
  return String(n % 1_000_000).padStart(6, '0');
}

// Returns the matched step, or null. A step at or before lastStep is refused so a code works once.
export function checkTotp(secret: string, code: string, now: number, lastStep: number): number | null {
  const cur = Math.floor(now / STEP_MS);
  for (const step of [cur - 1, cur, cur + 1]) {
    if (step > lastStep && step >= 0 && totpCode(secret, step) === code) return step;
  }
  return null;
}

export function otpauthUrl(name: string, secret: string): string {
  const issuer = encodeURIComponent('COGBox Server');
  return `otpauth://totp/${issuer}:${encodeURIComponent(name)}?secret=${secret}&issuer=${issuer}&algorithm=SHA1&digits=6&period=30`;
}

export function randomPassword(): string {
  const chars = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789';
  let out = '';
  for (let i = 0; i < 16; i++) out += chars[randomInt(chars.length)];
  return out;
}
