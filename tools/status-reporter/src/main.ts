import fs from 'node:fs';
import path from 'node:path';
import { checkGithub, checkGitlab, checkMaster, checkSourceforge } from './checks.js';
import { buildEmbed } from './embed.js';
import { emptyHistory, record, type History } from './history.js';
import { checkStun } from './stun.js';
import type { Line } from './types.js';
import { publish } from './webhook.js';

export interface Config {
  webhookUrl?: string;
  masterUrl: string;
  stunHost: string;
  stunPort: number;
  gitlabProject: string;
  sourceforgeProject?: string;
  githubEnabled: boolean;
  githubRepo: string;
  statePath: string;
  intervalS: number;
  logoUrl?: string;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  return {
    webhookUrl: env.DISCORD_WEBHOOK_URL || undefined,
    masterUrl: env.MASTER_URL || 'http://127.0.0.1:8080',
    stunHost: env.STUN_HOST || '127.0.0.1',
    stunPort: Number(env.STUN_PORT) || 3478,
    gitlabProject: env.GITLAB_PROJECT || 'shagg88/cogbox',
    sourceforgeProject: env.SOURCEFORGE_PROJECT || undefined,
    githubEnabled: /^(1|true|yes|on)$/i.test(env.GITHUB_ENABLED ?? ''),
    githubRepo: env.GITHUB_REPO || 'Shagg88/COGBox',
    statePath: env.STATE_PATH || '/data/state.json',
    intervalS: Math.max(30, Number(env.INTERVAL_S) || 60),
    logoUrl: env.LOGO_URL || 'https://gitlab.com/shagg88/cogbox/-/raw/main/Gears2-Native/cogbox/art/cogbox-256.png'
  };
}

// Public sources are checked less often than the server itself (GitLab allows unauthenticated reads, but not unlimited)
const PUBLIC_EVERY_S = 300;

export async function collect(cfg: Config, f: typeof fetch, cache: Map<string, { at: number; line: Line }>, nowMs: number): Promise<Line[]> {
  const cached = async (key: string, run: () => Promise<Line>): Promise<Line> => {
    const c = cache.get(key);
    if (c && nowMs < c.at) return c.line;
    const line = await run();
    cache.set(key, { at: nowMs + 1000 * Math.max(PUBLIC_EVERY_S, line.retryAfterS ?? 0), line });
    return line;
  };
  const jobs: Promise<Line>[] = [
    checkMaster(f, cfg.masterUrl),
    checkStun(cfg.stunHost, cfg.stunPort),
    cached('gitlab', () => checkGitlab(f, cfg.gitlabProject))
  ];
  if (cfg.sourceforgeProject) jobs.push(cached('sf', () => checkSourceforge(f, cfg.sourceforgeProject!)));
  if (cfg.githubEnabled) jobs.push(cached('gh', () => checkGithub(f, cfg.githubRepo)));
  return Promise.all(jobs);
}

interface State { messageId: string | null; history: History }
export function readState(p: string): State {
  try {
    const j = JSON.parse(fs.readFileSync(p, 'utf8')) as Partial<State>;
    return { messageId: j.messageId || null, history: j.history?.buckets ? { ...emptyHistory(), ...j.history } : emptyHistory() };
  } catch { return { messageId: null, history: emptyHistory() }; }
}
const writeState = (p: string, st: State) => {
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p + '.tmp', JSON.stringify(st));
  fs.renameSync(p + '.tmp', p);
};

// feeds the core results into the history; returns the lines unchanged
export function track(h: History, lines: Line[], now: number): void {
  for (const l of lines) {
    if (l.name === 'Matchmaking server') record(h, 'master', l.state === 'up', now);
    else if (l.name.startsWith('STUN')) record(h, 'stun', l.state === 'up', now);
  }
}

async function main() {
  const dry = process.argv.includes('--dry-run');
  const once = process.argv.includes('--once');
  const cfg = loadConfig();
  if (!dry && !cfg.webhookUrl) {
    // idle instead of exiting, so a missing status.env never puts the container in a restart loop
    console.error('status-reporter: DISCORD_WEBHOOK_URL is not set (status.env); idle until it is added and the service restarted');
    await new Promise(() => setInterval(() => {}, 1 << 30)); // a pending promise alone lets Node exit
  }
  const cache = new Map<string, { at: number; line: Line }>();
  const st = readState(cfg.statePath);
  if (dry) st.messageId = null;
  for (;;) {
    let waitS = cfg.intervalS;
    try {
      const now = Date.now();
      const lines = await collect(cfg, fetch, cache, now);
      if (!dry) track(st.history, lines, now);
      const embed = buildEmbed(lines, now, { history: st.history, logoUrl: cfg.logoUrl });
      if (dry) {
        console.log(JSON.stringify({ embeds: [embed] }, null, 2));
      } else {
        const r = await publish(fetch, cfg.webhookUrl!, st.messageId, embed);
        if (r.messageId) st.messageId = r.messageId;
        writeState(cfg.statePath, st);
        if (r.retryAfterS) { waitS = Math.max(waitS, r.retryAfterS); console.error(`status-reporter: Discord rate limit, waiting ${waitS}s`); }
      }
    } catch (err) {
      console.error(`status-reporter: ${(err as Error).message}`); // messages never contain the webhook URL
    }
    if (once) return;
    await new Promise((r) => setTimeout(r, waitS * 1000));
  }
}

if (process.argv[1]?.endsWith('main.js')) main();
