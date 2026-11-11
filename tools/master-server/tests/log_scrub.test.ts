import { describe, it, expect } from 'vitest';
import { logSafeUrl, logSafeRequest } from '../src/log_scrub.js';

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
    // a real request carries more than the serializer looks at
    const req = { method: 'GET', url: '/ws/signal?token=x', ip: '1.2.3.4', headers: { authorization: 'COGBox a:b' } };
    const r = logSafeRequest(req);
    expect(r).toEqual({ method: 'GET', url: '/ws/signal?token=REDACTED' });
  });
});
