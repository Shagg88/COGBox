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

export interface FabStats {
  disk: { total: number; free: number };
  month_bytes: number;   // bytes of versions published in the last 30 days
  mods: number;
  creators: number;
  open_reports: number;
}

// Open reports for one mod; players are tags (display_name#discriminator), never uuids.
export interface FabReportGroup {
  mod: { id: number; name: string; game: string; author: string; state: string };
  reports: { id: number; reason: string; text: string; reporter: string; created_at: number }[];
}

export type FabAction = 'takedown' | 'restore' | 'delete-files';

export interface MasterClient {
  status(): Promise<Status | null>; // null when no answer within 2 s
  lobbies(): Promise<Lobby[]>;
  closeLobby(id: string): Promise<boolean>;
  backup(): Promise<{ name: string; size: number }>;
  backups(): Promise<Backup[]>;
  fabReports(): Promise<FabReportGroup[]>;
  fabStats(): Promise<FabStats>;
  fabAction(id: number, action: FabAction, who: string): Promise<boolean>;      // false: no such mod, or not in a state for it
  fabPublishing(tag: string, allowed: boolean, who: string): Promise<boolean>;  // false: no account has that tag
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
