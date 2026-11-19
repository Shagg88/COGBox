import { DatabaseInstance } from '../db.js';

// Stale lobbies. A host's lobbies are deleted when its game's connection to us closes (signaling.ts). That close never
// came for the lobbies open when the server restarted, nor for a game whose PC dropped off the network: those stayed
// listed for days. Three safety nets:
//  - STARTUP_SWEEP_MS after start-up, the lobbies whose host has not reconnected go. Not at start-up itself: a game
//    reconnects its socket within seconds but never re-creates its lobby, so clearing them would delist every live
//    match on each restart;
//  - every SWEEP_INTERVAL_MS a lobby whose host has no game connection and that has not changed for LOBBY_IDLE_MS goes;
//  - connections are pinged, and one that misses HEARTBEAT_MISSES pings in a row is closed (its normal cleanup runs).

export const LOBBY_IDLE_MS = 60 * 60 * 1000;      // an hour without its host before a lobby counts as stale
export const STARTUP_SWEEP_MS = 2 * 60 * 1000;   // after a restart, time for live games to reconnect
export const SWEEP_INTERVAL_MS = 5 * 60 * 1000;   // how often that is checked
export const HEARTBEAT_INTERVAL_MS = 60 * 1000;   // a ping a minute...
export const HEARTBEAT_MISSES = 3;                // ...and a connection is closed after three unanswered

// Deletes the lobbies idle for `idleMs` whose host `isOnline` says has no game connection. Returns how many.
export function sweepOrphanLobbies(db: DatabaseInstance, isOnline: (uuid: string) => boolean, now: number,
                                   idleMs: number = LOBBY_IDLE_MS): number {
  const idle = db.prepare('SELECT lobby_id, host_uuid FROM lobbies WHERE updated_at < ?').all(now - idleMs) as
    { lobby_id: string; host_uuid: string }[];
  const del = db.prepare('DELETE FROM lobbies WHERE lobby_id = ?');
  let n = 0;
  for (const l of idle) {
    if (!isOnline(l.host_uuid)) n += del.run(l.lobby_id).changes;
  }
  return n;
}

// What the heartbeat needs of a connection (a ws WebSocket has both).
export interface Pingable { ping(): void; terminate(): void }

export class Heartbeat {
  private missed = new Map<Pingable, number>();
  add(c: Pingable): void { this.missed.set(c, 0); }
  remove(c: Pingable): void { this.missed.delete(c); }
  pong(c: Pingable): void { if (this.missed.has(c)) this.missed.set(c, 0); }
  // One beat: a connection that missed HEARTBEAT_MISSES pings in a row is closed, every other one pinged again.
  tick(): void {
    for (const [c, n] of this.missed) {
      if (n >= HEARTBEAT_MISSES) {
        this.missed.delete(c);
        try { c.terminate(); } catch { /* already gone */ }
        continue;
      }
      this.missed.set(c, n + 1);
      try { c.ping(); } catch { /* a closing socket: its close handler removes it */ }
    }
  }
}
