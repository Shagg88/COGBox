import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import yazl from 'yazl';
import sharp from 'sharp';
import { FAB_DEFAULTS } from '../src/fabricator/settings.js';
import { checkModZip, checkScreenshot, checkText, MOD_API_LEVEL, type UploadMeta } from '../src/fabricator/checks.js';

let dir: string;
let n = 0;
beforeAll(() => { dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabchk-')); });
afterAll(() => { fs.rmSync(dir, { recursive: true, force: true }); });

function makeZip(entries: Record<string, string | Buffer>, opts: Record<string, any> = {}): Promise<string> {
  const file = path.join(dir, `z${n++}.zip`);
  const z = new yazl.ZipFile();
  const patches: [string, string][] = [];   // yazl refuses hostile names: write a same-length stand-in, patch the bytes after
  let k = 0;
  for (const [name, data] of Object.entries(entries)) {
    let use = name;
    if (/^[\\/]|\.\.|:|\\/.test(name)) {
      use = 'Q' + String(k++).padStart(3, '0') + 'z'.repeat(Math.max(0, name.length - 4)); use = use.slice(0, name.length);
      patches.push([use, name]);
    }
    z.addBuffer(Buffer.from(data), use, { forceZip64Format: false, ...(opts[name] ?? {}) });
  }
  z.end();
  return new Promise((res, rej) => {
    const chunks: Buffer[] = [];
    z.outputStream.on('data', (c: Buffer) => chunks.push(c));
    z.outputStream.on('error', rej);
    z.outputStream.on('end', () => {
      let b = Buffer.concat(chunks);
      for (const [from, to] of patches) {
        let i = 0;
        while ((i = b.indexOf(from, i)) >= 0) { b.write(to, i, 'latin1'); i += from.length; }
      }
      fs.writeFileSync(file, b); res(file);
    });
  });
}

const S = FAB_DEFAULTS;
const bad = async (entries: Record<string, string | Buffer>, word: string, s = S, game = 'gears3', opts = {}) => {
  const r = await checkModZip(await makeZip(entries, opts), game, s);
  expect(r.ok).toBe(false);
  expect(r.problems.join(' | ')).toContain(word);
};

describe('checkModZip', () => {
  it('accepts a good zip', async () => {
    const r = await checkModZip(await makeZip({
      'GearGame/CookedXbox360/A.xxx': 'abc',
      'mod.ini': '; c\r\n[Mod]\r\nname=Horde Bank\r\nGAME=gears3\r\nRequires=Other Mod\r\nApi=1\r\nSide=Client\r\n',
    }), 'gears3', S);
    expect(r.problems).toEqual([]);
    expect(r.ok).toBe(true);
    expect(r.modIni?.name).toBe('Horde Bank');
    expect(r.modIni?.requires).toEqual(['Other Mod']);
    expect(r.modIni?.api).toBe(1);
    expect(r.files).toBe(2);
    expect(r.unpackedBytes).toBeGreaterThan(3);
  });
  it('finds mod.ini one folder down', async () => {
    const r = await checkModZip(await makeZip({ 'Folder/mod.ini': '[Mod]\nName=X\n', 'Folder/a.xxx': 'a' }), 'gears3', S);
    expect(r.modIni?.name).toBe('X');
  });
  it('rejects a non-zip', async () => {
    const f = path.join(dir, 'nz.zip'); fs.writeFileSync(f, 'not a zip at all');
    const r = await checkModZip(f, 'gears3', S);
    expect(r.ok).toBe(false); expect(r.problems.join()).toContain('zip');
  });
  it('rejects an encrypted entry', async () => {
    const f = await makeZip({ 'a.xxx': 'abc' });
    const b = fs.readFileSync(f);
    // set the encryption bit (bit 0) in the general purpose flags of the local and central headers
    for (let i = 0; i < b.length - 4; i++) {
      if (b.readUInt32LE(i) === 0x04034b50) b[i + 6] |= 1;
      if (b.readUInt32LE(i) === 0x02014b50) b[i + 8] |= 1;
    }
    fs.writeFileSync(f, b);
    const r = await checkModZip(f, 'gears3', S);
    expect(r.ok).toBe(false); expect(r.problems.join()).toContain('encrypted');
  });
  for (const p of ['../x', '/abs', 'C:/x', '//server/x', 'a/../../x', 'a\\..\\..\\x'])
    it(`rejects path ${p}`, () => bad({ [p]: 'x' }, 'path'));
  it('rejects a symlink entry', () => bad({ 'lnk': 'target' }, 'link', S, 'gears3', { lnk: { mode: 0o120777 } }));
  for (const p of ['bin/tool.dll', 'run.bat', 'a.LNK', 'x/y.Exe', 'a.xexp'])
    it(`rejects program ${p}`, () => bad({ [p]: 'x' }, 'program'));
  it('rejects too big unpacked', () => bad({ 'a.xxx': 'x'.repeat(100) }, 'size', { ...S, maxUnpackedBytes: 10 }));
  it('rejects too many files', () => bad({ 'a': '1', 'b': '2', 'c': '3' }, 'files', { ...S, maxFiles: 2 }));
  it('rejects wrong game', () => bad({ 'mod.ini': '[Mod]\nGame=gears2\n' }, 'game'));
  it('rejects newer api', () => bad({ 'mod.ini': `[Mod]\nApi=${MOD_API_LEVEL + 1}\n` }, 'newer'));
});

describe('checkScreenshot', () => {
  const mk = (w: number, h: number, fmt: 'png' | 'jpeg' = 'png') =>
    sharp({ create: { width: w, height: h, channels: 3, background: '#336699' } })[fmt]().toBuffer();
  it('passes a png and re-encodes', async () => {
    const r = await checkScreenshot(await mk(2, 2));
    expect(r.ok).toBe(true);
    if (r.ok) { expect(r.width).toBe(2); expect(r.png.subarray(1, 4).toString()).toBe('PNG'); }
  });
  it('passes a jpeg and returns png', async () => {
    const r = await checkScreenshot(await mk(8, 8, 'jpeg'));
    expect(r.ok).toBe(true);
    if (r.ok) expect(r.png.subarray(1, 4).toString()).toBe('PNG');
  });
  it('rejects random bytes', async () => {
    const r = await checkScreenshot(Buffer.from('definitely not an image, just text'));
    expect(r.ok).toBe(false); if (!r.ok) expect(r.problem).toContain('image');
  });
  it('rejects too large dimensions', async () => {
    const r = await checkScreenshot(await mk(4000, 10));
    expect(r.ok).toBe(false); if (!r.ok) expect(r.problem).toContain('size');
  });
  it('rejects over 4 MB', async () => {
    const r = await checkScreenshot(Buffer.alloc(4 * 1024 * 1024 + 1));
    expect(r.ok).toBe(false); if (!r.ok) expect(r.problem).toContain('size');
  });
});

describe('checkText', () => {
  const ok: UploadMeta = { name: 'Good Mod', game: 'gears3', type: 'gameplay', version: '1.0', short_desc: 's', long_desc: 'l', tags: ['a'] };
  it('accepts good meta', () => expect(checkText(ok)).toEqual([]));
  it('short name', () => expect(checkText({ ...ok, name: 'ab' }).length).toBeGreaterThan(0));
  it('9 tags', () => expect(checkText({ ...ok, tags: Array(9).fill('t') }).length).toBeGreaterThan(0));
  it('long tag', () => expect(checkText({ ...ok, tags: ['x'.repeat(25)] }).length).toBeGreaterThan(0));
  it('unknown game', () => expect(checkText({ ...ok, game: 'gears9' }).length).toBeGreaterThan(0));
  it('unknown type', () => expect(checkText({ ...ok, type: 'nope' }).length).toBeGreaterThan(0));
  it('long fields', () => {
    expect(checkText({ ...ok, short_desc: 'x'.repeat(121) }).length).toBe(1);
    expect(checkText({ ...ok, long_desc: 'x'.repeat(8001) }).length).toBe(1);
    expect(checkText({ ...ok, version: 'x'.repeat(25) }).length).toBe(1);
    expect(checkText({ ...ok, notes: 'x'.repeat(2001) }).length).toBe(1);
  });
});

describe('checkModZip hardening', () => {
  for (const p of ['evil.dll.', 'evil.dll ', 'evil.dll:stream', 'x/evil.exe. .'])
    it(`rejects windows-normalised name "${p}"`, async () => {
      const r = await checkModZip(await makeZip({ [p]: 'x' }), 'gears3', S);
      expect(r.ok).toBe(false);
      expect(r.problems.join()).toMatch(/program|path/);
    });
  for (const p of ['.. /x', '.../x', 'a/. ./x'])
    it(`rejects segment "${p}"`, () => bad({ [p]: 'x' }, 'path'));
  it('refuses an entry that inflates beyond its declared size', async () => {
    const f = await makeZip({ 'a.xxx': Buffer.alloc(100000, 0x41) });
    const b = fs.readFileSync(f);
    for (let i = 0; i < b.length - 4; i++) {
      if (b.readUInt32LE(i) === 0x04034b50) b.writeUInt32LE(10, i + 22);
      if (b.readUInt32LE(i) === 0x02014b50) b.writeUInt32LE(10, i + 24);
    }
    fs.writeFileSync(f, b);
    const r = await checkModZip(f, 'gears3', S);
    expect(r.ok).toBe(false);
    expect(r.problems.join()).toContain('size');
  });
  it('counts real inflated bytes against the limit', () =>
    bad({ 'a.xxx': Buffer.alloc(5000, 1), 'b.xxx': Buffer.alloc(5000, 2) }, 'size', { ...S, maxUnpackedBytes: 6000 }));
  it('mod.ini without Game counts as gears3', async () => {
    const z = await makeZip({ 'mod.ini': '[Mod]\nName=Nogame\n' });
    const r2 = await checkModZip(z, 'gears2', S);
    expect(r2.ok).toBe(false); expect(r2.problems.join()).toContain('game');
    const r3 = await checkModZip(z, 'gears3', S);
    expect(r3.ok).toBe(true); expect(r3.modIni?.game).toBe('gears3');
  });
  it('no mod.ini passes for any game', async () => {
    const r = await checkModZip(await makeZip({ 'a.xxx': 'a' }), 'gears2', S);
    expect(r.ok).toBe(true);
  });
});

describe('checkModZip: Windows device names and names that differ only in case', () => {
  for (const p of ['aux.txt', 'GearGame/CON', 'dir/com1.ini', 'Lpt9', 'nul.tar.gz', 'COM¹.dat', 'x/lpt³', 'prn .txt', 'AUX/readme.txt', 'con.'])
    it(`rejects device name "${p}"`, () => bad({ [p]: 'x' }, 'device name'));
  for (const p of ['console.txt', 'auxiliary.ini', 'com10.txt', 'com0.txt', 'lptx.bin', 'nulls/a.bin', 'my.con'])
    it(`accepts "${p}"`, async () => {
      const r = await checkModZip(await makeZip({ [p]: 'x' }), 'gears3', S);
      expect(r.problems).toEqual([]);
    });
  it('rejects two files whose paths differ only in case', () => bad({ 'Textures/Rock.dds': 'a', 'textures/ROCK.DDS': 'b' }, 'differ only in case'));
  it('rejects a file and a folder with the same name in another case', () => bad({ 'Data': 'a', 'data/x.bin': 'b' }, 'differ only in case'));
  it('accepts a folder entry next to its files', async () => {
    const file = path.join(dir, `z${n++}.zip`);
    const z = new yazl.ZipFile();
    z.addEmptyDirectory('Data');
    z.addBuffer(Buffer.from('b'), 'Data/x.bin');
    z.addBuffer(Buffer.from('c'), 'data/y.bin');   // the same folder in another case is fine
    z.end();
    await new Promise<void>((res, rej) => z.outputStream.pipe(fs.createWriteStream(file)).on('close', res).on('error', rej));
    const r = await checkModZip(file, 'gears3', S);
    expect(r.problems).toEqual([]);
  });
});
