import { describe, it, expect, afterEach } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { initDb, DatabaseInstance } from '../src/db.js';
import { buildServer } from '../src/server.js';
import { clearLobbies, sweepOrphanLobbies, Heartbeat, LOBBY_IDLE_MS } from '../src/social/lobby_cleanup.js';

const HOUR = 60 * 60 * 1000;

function addLobby(db: DatabaseInstance, id: string, host: string, updatedAt: number) {
  db.prepare(`INSERT INTO lobbies (lobby_id, host_uuid, host_xuid, title_id, room_code, session_info, created_at, updated_at)
              VALUES (?, ?, 'x', '4D5308AB', ?, '{}', ?, ?)`).run(id, host, `COG-${id}`, updatedAt, updatedAt);
}
const ids = (db: DatabaseInstance) =>
  (db.prepare('SELECT lobby_id FROM lobbies ORDER BY lobby_id').all() as { lobby_id: string }[]).map((r) => r.lobby_id);

describe('stale lobby cleanup', () => {
  it('waits an hour before a lobby without its host counts as stale', () => {
    expect(LOBBY_IDLE_MS).toBe(HOUR);
  });

  it('clears every lobby (at start-up nobody is connected, so none can be live)', () => {
    const db = initDb(':memory:');
    addLobby(db, 'A', 'host-a', 1);
    addLobby(db, 'B', 'host-b', 2);
    expect(clearLobbies(db)).toBe(2);
    expect(ids(db)).toEqual([]);
  });

  it('sweeps a lobby only when its host has no game connection and it has not changed for an hour', () => {
    const db = initDb(':memory:');
    const now = 10 * HOUR;
    addLobby(db, 'gone-old', 'gone', now - HOUR - 1);          // host gone, idle over an hour: swept
    addLobby(db, 'gone-recent', 'gone2', now - HOUR + 60_000); // host gone, but changed within the hour: kept
    addLobby(db, 'here-old', 'here', now - 5 * 24 * HOUR);     // host connected: kept however old
    const online = (uuid: string) => uuid === 'here';
    expect(sweepOrphanLobbies(db, online, now)).toBe(1);
    expect(ids(db)).toEqual(['gone-recent', 'here-old']);
  });

  it('closes a connection that misses three pings in a row, and keeps one that answers', () => {
    const made = () => ({ pings: 0, closed: false, ping() { this.pings++; }, terminate() { this.closed = true; } });
    const quiet = made();
    const answering = made();
    const hb = new Heartbeat();
    hb.add(quiet);
    hb.add(answering);
    for (let i = 0; i < 3; i++) {
      hb.tick();
      hb.pong(answering);
    }
    expect(quiet.closed).toBe(false);   // three pings sent, none answered yet
    expect(quiet.pings).toBe(3);
    hb.tick();                          // the fourth tick: three missed
    expect(quiet.closed).toBe(true);
    expect(answering.closed).toBe(false);
    expect(answering.pings).toBe(4);
    hb.remove(answering);
    hb.tick();
    expect(answering.pings).toBe(4);    // removed connections are not pinged any more
  });
});

describe('server start-up', () => {
  let dir = '';
  afterEach(() => { if (dir) fs.rmSync(dir, { recursive: true, force: true }); });

  it('clears the lobbies left by the previous run', async () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cogbox-lobbies-'));
    const file = path.join(dir, 'master.db');
    const before = initDb(file);
    addLobby(before, 'left-over', 'old-host', 1);
    before.close();
    const app = await buildServer({ dbPath: file });
    const db = (app as unknown as { db: DatabaseInstance }).db;
    expect(ids(db)).toEqual([]);
    await app.close();
  });
});
