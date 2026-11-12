import { bar, lastIncidentText, uptimePct, type History, type Svc } from './history.js';
import type { Embed, Line, State } from './types.js';

export const COLOURS = { green: 0x2ecc71, amber: 0xf1c40f, red: 0xe74c3c } as const;

const ICON: Record<State, string> = { up: '🟢', warn: '🟡', down: '🔴', unknown: '⚪', unavailable: '⚫' };

export function fmtUptime(s: number): string {
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  return `${m}m`;
}

const clip = (s: string, n: number) => (s.length <= n ? s : s.slice(0, n - 1) + '…');
const pct = (p: number | null) => (p === null ? '—' : `${p >= 99.95 ? 100 : p.toFixed(1)}%`);

export interface Extras { history?: History; logoUrl?: string }

export function buildEmbed(lines: Line[], nowMs: number, x: Extras = {}): Embed {
  const counted = lines.filter((l) => !l.optional);
  const red = counted.some((l) => l.core && l.state === 'down');
  const amber = counted.some((l) => l.state !== 'up');
  const unix = Math.floor(nowMs / 1000);
  const headline = red ? '🔴 **Outage**' : amber ? '⚠️ **Degraded**' : '✅ **All systems operational**';

  const field = (l: Line) => ({
    name: clip(`${ICON[l.state]} ${l.name}`, 256),
    value: clip((l.text || '—') + (l.ms !== undefined && l.state === 'up' ? `\n\`${l.ms} ms\`` : ''), 1024),
    inline: true
  });
  const coreLines = lines.filter((l) => l.core);
  const rest = lines.filter((l) => !l.core);
  const fields: Embed['fields'] = coreLines.map(field);

  if (x.history) {
    const h = x.history;
    const row = (svc: Svc, label: string) =>
      `**${label}** · 24h ${pct(uptimePct(h, svc, 24, nowMs))} · 7d ${pct(uptimePct(h, svc, 168, nowMs))}\n${bar(h, svc, nowMs)}`;
    fields.push({ name: '📈 Last 24 hours', value: clip(`${row('master', 'Matchmaking')}\n\n${row('stun', 'STUN')}`, 1024) });
    fields.push({ name: '🛠 Last incident', value: clip(lastIncidentText(h, nowMs), 1024) });
  }
  fields.push(...rest.map(field));
  // an empty line (zero-width space) after each section; Discord packs fields tightly otherwise
  for (const f of fields.slice(0, -1)) f.value = clip(f.value, 1022) + '\n​';

  return {
    title: 'COGBox server status',
    // Discord renders <t:…:R> as "2 minutes ago" in the reader's own clock, so a frozen embed visibly goes stale
    description: `${headline}\nLast checked <t:${unix}:R>`,
    color: red ? COLOURS.red : amber ? COLOURS.amber : COLOURS.green,
    ...(x.logoUrl ? { thumbnail: { url: x.logoUrl } } : {}),
    fields,
    footer: { text: 'Last updated' },
    timestamp: new Date(nowMs).toISOString()
  };
}
