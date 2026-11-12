import dgram from 'node:dgram';
import { randomBytes } from 'node:crypto';
import type { Line } from './types.js';

const MAGIC = 0x2112a442;

// RFC 5389 Binding Request: type 0x0001, length 0, magic cookie, 12-byte transaction id
export function bindingRequest(): Buffer {
  const b = Buffer.alloc(20);
  b.writeUInt16BE(0x0001, 0);
  b.writeUInt16BE(0, 2);
  b.writeUInt32BE(MAGIC, 4);
  randomBytes(12).copy(b, 8);
  return b;
}

export function isBindingSuccess(res: Buffer, req: Buffer): boolean {
  return res.length >= 20 && res.readUInt16BE(0) === 0x0101 && res.readUInt32BE(4) === MAGIC && res.subarray(8, 20).equals(req.subarray(8, 20));
}

export function checkStun(host: string, port: number, timeoutMs = 3000): Promise<Line> {
  const name = `STUN (UDP ${port})`;
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4');
    const req = bindingRequest();
    const t0 = performance.now();
    let settled = false;   // a second reply or a late error must not close the socket twice (that throws)
    const done = (l: Line) => { if (settled) return; settled = true; clearTimeout(t); try { sock.close(); } catch { /* already closed */ } resolve(l); };
    const t = setTimeout(() => done({ name, state: 'down', text: 'no answer to a binding request', core: true }), timeoutMs);
    sock.on('message', (m) => { if (isBindingSuccess(m, req)) done({ name, state: 'up', text: 'answering', core: true, ms: Math.round(performance.now() - t0) }); });
    sock.on('error', () => done({ name, state: 'down', text: 'socket error', core: true }));
    sock.send(req, port, host);
  });
}
