import yauzl from 'yauzl';
import sharp from 'sharp';
import type { FabSettings } from './settings.js';

/** The newest mod API COGBox knows; keep in step with modcore's kModApiLevel. */
export const MOD_API_LEVEL = 1;

export const GAMES = ['gears1', 'gears2', 'gears3', 'judgment'] as const;
export const TYPES = ['texture_pack', 'campaign', 'gameplay', 'visuals', 'audio', 'other'] as const;

const REFUSED_EXT = new Set(['.exe', '.dll', '.xex', '.xexp', '.bat', '.cmd', '.ps1', '.vbs', '.js', '.msi', '.scr', '.com', '.lnk']);
const MAX_INI_BYTES = 256 * 1024;
const MAX_SHOT_BYTES = 4 * 1024 * 1024;
const MAX_SHOT_W = 3840, MAX_SHOT_H = 2160;

export interface ModIniInfo { name?: string; game?: string; api?: number; requires: string[]; side?: string }

export interface UploadMeta {
  name: string; game: string; type: string; version: string; short_desc: string; long_desc: string;
  tags: string[]; side?: string; notes?: string; requires?: number[];
}

export interface ZipCheck { ok: boolean; problems: string[]; modIni: ModIniInfo | null; unpackedBytes: number; files: number }

/** mod.ini with modcore's rules: keys case-insensitive, `;` / `#` comment lines, `Requires` comma separated. */
export function parseModIni(text: string): ModIniInfo {
  const info: ModIniInfo = { requires: [], game: 'gears3' };   // modcore: no Game key means gears3
  let section = '';
  text = text.replace(/^﻿/, '');
  for (let line of text.split(/\r?\n/)) {
    line = line.trim();
    if (!line || line[0] === ';' || line[0] === '#') continue;
    if (line[0] === '[' && line[line.length - 1] === ']') { section = line.slice(1, -1).trim().toLowerCase(); continue; }
    if (section !== '' && section !== 'mod') continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim().toLowerCase();
    const value = line.slice(eq + 1).trim();
    if (key === 'name') info.name = value;
    else if (key === 'game') info.game = value.toLowerCase() || 'gears3';
    else if (key === 'api') { const n = parseInt(value, 10); if (!Number.isNaN(n)) info.api = n; }
    else if (key === 'side') info.side = value.toLowerCase();
    else if (key === 'requires') for (const r of value.split(',')) { const t = r.trim(); if (t) info.requires.push(t); }
  }
  return info;
}

function badPath(name: string): boolean {
  if (name.includes('\0')) return true;
  if (name.includes(':')) return true;   // drive letters and NTFS alternate data streams
  const p = name.replace(/\\/g, '/');
  if (p.startsWith('/')) return true;
  // Windows drops trailing dots and spaces from a name, so ".. " and "..." are really ".."
  return p.split('/').some((seg) => seg !== '' && seg !== '.' && seg.replace(/[. ]+$/, '') === '');
}

