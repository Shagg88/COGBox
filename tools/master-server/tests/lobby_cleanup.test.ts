import { describe, it, expect, afterEach } from 'vitest';
import WebSocket from 'ws';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { initDb, DatabaseInstance } from '../src/db.js';
import { buildServer } from '../src/server.js';
import { sweepOrphanLobbies, Heartbeat } from '../src/social/lobby_cleanup.js';

const HOUR = 60 * 60 * 1000;
const wait = (ms: number) => new Promise((r) => setTimeout(r, ms));

function addLobby(db: DatabaseInstance, id: string, host: string, updatedAt: number) {
  db.prepare(`INSERT INTO lobbies (lobby_id, host_uuid, host_xuid, title_id, room_code, session_info, created_at, updated_at)
              VALUES (?, ?, 'x', '4D5308AB', ?, '{}', ?, ?)`).run(id, host, `COG-${id}`, updatedAt, updatedAt);
}
const ids = (db: DatabaseInstance) =>
  (db.prepare('SELECT lobby_id FROM lobbies ORDER BY lobby_id').all() as { lobby_id: string }[]).map((r) => r.lobby_id);

describe('stale lobby cleanup', () => {
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

// The real thing: a server with fast timings and real game sockets.
describe('stale lobby cleanup with game connections', () => {
  let app: any;
  let address = '';
  const sockets: WebSocket[] = [];
  let tmp = '';
  afterEach(async () => {
    // Let the server finish each socket's close (presence, lobby cleanup) before its database closes.
    await Promise.all(sockets.map((s) => s.readyState === WebSocket.CLOSED ? null
      : new Promise((r) => { s.once('close', r); s.terminate(); })));
    sockets.length = 0;
    await wait(50);
    if (app) await app.close();
    app = undefined;
    if (tmp) fs.rmSync(tmp, { recursive: true, force: true });
    tmp = '';
  });

  async function start(opts: Record<string, number>, dbPath = ':memory:') {
    app = await buildServer({ dbPath, rateLimitMax: 10000, ...opts });
    await app.listen({ port: 0, host: '127.0.0.1' });
    address = `ws://127.0.0.1:${app.server.address().port}`;
  }
  async function claim(name: string): Promise<{ uuid: string; token: string }> {
    const res = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: name } });
    return res.json();
  }
  function game(c: { uuid: string; token: string }, autoPong = true): Promise<WebSocket> {
    const ws = new WebSocket(`${address}/ws/signal?uuid=${c.uuid}&token=${c.token}&role=game&title_id=4D5308AB`,
                             { autoPong });
    sockets.push(ws);
    return new Promise((resolve, reject) => { ws.once('open', () => resolve(ws)); ws.once('error', reject); });
  }
  const db = () => app.db as DatabaseInstance;

  it('closes a game that stops answering pings, and its lobby goes with it', async () => {
    await start({ heartbeatIntervalMs: 100 });
    const quiet = await claim('Quiet');
    const live = await claim('Live');
    addLobby(db(), 'quiet-lobby', quiet.uuid, Date.now());
    addLobby(db(), 'live-lobby', live.uuid, Date.now());
    await game(quiet, false);   // never answers a ping
    await game(live);           // answers, as WinHTTP does
    await wait(900);            // nine beats: three misses close it
    expect(ids(db())).toEqual(['live-lobby']);
  });

  it('keeps the lobby of a game that reconnected when its old socket is closed', async () => {
    await start({ heartbeatIntervalMs: 100 });
    const host = await claim('Host');
    const first = await game(host, false);   // the old socket, about to be replaced and then go quiet
    const replaced = new Promise((r) => first.once('close', r));
    await game(host);                         // the new socket replaces it (4002)
    await replaced;
    addLobby(db(), 'host-lobby', host.uuid, Date.now());
    await wait(700);
    expect(ids(db())).toEqual(['host-lobby']);
  });

  it('after a restart, removes the lobbies of hosts that did not reconnect and keeps the others', async () => {
    tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'cogbox-lobbies-'));
    const file = path.join(tmp, 'master.db');
    // The previous run: two hosts with open lobbies when the server stops.
    await start({}, file);
    const back = await claim('Back');
    const gone = await claim('Gone');
    addLobby(db(), 'back-lobby', back.uuid, Date.now());
    addLobby(db(), 'gone-lobby', gone.uuid, Date.now());
    await app.close();
    // The restart: both lobbies are still listed until the sweep...
    await start({ startupSweepMs: 1000 }, file);
    expect(ids(db())).toEqual(['back-lobby', 'gone-lobby']);
    await game(back);                                   // ...and one host's game reconnects in time
    addLobby(db(), 'new-lobby', 'no-socket-yet', Date.now() + 1);   // made after the restart: not the sweep's business
    await wait(1300);
    expect(ids(db())).toEqual(['back-lobby', 'new-lobby']);

  });

  it('the regular sweep uses the live connections', async () => {
    await start({ sweepIntervalMs: 40 });
    const here = await claim('Here');
    const gone = await claim('Gone');
    await game(here);
    addLobby(db(), 'here-lobby', here.uuid, Date.now() - 2 * HOUR);
    addLobby(db(), 'gone-lobby', gone.uuid, Date.now() - 2 * HOUR);
    await wait(200);
    expect(ids(db())).toEqual(['here-lobby']);
  });
});
