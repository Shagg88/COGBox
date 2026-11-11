import { describe, expect, it } from 'vitest';
import { collect, loadConfig } from '../src/main.js';

describe('loadConfig', () => {
  it('keeps GitHub off and SourceForge unset by default', () => {
    const c = loadConfig({});
    expect(c.githubEnabled).toBe(false);
    expect(c.sourceforgeProject).toBeUndefined();
    expect(c.intervalS).toBe(60);
  });
  it('turns GitHub on from the switch', () => {
    expect(loadConfig({ GITHUB_ENABLED: 'true' }).githubEnabled).toBe(true);
  });
});

describe('collect', () => {
  it('asks GitLab at most once per 5 minutes and skips disabled sources', async () => {
    let gitlab = 0;
    const f = (async (url: string | URL) => {
      const u = String(url);
      if (u.includes('gitlab.com')) gitlab++;
      if (u.includes('github') || u.includes('sourceforge')) throw new Error('should not be called');
      return new Response('{}', { status: 503 });
    }) as typeof fetch;
    const cfg = { ...loadConfig({}), stunPort: 9, stunHost: '127.0.0.1' };
    const cache = new Map();
    const a = await collect(cfg, f, cache, 0);
    await collect(cfg, f, cache, 60_000);
    expect(gitlab).toBe(1);
    expect(a.map((l) => l.name)).toEqual(['Matchmaking server', 'STUN (UDP 9)', 'GitLab']);
  }, 10_000);
});

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { readState, track } from '../src/main.js';
import { emptyHistory, uptimePct } from '../src/history.js';

describe('state', () => {
  it('reads the old {messageId} file and keeps the message', () => {
    const p = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sr-')), 'state.json');
    fs.writeFileSync(p, JSON.stringify({ messageId: '42' }));
    const st = readState(p);
    expect(st.messageId).toBe('42');
    expect(st.history.buckets).toEqual({});
  });
  it('tracks master and STUN results only', () => {
    const h = emptyHistory();
    track(h, [{ name: 'Matchmaking server', state: 'down', text: '' }, { name: 'STUN (UDP 3478)', state: 'up', text: '' }, { name: 'GitLab', state: 'down', text: '' }], 0);
    expect(uptimePct(h, 'master', 24, 0)).toBe(0);
    expect(uptimePct(h, 'stun', 24, 0)).toBe(100);
  });
});
