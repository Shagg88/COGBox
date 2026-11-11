import { describe, expect, it } from 'vitest';
// @ts-expect-error plain browser module, no types
import { auditHtml, esc, lobbiesHtml, logsHtml, overviewHtml } from '../public/render.js';

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
