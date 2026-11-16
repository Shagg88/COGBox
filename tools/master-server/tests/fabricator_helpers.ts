import { createHash, randomBytes } from 'node:crypto';
import yazl from 'yazl';
import sharp from 'sharp';

// Generated test content only: no mod file or screenshot is ever committed.
export const sha256 = (b: Buffer): string => createHash('sha256').update(b).digest('hex');

// A small mod zip: a mod.ini plus `bulk` random stored bytes (so its size is predictable).
export function modZip(ini: string, bulk = 64, extra: Record<string, string | Buffer> = {}): Promise<Buffer> {
  const z = new yazl.ZipFile();
  z.addBuffer(Buffer.from(ini), 'mod.ini');
  z.addBuffer(randomBytes(bulk), 'GearGame/CookedPCConsole/data.bin', { compress: false });
  for (const [name, data] of Object.entries(extra)) z.addBuffer(Buffer.from(data), name);
  z.end();
  return new Promise((res, rej) => {
    const chunks: Buffer[] = [];
    z.outputStream.on('data', (c: Buffer) => chunks.push(c));
    z.outputStream.on('error', rej);
    z.outputStream.on('end', () => res(Buffer.concat(chunks)));
  });
}

export const png = (w = 4, h = 4): Promise<Buffer> =>
  sharp({ create: { width: w, height: h, channels: 3, background: '#336699' } }).png().toBuffer();

export const meta = (over: Record<string, unknown> = {}) => ({
  name: 'Horde Bank', game: 'gears3', type: 'gameplay', version: '1.0', short_desc: 'Bank your horde cash',
  long_desc: 'Keeps your money between waves.', tags: ['horde', 'economy'], side: 'everyone', notes: '', ...over,
});