export function checkModZip(file: string, game: string, s: FabSettings): Promise<ZipCheck> {
  return new Promise((resolve) => {
    const problems: string[] = [];
    let unpacked = 0, files = 0, iniEntry: yauzl.Entry | null = null, iniDepth = 99;
    let refusedShown = 0;
    const toInflate: yauzl.Entry[] = [];
    const note = (p: string) => { if (!problems.includes(p)) problems.push(p); };
    const finish = (modIni: ModIniInfo | null) => {
      if (modIni) {
        if (modIni.game && modIni.game !== game) note(`mod.ini is for game ${modIni.game}, but this is uploaded as ${game}: wrong game`);
        if (modIni.api !== undefined && modIni.api > MOD_API_LEVEL) note(`mod.ini needs Api ${modIni.api}, which is newer than this store supports (${MOD_API_LEVEL})`);
      }
      resolve({ ok: problems.length === 0, problems, modIni, unpackedBytes: unpacked, files });
    };
    yauzl.open(file, { lazyEntries: true, validateEntrySizes: true, decodeStrings: false, autoClose: false }, (err, zip) => {
      if (err || !zip) { note('not a valid zip file'); return finish(null); }
      let stopped = false;
      const stop = () => { if (!stopped) { stopped = true; zip.close(); finish(null); } };
      zip.on('error', () => { note('the zip is damaged: not a valid zip file'); stop(); });
      zip.on('entry', (e: yauzl.Entry) => {
        if (stopped) return;
        const name = Buffer.isBuffer(e.fileName) ? (e.fileName as Buffer).toString('utf8') : String(e.fileName);
        files++;
        if (files > s.maxFiles) { note(`too many files: more than ${s.maxFiles} files in the zip`); return stop(); }
        if (e.isEncrypted()) note(`encrypted entry: ${name}`);
        if (badPath(name)) note('unsafe path in the zip (outside the mod folder)');
        const unixMode = (e.externalFileAttributes >>> 16) & 0xf000;
        if (unixMode === 0xa000) note('the zip contains a link (symlink), which is not allowed');
        const isDir = name.endsWith('/');
        if (!isDir) {
          const base = (name.replace(/\\/g, '/').split('/').pop() ?? '').replace(/[. ]+$/, '');
          const dot = base.lastIndexOf('.');
          const ext = dot >= 0 ? base.slice(dot).toLowerCase() : '';
          if (REFUSED_EXT.has(ext) && refusedShown++ < 5) note(`program file not allowed (${ext}): ${base}`);
          const low = name.replace(/\\/g, '/').toLowerCase();
          const depth = low === 'mod.ini' ? 0 : /^[^/]+\/mod\.ini$/.test(low) ? 1 : 99;
          if (depth < iniDepth) { iniDepth = depth; iniEntry = e; }
        }
        unpacked += e.uncompressedSize;
        if (!isDir && !e.isEncrypted()) toInflate.push(e);
        if (unpacked > s.maxUnpackedBytes) { note(`unpacked size is over the limit of ${s.maxUnpackedBytes} bytes`); return stop(); }
        zip.readEntry();
      });
      zip.on('end', () => {
        if (stopped) return;
        stopped = true;
        const e = iniEntry as yauzl.Entry | null;
        const readIni = () => {
        if (!e) { zip.close(); return finish(null); }
        if (e.uncompressedSize > MAX_INI_BYTES) { zip.close(); note('mod.ini is too large'); return finish(null); }
        zip.openReadStream(e, (rerr, stream) => {
          if (rerr || !stream) { zip.close(); if (!e.isEncrypted()) note('mod.ini cannot be read'); return finish(null); }
          const chunks: Buffer[] = [];
          stream.on('data', (c: Buffer) => chunks.push(c));
          stream.on('error', () => { note('mod.ini cannot be read'); zip.close(); finish(null); });
          stream.on('end', () => { zip.close(); finish(parseModIni(Buffer.concat(chunks).toString('utf8'))); });
        });
        };
        // second pass: really inflate every entry and count the true bytes (declared sizes can lie)
        if (problems.length > 0) return readIni();
        let real = 0, idx = 0;
        const next = (): void => {
          if (idx >= toInflate.length) return readIni();
          const entry = toInflate[idx++];
          zip.openReadStream(entry, (ierr, st) => {
            if (ierr || !st) { note('an entry cannot be unpacked: size or data is invalid'); zip.close(); return finish(null); }
            let over = false;
            st.on('data', (c: Buffer) => {
              real += c.length;
              if (!over && real > s.maxUnpackedBytes) {
                over = true; st.destroy();
                note(`unpacked size is over the limit of ${s.maxUnpackedBytes} bytes`); zip.close(); finish(null);
              }
            });
            st.on('error', () => {
              if (over) return;
              note('an entry unpacks to a different size than declared (size mismatch) or is damaged'); zip.close(); finish(null);
            });
            st.on('end', () => { if (!over) next(); });
          });
        };
        next();
      });
      zip.readEntry();
    });
  });
}

export async function checkScreenshot(bytes: Buffer): Promise<{ ok: true; png: Buffer; width: number; height: number } | { ok: false; problem: string }> {
  if (bytes.length > MAX_SHOT_BYTES) return { ok: false, problem: 'screenshot size is over 4 MB' };
  try {
    const img = sharp(bytes, { limitInputPixels: MAX_SHOT_W * MAX_SHOT_H * 4 });
    const meta = await img.metadata();
    if (meta.format !== 'png' && meta.format !== 'jpeg') return { ok: false, problem: 'screenshot is not a PNG or JPEG image' };
    const w = meta.width ?? 0, h = meta.height ?? 0;
    if (w < 1 || h < 1 || w > MAX_SHOT_W || h > MAX_SHOT_H) return { ok: false, problem: `screenshot size ${w}x${h} is over ${MAX_SHOT_W}x${MAX_SHOT_H}` };
    const { data, info } = await img.png().toBuffer({ resolveWithObject: true });   // re-encode: metadata is dropped
    return { ok: true, png: data, width: info.width, height: info.height };
  } catch {
    return { ok: false, problem: 'screenshot is not a readable image' };
  }
}

export function checkText(m: UploadMeta): string[] {
  const p: string[] = [];
  const name = (m.name ?? '').trim();
  if (name.length < 3 || name.length > 60) p.push('name must be 3 to 60 characters');
  if (!(GAMES as readonly string[]).includes(m.game)) p.push('unknown game');
  if (!(TYPES as readonly string[]).includes(m.type)) p.push('unknown type');
  if ((m.version ?? '').length > 24) p.push('version text is over 24 characters');
  if ((m.short_desc ?? '').length > 120) p.push('short description is over 120 characters');
  if ((m.long_desc ?? '').length > 8000) p.push('description is over 8000 characters');
  if ((m.notes ?? '').length > 2000) p.push('"what\'s new" is over 2000 characters');
  const tags = m.tags ?? [];
  if (tags.length > 8) p.push('more than 8 tags');
  if (tags.some((t) => t.length > 24)) p.push('a tag is over 24 characters');
  return p;
}
