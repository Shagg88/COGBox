import type { Backup, Lobby, MasterClient, Status } from './types.js';

const TIMEOUT_MS = 2000;

export function createMasterClient(baseUrl: string, secret: string): MasterClient {
  const base = baseUrl.replace(/\/+$/, '');
  const call = async <T>(method: string, path: string): Promise<T> => {
    const res = await fetch(base + path, {
      method,
      headers: { 'x-admin-secret': secret },
      signal: AbortSignal.timeout(TIMEOUT_MS)
    });
    if (!res.ok) throw new Error(`master ${method} ${path}: ${res.status}`);
    return (await res.json()) as T;
  };

  return {
    // the overview must still render when the master server is down, so any failure is just "no answer"
    async status(): Promise<Status | null> {
      try {
        return await call<Status>('GET', '/status');
      } catch {
        return null;
      }
    },
    async lobbies(): Promise<Lobby[]> {
      return (await call<{ lobbies: Lobby[] }>('GET', '/lobbies')).lobbies;
    },
    async closeLobby(id: string): Promise<boolean> {
      return (await call<{ closed: boolean }>('POST', `/lobbies/${encodeURIComponent(id)}/close`)).closed;
    },
    backup(): Promise<{ name: string; size: number }> {
      return call('POST', '/backup');
    },
    async backups(): Promise<Backup[]> {
      return (await call<{ backups: Backup[] }>('GET', '/backups')).backups;
    }
  };
}
