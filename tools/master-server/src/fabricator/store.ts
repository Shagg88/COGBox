import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { pipeline } from 'node:stream/promises';

// Fabricator file store: upload chunks, assembled mod zips and screenshots on disk.
// Layout under root: chunks/<uploadId>/<n>, staging/<uploadId>.zip, files/<xx>/<sha256>.zip, images/<sha256>.png.
const UPLOAD_ID = /^[A-Za-z0-9_-]{8,64}$/;
const KEY = /^[0-9a-f]{64}\.(zip|png)$/;
const SHA = /^[0-9a-f]{64}$/;

export class FabStore {
  constructor(private readonly root: string) {
    for (const d of ['chunks', 'staging', 'files', 'images']) fs.mkdirSync(path.join(root, d), { recursive: true });
  }

  private checkId(uploadId: string): string {
    if (typeof uploadId !== 'string' || !UPLOAD_ID.test(uploadId)) throw new Error('bad upload id');
    return uploadId;
  }

  private chunkDir(uploadId: string): string {
    return path.join(this.root, 'chunks', this.checkId(uploadId));
  }

  private stagingPath(uploadId: string): string {
    return path.join(this.root, 'staging', `${this.checkId(uploadId)}.zip`);
  }

  chunkPath(uploadId: string, n: number): string {
    if (!Number.isInteger(n) || n < 0) throw new Error('bad chunk number');
    return path.join(this.chunkDir(uploadId), String(n));
  }

  async writeChunk(uploadId: string, n: number, data: Buffer): Promise<void> {
    const file = this.chunkPath(uploadId, n);
    await fs.promises.mkdir(path.dirname(file), { recursive: true });
    await fs.promises.writeFile(file, data);
  }

  // Joins chunks 0..chunks-1 by streaming, hashing as it writes; never holds a whole file in memory.
  async assemble(uploadId: string, chunks: number): Promise<{ file: string; size: number; sha256: string }> {
    if (!Number.isInteger(chunks) || chunks < 1) throw new Error('bad chunk count');
    const file = this.stagingPath(uploadId);
    const hash = createHash('sha256');
    let size = 0;
    const out = fs.createWriteStream(file);
    try {
      for (let i = 0; i < chunks; i++) {
        const src = fs.createReadStream(this.chunkPath(uploadId, i));
        src.on('data', (b) => { hash.update(b); size += (b as Buffer).length; });
        await pipeline(src, out, { end: false });
      }
      await new Promise<void>((res, rej) => { out.once('error', rej); out.end(res); });
    } catch (e) {
      out.destroy();
      await fs.promises.rm(file, { force: true });
      throw e;
    }
    return { file, size, sha256: hash.digest('hex') };
  }

  // Moves a staged zip into content-addressed storage; identical content keeps one file.
  commit(stagingFile: string, sha256: string): string {
    if (!SHA.test(sha256)) throw new Error('bad sha256');
    const key = `${sha256}.zip`;
    const dest = this.pathOf(key);
    if (fs.existsSync(dest)) {
      fs.rmSync(stagingFile, { force: true });
      return key;
    }
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.renameSync(stagingFile, dest);
    return key;
  }

  pathOf(key: string): string {
    if (typeof key !== 'string' || !KEY.test(key)) throw new Error('bad key');
    if (key.endsWith('.png')) return path.join(this.root, 'images', key);
    return path.join(this.root, 'files', key.slice(0, 2), key);
  }

  remove(key: string): void {
    fs.rmSync(this.pathOf(key), { force: true });
  }

  saveImage(png: Buffer): string {
    const key = `${createHash('sha256').update(png).digest('hex')}.png`;
    const dest = this.pathOf(key);
    if (!fs.existsSync(dest)) fs.writeFileSync(dest, png);
    return key;
  }

  dropUpload(uploadId: string): void {
    fs.rmSync(this.chunkDir(uploadId), { recursive: true, force: true });
    fs.rmSync(this.stagingPath(uploadId), { force: true });
  }

  async disk(): Promise<{ total: number; free: number }> {
    const s = await fs.promises.statfs(this.root);
    return { total: Number(s.blocks) * Number(s.bsize), free: Number(s.bavail) * Number(s.bsize) };
  }
}
