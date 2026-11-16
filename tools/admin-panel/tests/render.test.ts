import { describe, expect, it } from 'vitest';
// @ts-expect-error plain browser module, no types
import { auditHtml, esc, fabReportsHtml, fabStatsHtml, lobbiesHtml, logsHtml, overviewHtml } from '../public/render.js';

const NOW = 1_800_000_000_000;
const lobby = (o: object) => ({ lobby_id: 'L1', room_code: 'ABCD', title_id: 't', host_tag: 'h', is_public: true, created_at: NOW, ...o });

describe('render', () => {
  it('render escapes tags and log lines', () => {
    expect(esc('<a href="x">&\'')).toBe('&lt;a href=&quot;x&quot;&gt;&amp;&#39;');
    const l = lobbiesHtml([lobby({ host_tag: '<script>x</script>' })], NOW);
    expect(l).toContain('&lt;script&gt;');
    expect(l).not.toContain('<script>');
    const g = logsHtml(['<img src=x onerror=alert(1)>', 'plain'], '');
    expect(g).toContain('&lt;img');
    expect(g).not.toContain('<img');
    expect(logsHtml(['abc', 'xyz'], 'ab')).not.toContain('xyz');
    const a = auditHtml([{ id: 1, time: NOW, name: '<b>', action: 'login', detail: '<i>' }]);
    expect(a).not.toContain('<b>');
    expect(a).not.toContain('<i>');
  });

  it('lobbies older than 2 h get class old', () => {
    const h = lobbiesHtml([lobby({ room_code: 'NEW1', created_at: NOW - 60_000 }), lobby({ room_code: 'OLD1', created_at: NOW - 2 * 3600_000 - 1000 })], NOW);
    expect(h).toMatch(/class="old"[^]*OLD1/);
    expect(h).not.toMatch(/class="old"[^]*NEW1/);
  });

  it('overview shows Down when status is null', () => {
    const h = overviewHtml({ health: null, status: null, last_action: null });
    expect(h).toContain('Down');
    expect(h).toContain('Unknown');
    expect(h).toContain('data-restart="master-server"');
  });
});

describe('render times', () => {
  it('a missing or invalid time renders as a dash', () => {
    const a = auditHtml([{ id: 1, time: undefined, name: 'a', action: 'login', detail: '' }, { id: 2, time: 'x', name: 'b', action: 'login', detail: '' }]);
    expect(a.match(/—/g)?.length).toBe(2);
  });
});

describe('render fabricator', () => {
  const UUID = '2f27a1b4-0c3d-4e5f-8a9b-1c2d3e4f5a6b';
  const queue = [{
    mod: { id: 7, name: '<img src=x onerror=alert(1)>', game: 'gears3', author: 'Bad"guy#1234', state: 'live' },
    reports: [{ id: 1, reason: 'malware', text: '<script>steal()</script>', reporter: "O'Neil#0001", created_at: NOW }],
  }, {
    mod: { id: 9, name: 'Down', game: 'gears2', author: 'Dom#0002', state: 'taken_down' },
    reports: [{ id: 2, reason: 'stolen', text: '', reporter: 'Cole#0003', created_at: NOW }],
  }];

  it('draws the queue escaped, with action buttons and no uuid', () => {
    const h = fabReportsHtml(queue);
    expect(h).not.toContain('<img');
    expect(h).not.toContain('<script>');
    expect(h).toContain('&lt;script&gt;');
    expect(h).toContain('Bad&quot;guy#1234');
    expect(h).toContain('O&#39;Neil#0001');
    expect(h).toContain('data-fab-action="takedown" data-mod="7"');
    expect(h).toContain('data-fab-action="restore" data-mod="9"');
    expect(h).not.toContain('data-fab-action="takedown" data-mod="9"');
    expect(h).toContain('data-fab-action="delete-files" data-mod="7"');
    expect(h).toContain('data-fab-stop="Bad&quot;guy#1234"');
    expect(h).not.toContain(UUID);
    expect(h).not.toMatch(/[0-9a-f]{8}-[0-9a-f]{4}-/);
    expect(fabReportsHtml([])).toContain('No open reports');
  });

  it('draws the stats', () => {
    const h = fabStatsHtml({ disk: { total: 100 * 1073741824, free: 40 * 1073741824 }, month_bytes: 3 * 1073741824, mods: 12, creators: 4, open_reports: 2 });
    expect(h).toContain('60.0 of 100.0 GB used');
    expect(h).toContain('3.00 GB');
    expect(h).toContain('>12<');
    expect(h).toContain('>4<');
    expect(h).toContain('>2<');
  });
});
