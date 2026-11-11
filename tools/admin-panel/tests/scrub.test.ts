import { describe, it, expect } from 'vitest';
import { logSafeUrl, logSafeRequest, logSafeText } from '../src/scrub.js';

// The server log never holds a sign-in token or a player's address (PRIVACY.md).
describe('log scrubbing', () => {
  it('hides the token in a query string', () => {
    expect(logSafeUrl('/ws/signal?uuid=2f27&token=abc123&role=game'))
      .toBe('/ws/signal?uuid=2f27&token=REDACTED&role=game');
    expect(logSafeUrl('/ws/signal?token=abc123')).toBe('/ws/signal?token=REDACTED');
  });
  it('leaves other urls alone', () => {
    expect(logSafeUrl('/api/v1/friends')).toBe('/api/v1/friends');
    expect(logSafeUrl('/api/v1/lobbies?title_id=4D5308AB')).toBe('/api/v1/lobbies?title_id=4D5308AB');
  });
  it('logs only the method and the scrubbed url, not the address', () => {
    const r = logSafeRequest({ method: 'GET', url: '/ws/signal?token=x', ip: '1.2.3.4', headers: { authorization: 'COGBox a:b' } });
    expect(r).toEqual({ method: 'GET', url: '/ws/signal?token=REDACTED' });
  });

  it('hides uuids, xuids, addresses and tokens in a message', () => {
    const msg = 'user 2f27a1b4-0c3d-4e5f-8a9b-1c2d3e4f5a6b xuid 0009000012345678 from 203.0.113.7:5432 and 2001:db8::1 / fe80:0:0:0:1:2:3:4 /ws?token=abc';
    const out = logSafeText(msg);
    expect(out).not.toMatch(/2f27a1b4|0009000012345678|203\.0\.113|2001:db8|fe80|abc/);
    expect(out).toContain('user');
  });
  it('leaves an ordinary message alone', () => {
    expect(logSafeText('SQLITE_BUSY: database is locked at 12:30:45')).toBe('SQLITE_BUSY: database is locked at 12:30:45');
    expect(logSafeText('title 4D5308AB')).toBe('title 4D5308AB');
  });
});
