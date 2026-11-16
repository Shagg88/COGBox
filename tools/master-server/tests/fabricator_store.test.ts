import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { FabStore } from '../src/fabricator/store.js';

// File store on an OS temp folder (Fabricator design, task 3).
describe('fabricator store', () => {
  let root: string;
  let store: FabStore;
  const sha = (b: Buffer) => createHash('sha256').update(b).digest('hex');

  beforeEach(() => {
    root = fs.mkdtempSync(path.join(os.tmpdir(), 'fabstore-'));
    store = new FabStore(root);
  });
  afterEach(() => { fs.rmSync(root, { recursive: true, force: true }); });

  it('assembles chunks into the joined bytes with their sha256', async () => {
    const a = Buffer.from('hello '); const b = Buffer.from('world');
    await store.writeChunk('upload-0001', 0, a);
    await store.writeChunk('upload-0001', 1, b);
    const r = await store.assemble('upload-0001', 2);
    expect(r.size).toBe(11);
    expect(r.sha256).toBe(sha(Buffer.concat([a, b])));
    expect(fs.readFileSync(r.file)).toEqual(Buffer.concat([a, b]));
    expect(r.file).toBe(path.join(root, 'staging', 'upload-0001.zip'));
  });

  it('fails to assemble when a chunk is missing', async () => {
    await store.writeChunk('upload-0002', 0, Buffer.from('x'));
    await expect(store.assemble('upload-0002', 2)).rejects.toThrow(/ENOENT/);
    expect(fs.existsSync(path.join(root, 'staging', 'upload-0002.zip'))).toBe(false);
  });

  it('commit moves the file and pathOf finds it', async () => {
    await store.writeChunk('upload-0003', 0, Buffer.from('zipbytes'));
    const r = await store.assemble('upload-0003', 1);
    const key = store.commit(r.file, r.sha256);
    expect(key).toBe(`${r.sha256}.zip`);
    expect(fs.existsSync(store.pathOf(key))).toBe(true);
    expect(store.pathOf(key)).toBe(path.join(root, 'files', r.sha256.slice(0, 2), key));
    expect(fs.existsSync(r.file)).toBe(false);
  });

  it('same content committed twice gives one file and one key', async () => {
    const keys: string[] = [];
    for (const id of ['upload-0004', 'upload-0005']) {
      await store.writeChunk(id, 0, Buffer.from('same'));
      const r = await store.assemble(id, 1);
      keys.push(store.commit(r.file, r.sha256));
      expect(fs.existsSync(r.file)).toBe(false);
    }
    expect(keys[0]).toBe(keys[1]);
    const dir = path.dirname(store.pathOf(keys[0]));
    expect(fs.readdirSync(dir)).toEqual([keys[0]]);
  });

  it('pathOf refuses keys that are not a sha256 zip/png', () => {
    expect(() => store.pathOf('../x')).toThrow();
    expect(() => store.pathOf('a/b.zip')).toThrow();
    expect(() => store.pathOf(`${'A'.repeat(64)}.zip`)).toThrow();
    expect(() => store.pathOf(`${'a'.repeat(64)}.exe`)).toThrow();
  });

  it('refuses unsafe upload ids and bad chunk numbers', async () => {
    await expect(store.writeChunk('../evil-upload', 0, Buffer.from('x'))).rejects.toThrow();
    expect(() => store.chunkPath('short', 0)).toThrow();
    expect(() => store.dropUpload('a/../../bad-bad-bad')).toThrow();
    expect(() => store.chunkPath('upload-0006', -1)).toThrow();
    expect(() => store.chunkPath('upload-0006', 1.5)).toThrow();
  });

  it('dropUpload removes the chunks and any staged file', async () => {
    await store.writeChunk('upload-0007', 0, Buffer.from('a'));
    await store.assemble('upload-0007', 1);
    store.dropUpload('upload-0007');
    expect(fs.existsSync(store.chunkPath('upload-0007', 0))).toBe(false);
    expect(fs.existsSync(path.join(root, 'staging', 'upload-0007.zip'))).toBe(false);
    store.dropUpload('upload-0007'); // already gone: fine
  });

  it('remove deletes a committed file, tolerating a missing one', async () => {
    await store.writeChunk('upload-0008', 0, Buffer.from('gone'));
    const r = await store.assemble('upload-0008', 1);
    const key = store.commit(r.file, r.sha256);
    store.remove(key);
    expect(fs.existsSync(store.pathOf(key))).toBe(false);
    store.remove(key);
  });

  it('saveImage stores a png under its sha256, once', () => {
    const png = Buffer.from('fakepngbytes');
    const key = store.saveImage(png);
    expect(key).toBe(`${sha(png)}.png`);
    expect(store.saveImage(png)).toBe(key);
    expect(fs.readFileSync(store.pathOf(key))).toEqual(png);
    expect(store.pathOf(key)).toBe(path.join(root, 'images', key));
  });

  it('disk reports total and free bytes', async () => {
    const d = await store.disk();
    expect(d.total).toBeGreaterThan(0);
    expect(d.free).toBeGreaterThan(0);
    expect(d.free).toBeLessThanOrEqual(d.total);
  });

  it('assembles many chunks without piling listeners on the output stream', async () => {
    const warnings: Error[] = [];
    const on = (w: Error) => warnings.push(w);
    process.on('warning', on);
    try {
      const parts = Array.from({ length: 40 }, (_, i) => Buffer.alloc(100 + i, i));
      for (let i = 0; i < parts.length; i++) await store.writeChunk('upload-0040', i, parts[i]);
      const r = await store.assemble('upload-0040', parts.length);
      expect(r.sha256).toBe(sha(Buffer.concat(parts)));
      expect(r.size).toBe(Buffer.concat(parts).length);
      await new Promise((res) => setTimeout(res, 20));
    } finally { process.off('warning', on); }
    expect(warnings.filter((w) => w.name === 'MaxListenersExceededWarning')).toEqual([]);
  });
});
