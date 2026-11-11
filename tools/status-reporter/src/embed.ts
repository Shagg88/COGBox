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

export function buildEmbed(lines: Line[], nowMs: number): Embed {
  const counted = lines.filter((l) => !l.optional);
  const red = counted.some((l) => l.core && l.state === 'down');
  const amber = counted.some((l) => l.state !== 'up');
  const unix = Math.floor(nowMs / 1000);
  return {
    title: 'COGBox server status',
    // Discord renders <t:…:R> as "2 minutes ago" in the reader's own clock, so a frozen embed visibly goes stale
    description: `Last checked <t:${unix}:R>`,
    color: red ? COLOURS.red : amber ? COLOURS.amber : COLOURS.green,
    fields: lines.map((l) => ({ name: clip(`${ICON[l.state]} ${l.name}`, 256), value: clip(l.text || '—', 1024) })),
    footer: { text: 'Last updated' },
    timestamp: new Date(nowMs).toISOString()
  };
}
