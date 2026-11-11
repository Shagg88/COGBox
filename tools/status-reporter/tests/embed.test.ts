import { describe, expect, it } from 'vitest';
import { buildEmbed, COLOURS, fmtUptime } from '../src/embed.js';
import type { Line } from '../src/types.js';

const up = (name: string, text = 'ok', core = false): Line => ({ name, state: 'up', text, core });

describe('buildEmbed', () => {
  it('is green when every line is up and ends with a relative timestamp', () => {
    const e = buildEmbed([up('Matchmaking server', 'up', true), up('STUN', 'answers', true)], 1_700_000_000_000);
    expect(e.color).toBe(COLOURS.green);
    expect(e.footer.text).toBe('Last updated');
    expect(e.description).toContain('<t:1700000000:R>');
    expect(e.timestamp).toBe(new Date(1_700_000_000_000).toISOString());
  });

  it('is red when a core service is down', () => {
    const e = buildEmbed([{ name: 'STUN', state: 'down', text: 'no answer', core: true }, up('GitLab')], 0);
    expect(e.color).toBe(COLOURS.red);
  });

  it('is amber when only a non-core source fails', () => {
    const e = buildEmbed([up('STUN', 'ok', true), { name: 'GitLab', state: 'unknown', text: 'API error' }], 0);
    expect(e.color).toBe(COLOURS.amber);
  });

  it('never changes colour for an optional source that is unavailable', () => {
    const e = buildEmbed([up('STUN', 'ok', true), { name: 'GitHub', state: 'unavailable', text: 'unavailable', optional: true }], 0);
    expect(e.color).toBe(COLOURS.green);
  });

  it('puts one field per service with its state icon', () => {
    const e = buildEmbed([up('Matchmaking server', 'up 2d 3h', true), { name: 'STUN', state: 'down', text: 'no answer', core: true }], 0);
    expect(e.fields.map((f) => f.name)).toEqual(['🟢 Matchmaking server', '🔴 STUN']);
    expect(e.fields[0].value).toBe('up 2d 3h');
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
