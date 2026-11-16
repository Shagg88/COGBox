import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { initDb, type DatabaseInstance } from '../src/db.js';
import { FAB_DEFAULTS, type FabSettings } from '../src/fabricator/settings.js';
import { FabStore } from '../src/fabricator/store.js';
import {
  canPublish, startUpload, putChunk, putScreen, finishUpload, expireUploads, startFinish, getUpload, recoverInterruptedFinishes,
  removeOwnMod, forgetFabricatorAccount, creatorLimits, UPLOAD_BUSY,
} from '../src/fabricator/publish.js';
import { modZip, png, meta, sha256 } from './fabricator_helpers.js';

const DAY = 24 * 3600 * 1000;
const GiB = 2 ** 30;
const NOW = 1_800_000_000_000;
const DISK = { total: 1000 * GiB, free: 500 * GiB };

describe('fabricator publishing rules', () => {
  let dir: string;
  let db: DatabaseInstance;
  let store: FabStore;
  let s: FabSettings;

  const addUser = (uuid: string, name: string, disc: string, createdAt = NOW - 10 * DAY) =>
    db.prepare(`INSERT INTO users (uuid, display_name, discriminator, xuid, token_hash, created_at, last_seen)
      VALUES (?, ?, ?, ?, ?, ?, ?)`).run(uuid, name, disc, `x-${uuid}`, '00'.repeat(32), createdAt, createdAt);

  // The whole session as the routes drive it: start, chunks in order, one screenshot, finish.
  async function publish(account: string, zip: Buffer, m = meta(), modId: number | null = null, opts: { hash?: string; shot?: boolean } = {}) {
    const up = startUpload(db, account, m as any, modId, zip.length, opts.hash ?? sha256(zip), NOW, s);
    for (let n = 0; n < up.chunks; n++) {
      const part = zip.subarray(n * up.chunkBytes, (n + 1) * up.chunkBytes);
      expect(await putChunk(db, store, up.id, n, part, sha256(part))).toBe('ok');
    }
    if (opts.shot !== false) expect(await putScreen(db, store, up.id, 0, await png())).toBeNull();
    return { up, r: await finishUpload(db, store, s, up.id, account, NOW) };
  }

  beforeEach(() => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fabpub-'));
    db = initDb(':memory:');
    store = new FabStore(path.join(dir, 'fabricator'));
    s = { ...FAB_DEFAULTS, chunkBytes: 1024, creatorUploadsPerDay: 100, diskFullFraction: 1 };   // finish reads the real disk
    addUser('acc-a', 'Baird', '0001');
    addUser('acc-b', 'Cole', '0002');
  });
  afterEach(() => { db.close(); fs.rmSync(dir, { recursive: true, force: true }); });

  describe('canPublish', () => {
    it('allows an established account', () => {
      expect(canPublish(db, s, 'acc-a', 1000, NOW, DISK)).toBeNull();
    });
    it('refuses an account created 1 day ago', () => {
      addUser('acc-new', 'Jace', '0003', NOW - DAY);
      expect(canPublish(db, s, 'acc-new', 1000, NOW, DISK)).toBe('account too new');
    });
    it('refuses an account whose publishing was stopped', () => {
      db.prepare('INSERT INTO fab_creators (account, publishing, bytes_used) VALUES (?, 0, 0)').run('acc-a');
      expect(canPublish(db, s, 'acc-a', 1000, NOW, DISK)).toBe('publishing stopped');
    });
    it('refuses past the creator quota', () => {
      db.prepare('INSERT INTO fab_creators (account, publishing, bytes_used) VALUES (?, 1, ?)').run('acc-a', s.creatorQuotaBytes - 500);
      expect(canPublish(db, s, 'acc-a', 400, NOW, DISK)).toBeNull();
      expect(canPublish(db, s, 'acc-a', 1000, NOW, DISK)).toBe('quota');
    });
    it('refuses a fourth upload in a day', async () => {
      s = { ...s, creatorUploadsPerDay: 3 };
      for (let i = 0; i < 3; i++) {
        const { r } = await publish('acc-a', await modZip(`[Mod]\nName=Mod ${i}\n`), meta({ name: `Mod ${i}` }));
        expect(r.ok).toBe(true);
      }
      expect(canPublish(db, s, 'acc-a', 1000, NOW, DISK)).toBe('daily limit');
      expect(canPublish(db, s, 'acc-a', 1000, NOW + DAY + 1, DISK)).toBeNull();
      expect(canPublish(db, s, 'acc-b', 1000, NOW, DISK)).toBeNull();
    });
    it('counts an upload in progress towards the daily limit', () => {
      s = { ...s, creatorUploadsPerDay: 1 };
      startUpload(db, 'acc-a', meta() as any, null, 100, 'ab'.repeat(32), NOW, s);
      expect(canPublish(db, s, 'acc-a', 1000, NOW, DISK)).toBe('daily limit');
    });
    it('refuses past the server-wide daily caps', async () => {
      const { r } = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'));
      expect(r.ok).toBe(true);
      expect(canPublish(db, { ...s, globalUploadsPerDay: 1 }, 'acc-b', 10, NOW, DISK)).toBe('server daily limit');
      expect(canPublish(db, { ...s, globalUploadBytesPerDay: 2000 }, 'acc-b', 1990, NOW, DISK)).toBe('server daily limit');
      expect(canPublish(db, { ...s, globalUploadsPerDay: 1 }, 'acc-b', 10, NOW + DAY + 1, DISK)).toBeNull();
    });
    it('counts open sessions against the creator quota', () => {
      s = { ...s, creatorQuotaBytes: 20_000 };
      db.prepare('INSERT INTO fab_creators (account, publishing, bytes_used) VALUES (?, 1, ?)').run('acc-a', 19_000);
      expect(canPublish(db, s, 'acc-a', 600, NOW, DISK)).toBeNull();
      startUpload(db, 'acc-a', meta() as any, null, 600, 'ab'.repeat(32), NOW, s);
      expect(canPublish(db, s, 'acc-a', 600, NOW, DISK)).toBe('quota');
      expect(canPublish(db, s, 'acc-a', 600, NOW + s.sessionTtlMs + 1, DISK)).toBeNull();   // the session expired
    });
    it('counts open sessions twice against the disk', () => {
      s = { ...s, diskFullFraction: 0.85 };
      const disk = { total: 100_000, free: 30_000 };   // 70% used
      expect(canPublish(db, s, 'acc-a', 1000, NOW, disk)).toBeNull();
      startUpload(db, 'acc-b', meta() as any, null, 7000, 'ab'.repeat(32), NOW, s);   // 70% + 2 x (7% + 1%) = 86%
      expect(canPublish(db, s, 'acc-a', 1000, NOW, disk)).toBe('server full');
    });
    it('refuses when the disk is over 85% used', () => {
      s = { ...s, diskFullFraction: 0.85 };
      expect(canPublish(db, s, 'acc-a', 1000, NOW, { total: 100 * GiB, free: 14 * GiB })).toBe('server full');
      expect(canPublish(db, s, 'acc-a', 1000, NOW, { total: 100 * GiB, free: 20 * GiB })).toBeNull();
    });
    it('refuses a file over maxFileBytes', () => {
      expect(canPublish(db, s, 'acc-a', s.maxFileBytes + 1, NOW, DISK)).toBe('file too big');
    });
  });

  describe('finishUpload', () => {
    it('publishes a live mod findable by search', async () => {
      const zip = await modZip('[Mod]\nName=Horde Bank\nGame=gears3\n');
      const { up, r } = await publish('acc-a', zip);
      expect(r).toEqual({ ok: true, modId: expect.any(Number), versionId: expect.any(Number) });
      if (!r.ok) return;
      const mod = db.prepare('SELECT * FROM fab_mods WHERE id = ?').get(r.modId) as any;
      expect(mod).toMatchObject({ owner: 'acc-a', name: 'Horde Bank', game: 'gears3', type: 'gameplay', state: 'live', slug: 'horde-bank' });
      expect(JSON.parse(mod.tags)).toEqual(['horde', 'economy']);
      const ver = db.prepare('SELECT * FROM fab_versions WHERE id = ?').get(r.versionId) as any;
      expect(ver).toMatchObject({ mod_id: r.modId, version: '1.0', size: zip.length, sha256: sha256(zip), state: 'live' });
      expect(fs.readFileSync(store.pathOf(ver.file_key))).toEqual(zip);
      const screens = db.prepare('SELECT * FROM fab_screens WHERE mod_id = ?').all(r.modId) as any[];
      expect(screens).toHaveLength(1);
      expect(screens[0].image_key).toMatch(/^[0-9a-f]{64}\.png$/);
      expect(fs.existsSync(store.pathOf(screens[0].image_key))).toBe(true);
      const hits = (q: string) => (db.prepare('SELECT rowid FROM fab_mods_fts WHERE fab_mods_fts MATCH ?').all(q) as any[]).map((x) => x.rowid);
      expect(hits('horde')).toEqual([r.modId]);
      expect(hits('economy')).toEqual([r.modId]);
      expect(hits('"Baird#0001"')).toEqual([r.modId]);
      expect((db.prepare('SELECT bytes_used FROM fab_creators WHERE account = ?').get('acc-a') as any).bytes_used).toBe(zip.length);
      // the session stays as 'done' (its outcome readable until it expires); its chunks are gone
      expect(getUpload(db, up.id)).toMatchObject({ state: 'done', mod_id: r.modId, version_id: r.versionId });
      expect(fs.existsSync(path.join(dir, 'fabricator', 'chunks', up.id))).toBe(false);
      expect(db.prepare('SELECT COUNT(*) AS n FROM fab_upload_screens WHERE upload_id = ?').get(up.id)).toEqual({ n: 0 });
    });

    it('refuses a name another creator uses for the same game, not for another game', async () => {
      expect((await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'))).r.ok).toBe(true);
      const taken = await publish('acc-b', await modZip('[Mod]\nName=horde bank\n'), meta({ name: 'horde bank' }));
      expect(taken.r.ok).toBe(false);
      if (!taken.r.ok) expect(taken.r.problems.join(' | ')).toContain('name taken');
      const other = await publish('acc-b', await modZip('[Mod]\nName=horde bank\nGame=gears2\n'), meta({ name: 'horde bank', game: 'gears2' }));
      expect(other.r.ok).toBe(true);
    });

    it('gives a hash problem when the file is not what was announced', async () => {
      const zip = await modZip('[Mod]\nName=Horde Bank\n');
      const { up, r } = await publish('acc-a', zip, meta(), null, { hash: 'ab'.repeat(32) });
      expect(r.ok).toBe(false);
      if (!r.ok) expect(r.problems.join(' | ')).toContain('hash');
      expect(db.prepare('SELECT 1 FROM fab_uploads WHERE id = ?').get(up.id)).toBeTruthy();   // kept: the creator sees why
      expect(fs.existsSync(path.join(dir, 'fabricator', 'staging', `${up.id}.zip`))).toBe(false);
      expect(db.prepare('SELECT COUNT(*) AS n FROM fab_mods').get()).toEqual({ n: 0 });
    });

    it('needs a screenshot and passes on the zip check problems', async () => {
      const noShot = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'), meta(), null, { shot: false });
      expect(noShot.r.ok).toBe(false);
      if (!noShot.r.ok) expect(noShot.r.problems.join(' | ')).toContain('screenshot');
      const badZip = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n', 64, { 'run.exe': 'MZ' }));
      expect(badZip.r.ok).toBe(false);
      if (!badZip.r.ok) expect(badZip.r.problems.join(' | ')).toContain('.exe');
    });

    it('fills requires from mod.ini, matched to live mods of the same game', async () => {
      const base = await publish('acc-b', await modZip('[Mod]\nName=Base Lib\n'), meta({ name: 'Base Lib' }));
      expect(base.r.ok).toBe(true);
      const dep = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\nRequires=base lib, Not In Store\n'));
      expect(dep.r.ok).toBe(true);
      if (!base.r.ok || !dep.r.ok) return;
      const row = db.prepare('SELECT requires FROM fab_mods WHERE id = ?').get(dep.r.modId) as any;
      expect(JSON.parse(row.requires)).toEqual([base.r.modId]);
    });

    it('a new version updates the text; a fourth version deletes the first version file', async () => {
      const zips = await Promise.all([1, 2, 3, 4].map((i) => modZip(`[Mod]\nName=Horde Bank\n; v${i}\n`)));
      const first = await publish('acc-a', zips[0]);
      expect(first.r.ok).toBe(true);
      if (!first.r.ok) return;
      const modId = first.r.modId;
      const versions: number[] = [first.r.versionId];
      for (let i = 1; i < 4; i++) {
        const v = await publish('acc-a', zips[i], meta({ version: `1.${i}`, short_desc: `Now with vaults ${i}` }), modId, { shot: false });
        expect(v.r.ok).toBe(true);
        if (v.r.ok) { expect(v.r.modId).toBe(modId); versions.push(v.r.versionId); }
      }
      const rows = db.prepare('SELECT id, state, file_key FROM fab_versions WHERE mod_id = ? ORDER BY id').all(modId) as any[];
      expect(rows.map((r) => r.state)).toEqual(['files_deleted', 'live', 'live', 'live']);
      expect(fs.existsSync(store.pathOf(rows[0].file_key))).toBe(false);
      expect(fs.existsSync(store.pathOf(rows[3].file_key))).toBe(true);
      const used = (db.prepare('SELECT bytes_used FROM fab_creators WHERE account = ?').get('acc-a') as any).bytes_used;
      expect(used).toBe(zips[1].length + zips[2].length + zips[3].length);
      expect((db.prepare('SELECT rowid FROM fab_mods_fts WHERE fab_mods_fts MATCH ?').all('vaults') as any[]).map((x) => x.rowid)).toEqual([modId]);
      expect(db.prepare('SELECT COUNT(*) AS n FROM fab_screens WHERE mod_id = ?').get(modId)).toEqual({ n: 1 });   // kept
    });

    it("stores modcore's side words", async () => {
      const side = async (name: string, ini: string, m: Record<string, unknown>) => {
        const { r } = await publish('acc-a', await modZip(`[Mod]\nName=${name}\n${ini}`), meta({ name, ...m }));
        if (!r.ok) throw new Error(r.problems.join());
        return (db.prepare('SELECT side FROM fab_mods WHERE id = ?').get(r.modId) as any).side;
      };
      expect(await side('Side Host', 'Side=host\n', { side: undefined })).toBe('host');
      expect(await side('Side Server', 'Side=Server\n', { side: undefined })).toBe('host');
      expect(await side('Side Client', '', { side: 'client' })).toBe('client');
      expect(await side('Side Both', 'Side=client\n', { side: 'both' })).toBe('everyone');
      expect(await side('Side None', '', { side: undefined })).toBe('everyone');
    });

    it("a new version keeps the mod's game", async () => {
      const first = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'));
      if (!first.r.ok) throw new Error('setup');
      const other = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\nGame=gears1\n'), meta({ game: 'gears1' }), first.r.modId);
      expect(other.r.ok).toBe(false);
      if (!other.r.ok) expect(other.r.problems.join(' | ')).toContain('game');
      const wrongZip = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\nGame=gears1\n'), meta(), first.r.modId);
      expect(wrongZip.r.ok).toBe(false);
      if (!wrongZip.r.ok) expect(wrongZip.r.problems.join(' | ')).toContain('game');
      expect((db.prepare('SELECT game FROM fab_mods WHERE id = ?').get(first.r.modId) as any).game).toBe('gears3');
    });

    it('finish checks the quota again', async () => {
      const zip = await modZip('[Mod]\nName=Horde Bank\n');
      const up = startUpload(db, 'acc-a', meta() as any, null, zip.length, sha256(zip), NOW, s);
      expect(await putChunk(db, store, up.id, 0, zip, sha256(zip))).toBe('ok');
      expect(await putScreen(db, store, up.id, 0, await png())).toBeNull();
      db.prepare('INSERT INTO fab_creators (account, publishing, bytes_used) VALUES (?, 1, ?)').run('acc-a', s.creatorQuotaBytes - 10);
      const r = await finishUpload(db, store, s, up.id, 'acc-a', NOW);
      expect(r.ok).toBe(false);
      if (!r.ok) expect(r.problems.join(' | ')).toContain('quota');
    });

    it('a new version must be the caller\'s own live mod', async () => {
      const first = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'));
      if (!first.r.ok) throw new Error('setup');
      expect(() => startUpload(db, 'acc-b', meta() as any, first.r.modId, 100, 'ab'.repeat(32), NOW, s)).toThrow('not your mod');
    });
  });

  describe('the asynchronous finish', () => {
    async function readySession(name = 'Horde Bank', shot = true) {
      const zip = await modZip(`[Mod]\nName=${name}\n`);
      const up = startUpload(db, 'acc-a', meta({ name }) as any, null, zip.length, sha256(zip), NOW, s);
      expect(await putChunk(db, store, up.id, 0, zip, sha256(zip))).toBe('ok');
      if (shot) expect(await putScreen(db, store, up.id, 0, await png())).toBeNull();
      return up;
    }

    it('runs in the background and records done; while it runs a second start and a screenshot are refused', async () => {
      const up = await readySession();
      const a = startFinish(db, store, s, up.id, 'acc-a', NOW);
      if (!('started' in a)) throw new Error('not started');
      expect(getUpload(db, up.id)?.state).toBe('finishing');
      expect(startFinish(db, store, s, up.id, 'acc-a', NOW)).toEqual({ refused: 'finishing' });
      expect(await putScreen(db, store, up.id, 1, await png())).toBe(UPLOAD_BUSY);
      const r = await a.started;
      if (!r.ok) throw new Error(r.problems.join());
      expect(getUpload(db, up.id)).toMatchObject({ state: 'done', mod_id: r.modId, version_id: r.versionId });
      expect(startFinish(db, store, s, up.id, 'acc-a', NOW)).toEqual({ done: { state: 'done', chunks: 1, chunks_done: 1, mod_id: r.modId, version_id: r.versionId } });
      expect(startFinish(db, store, s, up.id, 'acc-b', NOW)).toEqual({ refused: 'not_found' });
      // a done session is no open upload: one upload today, its bytes counted once
      expect(creatorLimits(db, s, 'acc-a', NOW, DISK)).toMatchObject({ uploads_today: 1 });
      expect(canPublish(db, { ...s, creatorUploadsPerDay: 2 }, 'acc-a', 1, NOW, DISK)).toBeNull();
    });

    it('a failed finish keeps the session, its chunks and its problems; it can be finished again', async () => {
      const up = await readySession('Horde Bank', false);
      const a = startFinish(db, store, s, up.id, 'acc-a', NOW);
      if (!('started' in a)) throw new Error('not started');
      expect((await a.started).ok).toBe(false);
      const row = getUpload(db, up.id);
      expect(row?.state).toBe('failed');
      expect(JSON.parse(row?.problems ?? '[]').join()).toContain('screenshot');
      expect(fs.existsSync(path.join(dir, 'fabricator', 'chunks', up.id))).toBe(true);
      expect(await putScreen(db, store, up.id, 0, await png())).toBeNull();
      expect((await finishUpload(db, store, s, up.id, 'acc-a', NOW)).ok).toBe(true);
    });

    it('missing chunks are refused at once, without starting', async () => {
      const up = startUpload(db, 'acc-a', meta() as any, null, 3000, 'ab'.repeat(32), NOW, s);
      expect(startFinish(db, store, s, up.id, 'acc-a', NOW)).toEqual({ problems: ['only 0 of 3 chunks are uploaded'] });
      expect(getUpload(db, up.id)?.state).toBe('open');
    });

    it('a finish interrupted by a restart is marked failed', async () => {
      const up = await readySession();
      db.prepare("UPDATE fab_uploads SET state = 'finishing' WHERE id = ?").run(up.id);
      expect(recoverInterruptedFinishes(db)).toBe(1);
      const row = getUpload(db, up.id);
      expect(row?.state).toBe('failed');
      expect(JSON.parse(row?.problems ?? '[]').join()).toContain('server restarted');
    });
  });

  describe('removed mods and deleted accounts', () => {
    it('a removed mod frees its name: for its owner, and for anyone once the account is deleted', async () => {
      const first = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'));
      if (!first.r.ok) throw new Error('setup');
      expect(removeOwnMod(db, store, 'acc-a', first.r.modId)).toBe(true);
      const again = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'));
      expect(again.r.ok).toBe(true);
      const blocked = await publish('acc-b', await modZip('[Mod]\nName=horde bank\n'), meta({ name: 'horde bank' }));
      expect(blocked.r.ok).toBe(false);
      forgetFabricatorAccount(db, store, 'acc-a');
      const other = await publish('acc-b', await modZip('[Mod]\nName=horde bank\n'), meta({ name: 'horde bank' }));
      expect(other.r.ok).toBe(true);
    });

    it('a deleted account keeps only the id, name, game and state of its mods', async () => {
      const live = await publish('acc-a', await modZip('[Mod]\nName=Horde Bank\n'), meta({ notes: 'first cut', requires: [] }));
      const gone = await publish('acc-a', await modZip('[Mod]\nName=Old Thing\n'), meta({ name: 'Old Thing' }));
      if (!live.r.ok || !gone.r.ok) throw new Error('setup');
      removeOwnMod(db, store, 'acc-a', gone.r.modId);   // removed earlier by its owner: cleared too
      forgetFabricatorAccount(db, store, 'acc-a');
      for (const [id, name] of [[live.r.modId, 'Horde Bank'], [gone.r.modId, 'Old Thing']] as const) {
        expect(db.prepare('SELECT id, owner, name, game, state, short_desc, long_desc, tags, requires FROM fab_mods WHERE id = ?').get(id)).toEqual({
          id, owner: '', name, game: 'gears3', state: 'removed', short_desc: '', long_desc: '', tags: '[]', requires: '[]' });
        expect(db.prepare("SELECT COUNT(*) AS n FROM fab_versions WHERE mod_id = ? AND notes != ''").get(id)).toEqual({ n: 0 });
        expect(db.prepare('SELECT COUNT(*) AS n FROM fab_screens WHERE mod_id = ?').get(id)).toEqual({ n: 0 });
      }
    });
  });

  describe('putChunk', () => {
    it('accepts chunks in order only; a repeat of the last one is ignored', async () => {
      const up = startUpload(db, 'acc-a', meta() as any, null, 3000, 'ab'.repeat(32), NOW, s);
      const c = (n: number, len = 1024) => Buffer.alloc(len, n + 1);
      expect(await putChunk(db, store, up.id, 1, c(1), sha256(c(1)))).toBe('order');
      expect(await putChunk(db, store, up.id, 0, c(0), sha256(c(1)))).toBe('hash');
      expect(await putChunk(db, store, up.id, 0, c(0), sha256(c(0)))).toBe('ok');
      expect(await putChunk(db, store, up.id, 0, c(0), sha256(c(0)))).toBe('repeat');
      expect(await putChunk(db, store, up.id, 0, c(5), sha256(c(5)))).toBe('order');
      expect(await putChunk(db, store, up.id, 1, c(1, 1000), sha256(c(1, 1000)))).toBe('size');
      expect(await putChunk(db, store, up.id, 3, c(3), sha256(c(3)))).toBe('order');
    });
  });

  describe('expireUploads', () => {
    it('removes sessions past their expiry with their chunks', async () => {
      const up = startUpload(db, 'acc-a', meta() as any, null, 3000, 'ab'.repeat(32), NOW, s);
      const c = Buffer.alloc(1024, 1);
      await putChunk(db, store, up.id, 0, c, sha256(c));
      expect(expireUploads(db, store, NOW + s.sessionTtlMs - 1)).toBe(0);
      expect(expireUploads(db, store, NOW + s.sessionTtlMs + 1)).toBe(1);
      expect(db.prepare('SELECT 1 FROM fab_uploads WHERE id = ?').get(up.id)).toBeUndefined();
      expect(fs.existsSync(path.join(dir, 'fabricator', 'chunks', up.id))).toBe(false);
    });
  });
});
