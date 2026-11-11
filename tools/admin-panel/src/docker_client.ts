import { CONTAINERS, type DockerClient, type Service, type ServiceHealth } from './types.js';

const TIMEOUT_MS = 10000;
type Controllable = 'master-server' | 'coturn';

// Only these two may be logged or restarted; checked at runtime because callers can pass any string.
const controllable = (s: string): Controllable => {
  if (s !== 'master-server' && s !== 'coturn') throw new Error(`service not allowed: ${s}`);
  return s;
};

// Docker multiplexes stdout/stderr: each frame is 8 header bytes (type, 3 zeros, big-endian length) then the payload.
function demux(buf: Buffer): string {
  const parts: Buffer[] = [];
  let i = 0;
  while (i + 8 <= buf.length) {
    const len = buf.readUInt32BE(i + 4);
    parts.push(buf.subarray(i + 8, i + 8 + len));
    i += 8 + len;
  }
  return Buffer.concat(parts).toString('utf8');
}

export function createDockerClient(baseUrl: string): DockerClient {
  const base = baseUrl.replace(/\/+$/, '');
  const call = (method: string, path: string) =>
    fetch(base + path, { method, signal: AbortSignal.timeout(TIMEOUT_MS) });

  return {
    async health(): Promise<Record<Service, ServiceHealth>> {
      const out = {} as Record<Service, ServiceHealth>;
      await Promise.all((Object.keys(CONTAINERS) as Service[]).map(async (s) => {
        try {
          const res = await call('GET', `/containers/${CONTAINERS[s]}/json`);
          if (!res.ok) throw new Error(String(res.status));
          const j = (await res.json()) as { State?: { Running?: boolean; Restarting?: boolean; StartedAt?: string }; RestartCount?: number };
          // a crash-looping container reports Running too, so Restarting counts as down
          out[s] = { up: Boolean(j.State?.Running) && !j.State?.Restarting, started_at: j.State?.StartedAt ?? '', restarts: j.RestartCount ?? 0 };
        } catch {
          out[s] = { up: false, started_at: '', restarts: 0 };   // missing or unreachable counts as down
        }
      }));
      return out;
    },
    async logs(s, lines): Promise<string[]> {
      const name = CONTAINERS[controllable(s)];
      const n = Math.max(1, Math.floor(lines));
      const res = await call('GET', `/containers/${name}/logs?stdout=1&stderr=1&tail=${n}`);
      if (!res.ok) throw new Error(`docker logs: ${res.status}`);
      const text = demux(Buffer.from(await res.arrayBuffer()));
      return text.split('\n').filter((l, i, a) => l !== '' || i < a.length - 1).map((l) => l.replace(/\r$/, ''));
    },
    async restart(s): Promise<void> {
      const name = CONTAINERS[controllable(s)];
      const res = await call('POST', `/containers/${name}/restart?t=5`);
      if (!res.ok) throw new Error(`docker restart: ${res.status}`);
    }
  };
}
