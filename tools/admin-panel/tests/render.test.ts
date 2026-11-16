import { describe, expect, it } from 'vitest';
// @ts-expect-error plain browser module, no types
import { auditHtml, esc, fabModHtml, fabModsHtml, fabReportsHtml, fabStatsHtml, lobbiesHtml, logsHtml, overviewHtml } from '../public/render.js';

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

  it('offers no Stop publishing for an unknown author', () => {
    const h = fabReportsHtml([{ ...queue[1], mod: { ...queue[1].mod, author: 'unknown' } }]);
    expect(h).not.toContain('data-fab-stop');
    expect(h).toContain('data-fab-action="restore"');
  });

  it('offers Allow publishing for a creator whose publishing is stopped', () => {
    const h = fabReportsHtml([{ ...queue[1], mod: { ...queue[1].mod, publishing: false } }]);
    expect(h).toContain('data-fab-allow="Dom#0002"');
    expect(h).not.toContain('data-fab-stop');
    expect(fabReportsHtml([{ ...queue[1], mod: { ...queue[1].mod, publishing: true } }])).toContain('data-fab-stop="Dom#0002"');
  });

  it('draws the mods list escaped, with the actions each state allows', () => {
    const mods = [
      { id: 7, name: '<img src=x onerror=alert(1)>', game: 'gears3', author: 'Bad"guy#1234', state: 'live', versions: 2, open_reports: 1, publishing: true, created_at: NOW },
      { id: 8, name: 'Down', game: 'gears2', author: 'Dom#0002', state: 'taken_down', versions: 1, open_reports: 0, publishing: false, created_at: NOW },
      { id: 9, name: 'Gone', game: 'gears1', author: 'unknown', state: 'removed', versions: 1, open_reports: 0, publishing: true, created_at: NOW },
    ];
    const h = fabModsHtml(mods);
    expect(h).not.toContain('<img');
    expect(h).toContain('&lt;img src=x onerror=alert(1)&gt;');
    expect(h).toContain('Bad&quot;guy#1234');
    expect(h).toContain('data-fab-detail="7"');
    expect(h).toContain('data-fab-action="takedown" data-mod="7"');
    expect(h).toContain('data-fab-action="delete-files" data-mod="7"');
    expect(h).toContain('data-fab-action="restore" data-mod="8"');
    expect(h).not.toContain('data-fab-action="takedown" data-mod="8"');
    expect(h).toContain('data-fab-action="delete-files" data-mod="8"');
    expect(h).not.toContain('data-mod="9"');   // removed: nothing left to act on
    expect(h).toContain('Removed');
    expect(fabModsHtml([])).toContain('No mods');
  });

  it("draws a mod's details escaped: versions, screenshots, reports and the creator's publishing", () => {
    const d = {
      id: 7, name: '<b>Alpha</b>', game: 'gears3', type: 'texture_pack', author: "O'Neil#0001", state: 'taken_down', publishing: false,
      created_at: NOW, updated_at: NOW, screens: 3,
      versions: [
        { id: 2, version: '<1.1>', size: 3 * 1048576, sha256: 'a'.repeat(64), state: 'live', created_at: NOW },
        { id: 1, version: '1.0', size: 1048576, sha256: 'b'.repeat(64), state: 'files_deleted', created_at: NOW },
      ],
      reports: [{ id: 5, reason: 'stolen', text: '<script>x()</script>', reporter: 'Cole#0003', created_at: NOW, handled_by: 'alice', outcome: 'taken_down' }],
    };
    const h = fabModHtml(d);
    expect(h).not.toContain('<b>Alpha');
    expect(h).not.toContain('<script>');
    expect(h).toContain('&lt;b&gt;Alpha&lt;/b&gt;');
    expect(h).toContain('O&#39;Neil#0001');
    expect(h).toContain('&lt;1.1&gt;');
    expect(h).toContain('3.00 MB');
    expect(h).toContain('a'.repeat(64));
    expect(h).toContain('Files deleted');
    expect(h).toContain('3 screenshots');
    expect(h).toContain('&lt;script&gt;x()&lt;/script&gt;');
    expect(h).toContain('alice');
    expect(h).toContain('data-fab-allow="O&#39;Neil#0001"');
    expect(h).toContain('data-fab-action="restore" data-mod="7"');
    expect(fabModHtml({ ...d, author: 'unknown', reports: [] })).not.toContain('data-fab-allow');
  });

  it('draws the stats', () => {
    const h = fabStatsHtml({ disk: { total: 100 * 1073741824, free: 40 * 1073741824 }, month_bytes: 3 * 1073741824, month_bytes_out: 5.5 * 1073741824,
      mods: 12, creators: 4, open_reports: 2 });
    expect(h).toContain('60.0 of 100.0 GB used');
    expect(h).toContain('Uploads (30 days)');
    expect(h).toContain('3.00 GB');
    expect(h).toContain('Downloads (30 days)');
    expect(h).toContain('5.50 GB');
    expect(h).toContain('>12<');
    expect(h).toContain('>4<');
    expect(h).toContain('>2<');
  });
});
