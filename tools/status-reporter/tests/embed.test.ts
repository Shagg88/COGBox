import { describe, expect, it } from 'vitest';
import { buildEmbed, COLOURS, fmtUptime } from '../src/embed.js';
import { emptyHistory, record } from '../src/history.js';
import type { Line } from '../src/types.js';

const up = (name: string, text = 'ok', core = false, ms?: number): Line => ({ name, state: 'up', text, core, ms });
const core = [up('Matchmaking server', 'up 2d 3h', true, 12), up('STUN (UDP 3478)', 'answering', true, 4)];

describe('buildEmbed', () => {
  it('is green with an "all systems operational" headline and a relative timestamp', () => {
    const e = buildEmbed(core, 1_700_000_000_000);
    expect(e.color).toBe(COLOURS.green);
    expect(e.description).toContain('All systems operational');
    expect(e.description).toContain('<t:1700000000:R>');
    expect(e.footer.text).toBe('Last updated');
    expect(e.timestamp).toBe(new Date(1_700_000_000_000).toISOString());
  });

  it('is red with an outage headline when a core service is down', () => {
    const e = buildEmbed([{ name: 'STUN', state: 'down', text: 'no answer', core: true }, up('GitLab')], 0);
    expect(e.color).toBe(COLOURS.red);
    expect(e.description).toContain('Outage');
  });

  it('is amber when only a non-core source fails', () => {
    const e = buildEmbed([...core, { name: 'GitLab', state: 'unknown', text: 'API error' }], 0);
    expect(e.color).toBe(COLOURS.amber);
    expect(e.description).toContain('Degraded');
  });

  it('never changes colour for an optional source that is unavailable', () => {
    const e = buildEmbed([...core, { name: 'GitHub', state: 'unavailable', text: 'unavailable', optional: true }], 0);
    expect(e.color).toBe(COLOURS.green);
  });

  it('puts core services side by side with their response time', () => {
    const e = buildEmbed(core, 0);
    expect(e.fields[0]).toMatchObject({ name: '🟢 Matchmaking server', value: 'up 2d 3h\n`12 ms`', inline: true });
    expect(e.fields[1]).toMatchObject({ name: '🟢 STUN (UDP 3478)', inline: true });
  });

  it('shows the 24h bars, uptime % and last incident when history is given', () => {
    const h = emptyHistory();
    const now = 10 * 3600_000;
    record(h, 'master', true, now); record(h, 'stun', true, now);
    const e = buildEmbed(core, now, { history: h });
    const f = e.fields.find((x) => x.name.startsWith('📈'))!;
    expect(f.value).toContain('Matchmaking');
    expect(f.value).toContain('🟩');
    expect(f.value).toContain('100%');
    expect(f.value).toContain('🟩\n\n**STUN**');
    expect(e.fields.find((x) => x.name.startsWith('🛠'))!.value).toBe('No outages recorded');
  });

  it('shows the logo as a thumbnail when set', () => {
    expect(buildEmbed(core, 0, { logoUrl: 'https://x/logo.png' }).thumbnail).toEqual({ url: 'https://x/logo.png' });
  });

  it('leaves an empty line after every section except the last', () => {
    const e = buildEmbed([...core, up('GitLab', 'g'), up('SourceForge', 's')], 0, { history: emptyHistory() });
    const vals = e.fields.map((f) => f.value);
    expect(vals.slice(0, -1).every((v) => v.endsWith('\n​'))).toBe(true);
    expect(vals.at(-1)).toBe('s');
  });

  it('keeps field values inside Discord limits', () => {
    const e = buildEmbed([up('GitLab', 'x'.repeat(3000))], 0);
    expect(e.fields[0].value.length).toBeLessThanOrEqual(1024);
  });
});

describe('fmtUptime', () => {
  it('formats days, hours, minutes', () => {
    expect(fmtUptime(59)).toBe('0m');
    expect(fmtUptime(3 * 3600 + 5 * 60)).toBe('3h 5m');
    expect(fmtUptime(2 * 86400 + 3600)).toBe('2d 1h');
  });
});
