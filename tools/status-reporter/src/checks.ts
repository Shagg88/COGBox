import { fmtUptime } from './embed.js';
import type { Line } from './types.js';

const TIMEOUT_MS = 8000;

class HttpError extends Error {
  constructor(public status: number, public retryAfterS?: number) { super(`HTTP ${status}`); }
}

async function getJson<T>(f: typeof fetch, url: string, headers: Record<string, string> = {}): Promise<T> {
  const res = await f(url, { headers: { 'user-agent': 'cogbox-status-reporter', ...headers }, signal: AbortSignal.timeout(TIMEOUT_MS) });
  if (!res.ok) {
    const ra = Number(res.headers.get('retry-after'));
    throw new HttpError(res.status, Number.isFinite(ra) && ra > 0 ? Math.ceil(ra) : undefined);
  }
  return (await res.json()) as T;
}

function failLine(name: string, err: unknown, extra: Partial<Line> = {}): Line {
  const e = err as HttpError;
  if (e instanceof HttpError && (e.status === 429 || e.retryAfterS)) {
    return { name, state: 'unknown', text: 'rate limited, will retry', retryAfterS: e.retryAfterS ?? 300, ...extra };
  }
  return { name, state: 'unknown', text: e instanceof HttpError ? `API error (${e.status})` : 'not reachable', ...extra };
}

// One-line Markdown link; strips characters that would break Discord's [text](url) form
const link = (text: string, url?: string) => {
  const t = text.replace(/[[\]]/g, '');
  return url && /^https:\/\//.test(url) ? `[${t}](${url.replace(/[()\s]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0'))})` : t;
};
const firstLine = (s: string) => s.split('\n')[0].trim().slice(0, 100);

// Uptime only: the brief rules out player and lobby counts, names and addresses. The public health endpoint is enough,
// so the reporter needs no admin secret (0.6.0 review).
export async function checkMaster(f: typeof fetch, baseUrl: string): Promise<Line> {
  const name = 'Matchmaking server';
  try {
    const t0 = performance.now();
    const s = await getJson<{ status?: string; uptime_s?: number }>(f, baseUrl.replace(/\/+$/, '') + '/api/v1/health');
    const ms = Math.round(performance.now() - t0);
    const up = typeof s.uptime_s === 'number' && Number.isFinite(s.uptime_s) ? `up ${fmtUptime(s.uptime_s)}` : 'up';
    return { name, state: 'up', text: up, core: true, ms };
  } catch {
    return { name, state: 'down', text: 'not answering', core: true };
  }
}

interface RepoSummary { release?: string; ci?: string; ciOk: boolean; commit?: string; issues?: number }

function repoText(r: RepoSummary): string {
  return [
    `Release: ${r.release ?? 'none'}`,
    `Pipeline: ${r.ci ?? 'none'}`,
    `Newest: ${r.commit ?? 'none'}`,
    `${r.issues ?? 0} open issues`
  ].join('\n');
}

export async function checkGitlab(f: typeof fetch, project: string, name = 'GitLab'): Promise<Line> {
  const base = `https://gitlab.com/api/v4/projects/${encodeURIComponent(project)}`;
  try {
    const p = await getJson<{ default_branch: string; open_issues_count?: number; web_url: string }>(f, base);
    const ref = encodeURIComponent(p.default_branch);
    const [rel, pipes, commits] = await Promise.all([
      getJson<{ tag_name: string; _links?: { self?: string } }[]>(f, `${base}/releases?per_page=1`),
      getJson<{ status: string; web_url: string }[]>(f, `${base}/pipelines?ref=${ref}&per_page=1`),
      getJson<{ short_id: string; title: string; web_url: string }[]>(f, `${base}/repository/commits?ref_name=${ref}&per_page=1`)
    ]);
    const pipe = pipes[0];
    const c = commits[0];
    const summary: RepoSummary = {
      release: rel[0] ? link(rel[0].tag_name, rel[0]._links?.self) : undefined,
      ci: pipe ? link(pipe.status, pipe.web_url) : undefined,
      ciOk: !pipe || !['failed', 'canceled'].includes(pipe.status),
      commit: c ? `\`${c.short_id}\` ${firstLine(c.title)}` : undefined,
      issues: p.open_issues_count
    };
    return { name, state: summary.ciOk ? 'up' : 'warn', text: repoText(summary) };
  } catch (err) {
    return failLine(name, err);
  }
}

// GitHub is optional: anything short of a full answer shows "unavailable" and never changes the colour
export async function checkGithub(f: typeof fetch, repo: string, name = 'GitHub'): Promise<Line> {
  const base = `https://api.github.com/repos/${repo}`;
  const h = { accept: 'application/vnd.github+json' };
  try {
    const p = await getJson<{ default_branch: string; open_issues_count: number }>(f, base, h);
    const ref = encodeURIComponent(p.default_branch);
    const [rel, runs, commits] = await Promise.all([
      getJson<{ tag_name: string; html_url: string }>(f, `${base}/releases/latest`, h).catch(() => undefined),
      getJson<{ workflow_runs: { status: string; conclusion: string | null; html_url: string }[] }>(f, `${base}/actions/runs?branch=${ref}&per_page=1`, h),
      getJson<{ sha: string; commit: { message: string } }[]>(f, `${base}/commits?sha=${ref}&per_page=1`, h)
    ]);
    const run = runs.workflow_runs[0];
    const c = commits[0];
    const ci = run ? run.conclusion ?? run.status : undefined;
    const summary: RepoSummary = {
      release: rel ? link(rel.tag_name, rel.html_url) : undefined,
      ci: run && ci ? link(ci, run.html_url) : undefined,
      ciOk: !ci || !['failure', 'cancelled', 'timed_out'].includes(ci),
      commit: c ? `\`${c.sha.slice(0, 7)}\` ${firstLine(c.commit.message)}` : undefined,
      issues: p.open_issues_count
    };
    return { name, state: summary.ciOk ? 'up' : 'warn', text: repoText(summary), optional: true };
  } catch (err) {
    const l = failLine(name, err, { optional: true });
    return { ...l, state: 'unavailable', text: 'unavailable' };
  }
}

export async function checkSourceforge(f: typeof fetch, project: string, name = 'SourceForge'): Promise<Line> {
  try {
    const r = await getJson<{ release?: { filename: string; url: string; date?: string } }>(
      f, `https://sourceforge.net/projects/${encodeURIComponent(project)}/best_release.json`);
    if (!r.release) return { name, state: 'up', text: 'no release yet' };
    const file = r.release.filename.split('/').pop() || r.release.filename;
    const date = r.release.date ? ` (${r.release.date.slice(0, 10)})` : '';
    return { name, state: 'up', text: `Latest: ${link(file, r.release.url)}${date}` };
  } catch (err) {
    return failLine(name, err);
  }
}
