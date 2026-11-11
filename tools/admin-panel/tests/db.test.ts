import { describe, it, expect } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { openDb } from '../src/db.js';

describe('db files', () => {
  it.skipIf(process.platform === 'win32')('are owner-only, including a pre-existing file', () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'admindb-'));
    const p = path.join(dir, 'admin.db');
    fs.writeFileSync(p, '', { mode: 0o644 });
    const db = openDb(p);
    db.prepare("INSERT INTO audit (time, name, action) VALUES (1, 'x', 'y')").run();
    for (const f of [p, p + '-wal', p + '-shm']) {
      expect(fs.statSync(f).mode & 0o777).toBe(0o600);
    }
    db.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });
});
