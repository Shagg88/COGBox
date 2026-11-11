import { describe, expect, it } from 'vitest';
import { emptyHistory, record, bar, uptimePct, lastIncidentText } from '../src/history.js';

const H = 3600_000;

describe('history', () => {
  it('buckets samples by hour and computes uptime %', () => {
    const h = emptyHistory();
    record(h, 'master', true, 0);
    record(h, 'master', true, 60_000);
    record(h, 'master', false, 120_000);
    record(h, 'master', true, 180_000);
    expect(uptimePct(h, 'master', 24, 180_000)).toBeCloseTo(75);
  });

  it('draws 12 two-hour squares, oldest first: no data, all up, some down, mostly down', () => {
    const h = emptyHistory();
    const now = 23 * H; // squares cover hours 0-1, 2-3 … 22-23
    record(h, 'stun', true, 16 * H); record(h, 'stun', true, 17 * H);
    record(h, 'stun', true, 18 * H); record(h, 'stun', false, 19 * H);
    record(h, 'stun', true, 20 * H); record(h, 'stun', false, 20 * H + 1); record(h, 'stun', false, 21 * H);
    record(h, 'stun', true, 23 * H);
    const b = [...bar(h, 'stun', now)];
    expect(b.length).toBe(12);
    expect(b.slice(0, 8).every((c) => c === '⬛')).toBe(true);
    expect(b.slice(8)).toEqual(['🟩', '🟨', '🟥', '🟩']);
  });

  it('drops buckets older than 7 days', () => {
    const h = emptyHistory();
    record(h, 'master', false, 0);
    record(h, 'master', true, 8 * 24 * H);
    expect(uptimePct(h, 'master', 24 * 7, 8 * 24 * H)).toBe(100);
    expect(Object.keys(h.buckets).length).toBe(1);
  });

  it('returns null uptime with no data', () => {
    expect(uptimePct(emptyHistory(), 'master', 24, 0)).toBeNull();
  });

  it('records an outage from first failure to recovery', () => {
    const h = emptyHistory();
    record(h, 'master', true, 0);
    record(h, 'master', false, 60_000);
    expect(lastIncidentText(h, 90_000)).toBe('🔴 Matchmaking server down since <t:60:R>');
    record(h, 'master', false, 120_000);
    record(h, 'master', true, 240_000);
    expect(lastIncidentText(h, 300_000)).toBe('Matchmaking server, <t:60:R>, lasted 3m');
  });

  it('says so when nothing has gone down', () => {
    const h = emptyHistory();
    record(h, 'stun', true, 0);
    expect(lastIncidentText(h, 0)).toBe('No outages recorded');
  });
});
