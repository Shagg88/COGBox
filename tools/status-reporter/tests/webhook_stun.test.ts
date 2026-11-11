import { describe, expect, it } from 'vitest';
import dgram from 'node:dgram';
import { publish } from '../src/webhook.js';
import { bindingRequest, isBindingSuccess, checkStun } from '../src/stun.js';

const HOOK = 'https://discord.com/api/webhooks/123/tok';

function hookFetch(answers: { status: number; body?: unknown; headers?: Record<string, string> }[]) {
  const calls: { method: string; url: string }[] = [];
  const f = async (url: string | URL, init?: RequestInit) => {
    calls.push({ method: init?.method ?? 'GET', url: String(url) });
    const a = answers.shift() ?? { status: 500 };
    return new Response(a.body === undefined ? null : JSON.stringify(a.body), { status: a.status, headers: a.headers });
  };
  return { f: f as typeof fetch, calls };
}

describe('publish', () => {
  it('creates the message once with wait=true and returns its id', async () => {
    const { f, calls } = hookFetch([{ status: 200, body: { id: '999' } }]);
    const r = await publish(f, HOOK, null, { title: 't' } as any);
    expect(r).toEqual({ messageId: '999' });
    expect(calls).toEqual([{ method: 'POST', url: `${HOOK}?wait=true` }]);
  });
  it('edits the stored message', async () => {
    const { f, calls } = hookFetch([{ status: 200, body: { id: '999' } }]);
    expect(await publish(f, HOOK, '999', {} as any)).toEqual({ messageId: '999' });
    expect(calls[0]).toEqual({ method: 'PATCH', url: `${HOOK}/messages/999` });
  });
  it('recreates the message when it was deleted', async () => {
    const { f, calls } = hookFetch([{ status: 404, body: { code: 10008 } }, { status: 200, body: { id: '1000' } }]);
    expect(await publish(f, HOOK, '999', {} as any)).toEqual({ messageId: '1000' });
    expect(calls.map((c) => c.method)).toEqual(['PATCH', 'POST']);
  });
  it('backs off on 429 using retry_after', async () => {
    const { f } = hookFetch([{ status: 429, body: { retry_after: 7.5 } }]);
    expect(await publish(f, HOOK, '999', {} as any)).toEqual({ messageId: '999', retryAfterS: 8 });
  });
  it('never puts the webhook URL in an error', async () => {
    const { f } = hookFetch([{ status: 500 }]);
    await expect(publish(f, HOOK, '999', {} as any)).rejects.toThrow(/500/);
    await expect(publish(hookFetch([{ status: 500 }]).f, HOOK, '999', {} as any)).rejects.not.toThrow(/tok/);
  });
});

describe('stun', () => {
  it('matches only a success response with the same transaction id', () => {
    const req = bindingRequest();
    const res = Buffer.from(req);
    res.writeUInt16BE(0x0101, 0);
    expect(isBindingSuccess(res, req)).toBe(true);
    const other = Buffer.from(res); other[19] ^= 0xff;
    expect(isBindingSuccess(other, req)).toBe(false);
    expect(isBindingSuccess(req, req)).toBe(false);
  });
  it('answers up against a local fake STUN server and down on silence', async () => {
    const srv = dgram.createSocket('udp4');
    srv.on('message', (m, r) => { const o = Buffer.from(m); o.writeUInt16BE(0x0101, 0); srv.send(o, r.port, r.address); });
    await new Promise<void>((res) => srv.bind(0, '127.0.0.1', res));
    const port = (srv.address() as { port: number }).port;
    expect((await checkStun('127.0.0.1', port, 1000)).state).toBe('up');
    srv.close();
    const silent = dgram.createSocket('udp4');
    await new Promise<void>((res) => silent.bind(0, '127.0.0.1', res));
    const sp = (silent.address() as { port: number }).port;
    const l = await checkStun('127.0.0.1', sp, 300);
    silent.close();
    expect(l).toMatchObject({ state: 'down', core: true });
  });
});
