// Hourly up/total counts per core service for the last 7 days, plus the latest outage. Lives in the state file.
export type Svc = 'master' | 'stun';
export const SVC_NAME: Record<Svc, string> = { master: 'Matchmaking server', stun: 'STUN' };

export interface Incident { svc: Svc; start: number; end?: number }
export interface History {
  buckets: Record<string, Partial<Record<Svc, [number, number]>>>; // hour index -> svc -> [up, total]
  open: Partial<Record<Svc, number>>;                               // svc -> outage start (ms)
  last?: Incident;
}

const HOUR = 3600_000;
const KEEP_H = 24 * 7;

export const emptyHistory = (): History => ({ buckets: {}, open: {} });

export function record(h: History, svc: Svc, up: boolean, now: number): void {
  const hour = Math.floor(now / HOUR);
  const b = (h.buckets[hour] ??= {});
  const c = (b[svc] ??= [0, 0]);
  c[0] += up ? 1 : 0;
  c[1] += 1;
  for (const k of Object.keys(h.buckets)) if (Number(k) <= hour - KEEP_H) delete h.buckets[k];

  const start = h.open[svc];
  if (!up && start === undefined) { h.open[svc] = now; h.last = { svc, start: now }; }
  if (up && start !== undefined) { delete h.open[svc]; h.last = { svc, start, end: now }; }
}

export function uptimePct(h: History, svc: Svc, hours: number, now: number): number | null {
  const hour = Math.floor(now / HOUR);
  let up = 0, total = 0;
  for (let i = hour - hours + 1; i <= hour; i++) {
    const c = h.buckets[i]?.[svc];
    if (c) { up += c[0]; total += c[1]; }
  }
  return total ? (100 * up) / total : null;
}

// 12 squares of 2 hours each (24 do not fit beside the thumbnail), oldest on the left:
// ⬛ no data, 🟩 all up, 🟨 some failures, 🟥 down more than half the time
export function bar(h: History, svc: Svc, now: number): string {
  const hour = Math.floor(now / HOUR);
  let s = '';
  for (let i = hour - 23; i <= hour; i += 2) {
    const a = h.buckets[i]?.[svc], b = h.buckets[i + 1]?.[svc];
    const up = (a?.[0] ?? 0) + (b?.[0] ?? 0), total = (a?.[1] ?? 0) + (b?.[1] ?? 0);
    s += !total ? '⬛' : up === total ? '🟩' : up * 2 >= total ? '🟨' : '🟥';
  }
  return s;
}

const dur = (ms: number) => {
  const m = Math.max(1, Math.round(ms / 60000));
  return m < 60 ? `${m}m` : m < 1440 ? `${Math.floor(m / 60)}h ${m % 60}m` : `${Math.floor(m / 1440)}d ${Math.floor((m % 1440) / 60)}h`;
};

export function lastIncidentText(h: History, _now: number): string {
  const open = (Object.entries(h.open) as [Svc, number][]).sort((a, b) => a[1] - b[1])[0];
  if (open) return `🔴 ${SVC_NAME[open[0]]} down since <t:${Math.floor(open[1] / 1000)}:R>`;
  const l = h.last;
  if (!l || l.end === undefined) return 'No outages recorded';
  return `${SVC_NAME[l.svc]}, <t:${Math.floor(l.start / 1000)}:R>, lasted ${dur(l.end - l.start)}`;
}
