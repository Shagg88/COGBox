import { describe, expect, it } from 'vitest';
import { checkGithub, checkGitlab, checkMaster, checkSourceforge } from '../src/checks.js';

type Routes = Record<string, { status?: number; body?: unknown; headers?: Record<string, string> } | Error>;
// a fake fetch that answers by URL prefix and records the requests
function fakeFetch(routes: Routes) {
  const seen: { url: string; headers: Record<string, string> }[] = [];
  const f = async (url: string | URL, init?: RequestInit) => {
    const u = String(url);
    seen.push({ url: u, headers: (init?.headers ?? {}) as Record<string, string> });
    const key = Object.keys(routes).find((k) => u.startsWith(k));
    const r = key ? routes[key] : { status: 404, body: {} };
    if (r instanceof Error) throw r;
    return new Response(JSON.stringify(r.body ?? {}), { status: r.status ?? 200, headers: r.headers });
  };
  return { f: f as typeof fetch, seen };
}

describe('checkMaster', () => {
  it('shows uptime only, from the public health endpoint, with no secret', async () => {
    const { f, seen } = fakeFetch({ 'http://m/api/v1/health': { body: { status: 'ok', timestamp: 1, uptime_s: 90000 } } });
    const l = await checkMaster(f, 'http://m');
    expect(l.state).toBe('up');
    expect(l.text).toBe('up 1d 1h');
    expect(seen[0].headers['x-admin-secret']).toBeUndefined();
  });
  it('says just "up" when the uptime is missing (no NaN)', async () => {
    const { f } = fakeFetch({ 'http://m/api/v1/health': { body: { status: 'ok' } } });
    const l = await checkMaster(f, 'http://m');
    expect(l).toMatchObject({ state: 'up', text: 'up' });
  });
  it('is down when it does not answer', async () => {
    const { f } = fakeFetch({ 'http://m/api/v1/health': new Error('ECONNREFUSED') });
    const l = await checkMaster(f, 'http://m');
    expect(l).toMatchObject({ state: 'down', core: true });
  });
});

const GL = 'https://gitlab.com/api/v4/projects/shagg88%2Fcogbox';
describe('checkGitlab', () => {
  it('shows release, pipeline, newest commit and open issues', async () => {
    const { f } = fakeFetch({
      [`${GL}/releases`]: { body: [{ tag_name: 'v0.5.0', name: 'COGBox 0.5.0', _links: { self: 'https://gitlab.com/r/1' } }] },
      [`${GL}/pipelines`]: { body: [{ status: 'success', web_url: 'https://gitlab.com/p/9' }] },
      [`${GL}/repository/commits`]: { body: [{ short_id: 'abc1234', title: 'Fix join', web_url: 'https://gitlab.com/c/abc' }] },
      [GL]: { body: { default_branch: 'main', open_issues_count: 4, web_url: 'https://gitlab.com/shagg88/cogbox' } }
    });
    const l = await checkGitlab(f, 'shagg88/cogbox');
    expect(l.state).toBe('up');
    expect(l.text).toContain('[v0.5.0](https://gitlab.com/r/1)');
    expect(l.text).toContain('success');
    expect(l.text).toContain('`abc1234` Fix join');
    expect(l.text).toContain('4 open issues');
  });
  it('is amber-worthy (warn) when the pipeline failed', async () => {
    const { f } = fakeFetch({
      [`${GL}/releases`]: { body: [] },
      [`${GL}/pipelines`]: { body: [{ status: 'failed', web_url: 'u' }] },
      [`${GL}/repository/commits`]: { body: [] },
      [GL]: { body: { default_branch: 'main', open_issues_count: 0, web_url: 'w' } }
    });
    expect((await checkGitlab(f, 'shagg88/cogbox')).state).toBe('warn');
  });
  it('is unknown when the API is down', async () => {
    const { f } = fakeFetch({ [GL]: { status: 503 } });
    expect((await checkGitlab(f, 'shagg88/cogbox')).state).toBe('unknown');
  });
  it('reports rate limiting with the wait time', async () => {
    const { f } = fakeFetch({ [GL]: { status: 429, headers: { 'retry-after': '120' } } });
    const l = await checkGitlab(f, 'shagg88/cogbox');
    expect(l.state).toBe('unknown');
    expect(l.retryAfterS).toBe(120);
  });
});

describe('checkGithub', () => {
  it('is optional and "unavailable" when the repo cannot be reached', async () => {
    const { f } = fakeFetch({ 'https://api.github.com/repos/Shagg88/COGBox': { status: 403 } });
    const l = await checkGithub(f, 'Shagg88/COGBox');
    expect(l).toMatchObject({ state: 'unavailable', optional: true, text: 'unavailable' });
  });
  it('shows the same details as GitLab when reachable', async () => {
    const R = 'https://api.github.com/repos/Shagg88/COGBox';
    const { f } = fakeFetch({
      [`${R}/releases/latest`]: { body: { tag_name: 'v0.5.0', html_url: 'https://github.com/r' } },
      [`${R}/actions/runs`]: { body: { workflow_runs: [{ status: 'completed', conclusion: 'success', html_url: 'h' }] } },
      [`${R}/commits`]: { body: [{ sha: 'abcdef1234567', commit: { message: 'Fix join\n\nbody' }, html_url: 'c' }] },
      [R]: { body: { default_branch: 'main', open_issues_count: 2, html_url: 'https://github.com/Shagg88/COGBox' } }
    });
    const l = await checkGithub(f, 'Shagg88/COGBox');
    expect(l.state).toBe('up');
    expect(l.text).toContain('[v0.5.0](https://github.com/r)');
    expect(l.text).toContain('`abcdef1` Fix join');
    expect(l.text).not.toContain('body');
  });
});

describe('checkSourceforge', () => {
  it('shows the latest file with a link', async () => {
    const { f } = fakeFetch({
      'https://sourceforge.net/projects/cogbox/best_release.json': {
        body: { release: { filename: '/0.5.0/COGBox-0.5.0.zip', url: 'https://sourceforge.net/projects/cogbox/files/0.5.0/COGBox-0.5.0.zip/download', date: '2026-11-10 12:00:00' } }
      }
    });
    const l = await checkSourceforge(f, 'cogbox');
    expect(l.state).toBe('up');
    expect(l.text).toContain('[COGBox-0.5.0.zip](https://sourceforge.net/projects/cogbox/files/0.5.0/COGBox-0.5.0.zip/download)');
    expect(l.text).toContain('2026-11-10');
  });
  it('is unknown when SourceForge fails', async () => {
    const { f } = fakeFetch({ 'https://sourceforge.net/': new Error('timeout') });
    expect((await checkSourceforge(f, 'cogbox')).state).toBe('unknown');
  });
});

import { shortTitle } from '../src/checks.js';
describe('shortTitle', () => {
  it('keeps a short title and cuts a long one at 40 characters with an ellipsis', () => {
    expect(shortTitle('Fix the thing\nmore text')).toBe('Fix the thing');
    const long = "status-reporter: MASTER_URL is the master's game port 18443 on the Oracle server";
    expect(shortTitle(long)).toBe(long.slice(0, 39).trimEnd() + '…');
    expect(shortTitle(long).length).toBeLessThanOrEqual(40);
  });
});
