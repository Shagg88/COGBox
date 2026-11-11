import { describe, it, expect, afterEach } from 'vitest';
import http from 'node:http';
import type { AddressInfo } from 'node:net';
import { createMasterClient } from '../src/master_client.js';
import { createDockerClient } from '../src/docker_client.js';

type Handler = (req: http.IncomingMessage, res: http.ServerResponse) => void;
const servers: http.Server[] = [];

async function fake(handler: Handler): Promise<string> {
  const s = http.createServer(handler);
  servers.push(s);
  await new Promise<void>((r) => s.listen(0, '127.0.0.1', r));
  return `http://127.0.0.1:${(s.address() as AddressInfo).port}`;
}

afterEach(() => {
  for (const s of servers.splice(0)) { s.closeAllConnections(); s.close(); }
});

const frame = (stream: number, text: string): Buffer => {
  const body = Buffer.from(text);
  const head = Buffer.alloc(8);
  head[0] = stream;
  head.writeUInt32BE(body.length, 4);
  return Buffer.concat([head, body]);
};

describe('master client', () => {
  it('master client sends the secret', async () => {
    const seen: { url?: string; secret?: string | string[] }[] = [];
    const url = await fake((req, res) => {
      seen.push({ url: req.url, secret: req.headers['x-admin-secret'] });
      res.setHeader('content-type', 'application/json');
      if (req.url === '/lobbies/abc/close') return res.end('{"closed":true}');
      if (req.url === '/backup') return res.end('{"name":"master-20260101000000.db","size":5}');
      if (req.url === '/backups') return res.end('{"backups":[{"name":"master-20260101000000.db","size":5,"mtime":1}]}');
      if (req.url === '/lobbies') return res.end('{"lobbies":[]}');
      res.end(JSON.stringify({ uptime_s: 1, version: 'x', players: { total: 0, by_title: {}, by_region: {} }, lobbies: 0, pending_invites: 0, errors: [] }));
    });
    const c = createMasterClient(url, 's3cret');
    expect((await c.status())?.version).toBe('x');
    expect(await c.lobbies()).toEqual([]);
    expect(await c.closeLobby('abc')).toBe(true);
    expect(await c.backup()).toEqual({ name: 'master-20260101000000.db', size: 5 });
    expect((await c.backups())[0].name).toBe('master-20260101000000.db');
    expect(seen).toHaveLength(5);
    expect(seen.every((s) => s.secret === 's3cret')).toBe(true);
  });

  it('backup waits longer than the status timeout', async () => {
    const url = await fake((_q, res) => { setTimeout(() => res.end('{"name":"master-20260101000000.db","size":5}'), 2500); });
    expect(await createMasterClient(url, 's').backup()).toEqual({ name: 'master-20260101000000.db', size: 5 });
  }, 10000);

  it('master status null on timeout', async () => {
    const url = await fake(() => { /* never answers */ });
    expect(await createMasterClient(url, 's').status()).toBeNull();
  }, 10000);

  it('master status null on refused connection or 401', async () => {
    expect(await createMasterClient('http://127.0.0.1:1', 's').status()).toBeNull();
    const url = await fake((_q, res) => { res.statusCode = 401; res.end('{"error":"unauthorized"}'); });
    expect(await createMasterClient(url, 'bad').status()).toBeNull();
  });
});

describe('docker client', () => {
  it('docker restart hits only the named container\'s restart path', async () => {
    const seen: string[] = [];
    const url = await fake((req, res) => { seen.push(`${req.method} ${req.url}`); res.statusCode = 204; res.end(); });
    await createDockerClient(url).restart('coturn');
    expect(seen).toEqual(['POST /containers/cogbox-master-coturn-1/restart?t=5']);
  });

  it('docker restart refuses caddy', async () => {
    let hits = 0;
    const url = await fake((_q, res) => { hits++; res.end(); });
    const d = createDockerClient(url);
    await expect(d.restart('caddy' as any)).rejects.toThrow();
    await expect(d.logs('caddy' as any, 5)).rejects.toThrow();
    expect(hits).toBe(0);
  });

  it('logs strips the 8-byte stream headers', async () => {
    let seen = '';
    const url = await fake((req, res) => {
      seen = req.url!;
      res.end(Buffer.concat([frame(1, 'one\ntwo\n'), frame(2, 'three\n')]));
    });
    const lines = await createDockerClient(url).logs('master-server', 3);
    expect(lines).toEqual(['one', 'two', 'three']);
    expect(seen).toBe('/containers/cogbox-master-master-server-1/logs?stdout=1&stderr=1&tail=3');
  });

  it('health reports a restarting or stopped container as down', async () => {
    const url = await fake((req, res) => {
      const restarting = req.url!.includes('coturn');
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ State: { Running: restarting, Restarting: restarting, StartedAt: 't' }, RestartCount: 7 }));
    });
    const h = await createDockerClient(url).health();
    expect(h.coturn.up).toBe(false);
    expect(h['master-server'].up).toBe(false);
  });

  it('health reads state, and treats a missing container as down', async () => {
    const url = await fake((req, res) => {
      if (req.url === '/containers/cogbox-master-caddy-1/json') { res.statusCode = 404; return res.end('{"message":"no such"}'); }
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ State: { Running: true, StartedAt: '2026-01-01T00:00:00Z' }, RestartCount: 2 }));
    });
    const h = await createDockerClient(url).health();
    expect(h['master-server']).toEqual({ up: true, started_at: '2026-01-01T00:00:00Z', restarts: 2 });
    expect(h.caddy).toEqual({ up: false, started_at: '', restarts: 0 });
  });
});
