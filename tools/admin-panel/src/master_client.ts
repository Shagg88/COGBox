import type { Backup, FabAction, FabReportGroup, FabStats, Lobby, MasterClient, Status } from './types.js';

const STATUS_MS = 2000;   // the overview must not hang on a dead master
const CALL_MS = 10000;
const BACKUP_MS = 60000;  // db.backup() on a big file is slow; a timeout would report failure while it completes

export function createMasterClient(baseUrl: string, secret: string): MasterClient {
  const base = baseUrl.replace(/\/+$/, '');
  const call = async <T>(method: string, path: string, ms = CALL_MS, body?: unknown): Promise<T> => {
    const res = await fetch(base + path, {
      method,
      headers: body === undefined ? { 'x-admin-secret': secret } : { 'x-admin-secret': secret, 'content-type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(ms)
    });
    if (!res.ok) throw new Error(`master ${method} ${path}: ${res.status}`);
    return (await res.json()) as T;
  };

  return {
    // the overview must still render when the master server is down, so any failure is just "no answer"
    async status(): Promise<Status | null> {
      try {
        return await call<Status>('GET', '/status', STATUS_MS);
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
      return call('POST', '/backup', BACKUP_MS);
    },
    async backups(): Promise<Backup[]> {
      return (await call<{ backups: Backup[] }>('GET', '/backups')).backups;
    },
    async fabReports(): Promise<FabReportGroup[]> {
      return (await call<{ reports: FabReportGroup[] }>('GET', '/fabricator/reports')).reports;
    },
    fabStats(): Promise<FabStats> {
      return call('GET', '/fabricator/stats');
    },
    async fabAction(id: number, action: FabAction, who: string): Promise<boolean> {
      // deleting files of a big mod touches the disk: give it the backup's time
      return (await call<{ ok: boolean }>('POST', `/fabricator/mods/${encodeURIComponent(String(id))}/${action}`, BACKUP_MS, { who })).ok;
    },
    async fabPublishing(tag: string, allowed: boolean, who: string): Promise<boolean> {
      return (await call<{ ok: boolean }>('POST', `/fabricator/creators/${encodeURIComponent(tag)}/publishing`, CALL_MS, { allowed, who })).ok;
    }
  };
}
