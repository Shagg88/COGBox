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

  // GitHub allows 60 unauthenticated requests an hour per address and one check is 4: every 15 minutes is 16 an hour
  const githubFetch = (state: { calls: number; fail: boolean }) => (async (url: string | URL) => {
    const u = String(url);
    if (u.includes('api.github.com')) {
      state.calls++;
      if (state.fail) return new Response('{"message":"API rate limit exceeded"}', { status: 403 });
      if (u.endsWith('/repos/o/r')) return Response.json({ default_branch: 'main', open_issues_count: 3 });
      if (u.includes('/releases/latest')) return Response.json({ tag_name: 'v1', html_url: 'https://github.com/o/r/releases/v1' });
      if (u.includes('/actions/runs')) return Response.json({ workflow_runs: [] });
      if (u.includes('/commits')) return Response.json([{ sha: 'abcdef1234', commit: { message: 'hello' } }]);
    }
    return new Response('{}', { status: 503 });
  }) as typeof fetch;
  const ghCfg = () => ({ ...loadConfig({ GITHUB_ENABLED: 'true', GITHUB_REPO: 'o/r' }), stunPort: 9, stunHost: '127.0.0.1' });
  const gh = (lines: { name: string; text: string }[]) => lines.find((l) => l.name === 'GitHub')!;

  it('asks GitHub at most once per 15 minutes', async () => {
    const s = { calls: 0, fail: false };
    const cache = new Map();
    await collect(ghCfg(), githubFetch(s), cache, 0);
    const once = s.calls;
    await collect(ghCfg(), githubFetch(s), cache, 10 * 60_000);
    expect(s.calls).toBe(once);
    await collect(ghCfg(), githubFetch(s), cache, 15 * 60_000);
    expect(s.calls).toBe(once * 2);
  }, 10_000);

  it('keeps the last good GitHub answer when GitHub refuses a check', async () => {
    const s = { calls: 0, fail: false };
    const cache = new Map();
    const good = gh(await collect(ghCfg(), githubFetch(s), cache, 0));
    expect(good.text).toContain('v1');
    s.fail = true;
    const later = gh(await collect(ghCfg(), githubFetch(s), cache, 15 * 60_000));
    expect(later.text).toBe(good.text);
  }, 10_000);

  it('says unavailable when GitHub never answered', async () => {
    const s = { calls: 0, fail: true };
    expect(gh(await collect(ghCfg(), githubFetch(s), new Map(), 0)).text).toBe('unavailable');
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
