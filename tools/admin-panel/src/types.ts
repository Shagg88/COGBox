// Shared by buildApp and the clients (Task 4 implements them); types mirror master-server's private API.

export interface Status {
  uptime_s: number;
  version: string;
  players: { total: number; by_title: Record<string, number>; by_region: Record<string, number> };
  lobbies: number;
  pending_invites: number;
  errors: { time: number; msg: string }[];
}

export interface Lobby {
  lobby_id: string;
  room_code: string;
  title_id: string;
  host_tag: string;
  is_public: boolean;
  created_at: number;
}

export interface Backup { name: string; size: number; mtime: number }

export interface MasterClient {
  status(): Promise<Status | null>; // null when no answer within 2 s
  lobbies(): Promise<Lobby[]>;
  closeLobby(id: string): Promise<boolean>;
  backup(): Promise<{ name: string; size: number }>;
  backups(): Promise<Backup[]>;
}

export type Service = 'master-server' | 'coturn' | 'caddy';

export const CONTAINERS: Record<Service, string> = {
  'master-server': 'cogbox-master-master-server-1',
  coturn: 'cogbox-master-coturn-1',
  caddy: 'cogbox-master-caddy-1',
};

export interface ServiceHealth { up: boolean; started_at: string; restarts: number }

export interface DockerClient {
  health(): Promise<Record<Service, ServiceHealth>>;
  logs(s: 'master-server' | 'coturn', lines: number): Promise<string[]>;
  restart(s: 'master-server' | 'coturn'): Promise<void>;
}
