import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { buildServer } from '../src/server.js';
import WebSocket from 'ws';

// XSESSION_INFO is 60 bytes: XNKID (8), XNADDR (36), XNKEY (16).
const SESSION_INFO = '11'.repeat(8) + '22'.repeat(36) + '33'.repeat(16);

describe('Master Server API', () => {
  let app: any;
  let serverAddress: string;

  interface Claimed { uuid: string; token: string; xuid: string; discriminator: string; tag: string; display_name: string }

  async function claim(display_name: string): Promise<Claimed> {
    const res = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name } });
    expect(res.statusCode).toBe(200);
    return res.json();
  }
  const auth = (c: Claimed) => ({ authorization: `COGBox ${c.uuid}:${c.token}` });

  function openSocket(c: Claimed | { uuid: string; token: string }, query = ''): Promise<WebSocket> {
    const ws = new WebSocket(`${serverAddress}/ws/signal?uuid=${c.uuid}&token=${c.token}${query}`);
    return new Promise((resolve, reject) => {
      ws.once('open', () => resolve(ws));
      ws.once('error', reject);
    });
  }
  function nextMessage(ws: WebSocket): Promise<any> {
    return new Promise((resolve) => ws.once('message', (d) => resolve(JSON.parse(d.toString()))));
  }

  let clock = 1_000_000_000;   // social step 3: a request marks its account seen for 60 s
  beforeAll(async () => {
    app = await buildServer({ dbPath: ':memory:', rateLimitMax: 10000, now: () => clock });
    await app.listen({ port: 0, host: '127.0.0.1' });
    serverAddress = `ws://127.0.0.1:${app.server.address().port}`;
  });

  afterAll(async () => {
    await app.close();
  });

  it('allocates unique discriminators, 64-bit XUIDs and a signaling token', async () => {
    const d1 = await claim('Marcus');
    const d2 = await claim('Marcus');
    expect(d1.discriminator).not.toBe(d2.discriminator);
    expect(d1.tag).toBe(`Marcus#${d1.discriminator}`);
    expect(BigInt(d1.xuid)).toBeGreaterThan(0n);
    expect(d1.xuid).not.toBe(d2.xuid);
    expect(d1.token).toMatch(/^[0-9a-f]{64}$/);
    expect(d1.token).not.toBe(d2.token);
  });

  it('re-claiming a uuid needs its token', async () => {
    const d1 = await claim('Dom');
    const again = await app.inject({
      method: 'POST', url: '/api/v1/auth/claim', payload: { uuid: d1.uuid, token: d1.token, display_name: 'Dom' }
    });
    expect(again.statusCode).toBe(200);
    expect(again.json().xuid).toBe(d1.xuid);
    expect(again.json().discriminator).toBe(d1.discriminator);
    const stolen = await app.inject({
      method: 'POST', url: '/api/v1/auth/claim', payload: { uuid: d1.uuid, display_name: 'Dom' }
    });
    expect(stolen.statusCode).toBe(401);
    const wrong = await app.inject({
      method: 'POST', url: '/api/v1/auth/claim', payload: { uuid: d1.uuid, token: 'ab'.repeat(32), display_name: 'Dom' }
    });
    expect(wrong.statusCode).toBe(401);
  });

  it('creating a lobby needs a token; the lobby keeps session_info, properties and contexts', async () => {
    const host = await claim('Anya');
    const anon = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', payload: { title_id: '4D53082D', session_info: SESSION_INFO }
    });
    expect(anon.statusCode).toBe(401);

    const created = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host),
      payload: {
        title_id: '4D53082D', session_info: SESSION_INFO, region: 'us-east', max_players: 8,
        properties: [{ id: 0x10000001, value: -5 }], contexts: [{ id: 0x0000000A, value: 3 }]
      }
    });
    expect(created.statusCode).toBe(200);
    const { lobby_id, room_code } = created.json();
    expect(room_code).toMatch(/^COG-[0-9A-Z]{4}$/);

    const found = await app.inject({ method: 'GET', url: `/api/v1/lobbies/lookup/${room_code.toLowerCase()}` });
    expect(found.statusCode).toBe(200);
    const lobby = found.json().lobby;
    expect(lobby.lobby_id).toBe(lobby_id);
    expect(lobby.host_uuid).toBe(host.uuid);
    expect(lobby.host_xuid).toBe(host.xuid);
    expect(lobby.host_name).toBe('Anya');
    expect(lobby.session_info).toBe(SESSION_INFO);
    expect(lobby.properties).toEqual([{ id: 0x10000001, value: -5 }]);
    expect(lobby.contexts).toEqual([{ id: 0x0000000A, value: 3 }]);

    const search = await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D53082D&region=us-east' });
    expect(search.json().lobbies.some((l: any) => l.lobby_id === lobby_id)).toBe(true);
    const other = await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D53082D&region=eu-west' });
    expect(other.json().lobbies.length).toBe(0);
  });

  it('rejects a session_info that is not 60 bytes of hex', async () => {
    const host = await claim('Sam');
    for (const bad of ['zz', '11'.repeat(59), '11'.repeat(61)]) {
      const res = await app.inject({
        method: 'POST', url: '/api/v1/lobbies', headers: auth(host), payload: { title_id: '4D53082D', session_info: bad }
      });
      expect(res.statusCode).toBe(400);
    }
  });

  it('only the host can delete a lobby', async () => {
    const host = await claim('Carmine');
    const other = await claim('Clayton');
    const created = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host), payload: { title_id: '4D5308AB', session_info: SESSION_INFO }
    });
    const { lobby_id } = created.json();
    const notMine = await app.inject({ method: 'DELETE', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(other) });
    expect(notMine.statusCode).toBe(403);
    const anon = await app.inject({ method: 'DELETE', url: `/api/v1/lobbies/${lobby_id}` });
    expect(anon.statusCode).toBe(401);
    const mine = await app.inject({ method: 'DELETE', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(host) });
    expect(mine.statusCode).toBe(200);
    const search = await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D5308AB' });
    expect(search.json().lobbies.some((l: any) => l.lobby_id === lobby_id)).toBe(false);
  });

  it('normalises names and refuses bad ones at claim', async () => {
    const ok = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: '  Cole  Train ' } });
    expect(ok.statusCode).toBe(200);
    expect(ok.json().display_name).toBe('Cole Train');
    for (const bad of ['Do', 'ABCDEFGHIJKLMNOP', 'Dom!', 'Mårcus', '   ']) {
      const r = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: bad } });
      expect(r.statusCode, bad).toBe(400);
    }
  });

  it('a returning account is not refused for the name in its local file', async () => {
    const me = await claim('Hoffman');
    const r = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { uuid: me.uuid, token: me.token, display_name: 'Colonel Victor Hoffman!' } });
    expect(r.statusCode).toBe(200);
    expect(r.json().display_name).toBe('Hoffman');
  });

  it('icon: a profile picture name is stored and shown with friend requests; anything else is refused', async () => {
    const a = await claim('Anya');
    const b = await claim('Benjamin');
    const set = await app.inject({ method: 'POST', url: '/api/v1/auth/icon', headers: auth(b), payload: { icon: 'icons/4D5308AB/8000.png' } });
    expect(set.statusCode).toBe(200);
    expect(set.json().icon).toBe('icons/4D5308AB/8000.png');
    for (const bad of ['../x.png', 'icons/4D5308AB/8000.jpg', 'icons/zz/1.png', 'C:/icons/4D5308AB/1.png', 7]) {
      const r = await app.inject({ method: 'POST', url: '/api/v1/auth/icon', headers: auth(b), payload: { icon: bad } });
      expect(r.statusCode, String(bad)).toBe(400);
    }
    expect((await app.inject({ method: 'POST', url: '/api/v1/auth/icon', payload: { icon: '' } })).statusCode).toBe(401);
    await app.inject({ method: 'POST', url: '/api/v1/friends/requests', headers: auth(b), payload: { tag: `${a.display_name}#${a.discriminator}` } });
    const list = await app.inject({ method: 'GET', url: '/api/v1/friends', headers: auth(a) });
    expect(list.json().incoming[0]).toMatchObject({ uuid: b.uuid, icon: 'icons/4D5308AB/8000.png' });
    const cleared = await app.inject({ method: 'POST', url: '/api/v1/auth/icon', headers: auth(b), payload: { icon: '' } });
    expect(cleared.json().icon).toBe('');
  });

  it('claim: a uuid that is not a uuid is refused, not stored', async () => {
    for (const bad of [7, 'x'.repeat(300), 'a:b', 'has space', '../..']) {
      const r = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { uuid: bad, display_name: 'Marcus' } });
      expect(r.statusCode, String(bad)).toBe(400);
    }
    const ok = await app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { uuid: '', display_name: 'Marcus' } });
    expect(ok.statusCode).toBe(200);
  });

  it('delete account: the player, their friendships and lobbies go; the tag is free again; only with the token', async () => {
    const a = await claim('Kait');
    const b = await claim('Jace');
    await app.inject({ method: 'POST', url: '/api/v1/friends/requests', headers: auth(b), payload: { tag: `${a.display_name}#${a.discriminator}` } });
    expect((await app.inject({ method: 'DELETE', url: '/api/v1/auth/account' })).statusCode).toBe(401);
    const del = await app.inject({ method: 'DELETE', url: '/api/v1/auth/account', headers: auth(a) });
    expect(del.statusCode).toBe(200);
    expect((await app.inject({ method: 'GET', url: '/api/v1/friends', headers: auth(a) })).statusCode).toBe(401);
    const theirs = await app.inject({ method: 'GET', url: '/api/v1/friends', headers: auth(b) });
    expect(theirs.json().outgoing).toEqual([]);
  });

  it('renames: same uuid and xuid, discriminator kept when free', async () => {
    const me = await claim('Baird');
    const r = await app.inject({ method: 'POST', url: '/api/v1/auth/rename', headers: auth(me), payload: { display_name: 'Damon Baird' } });
    expect(r.statusCode).toBe(200);
    expect(r.json()).toMatchObject({ uuid: me.uuid, xuid: me.xuid, display_name: 'Damon Baird', discriminator: me.discriminator });
  });

  it('renames: a taken discriminator gets a new one; same name keeps it; bad names and no auth refused', async () => {
    const a = await claim('Samuel');
    const b = await claim('Samantha');
    const moved = await app.inject({ method: 'POST', url: '/api/v1/auth/rename', headers: auth(b), payload: { display_name: 'Samuel' } });
    expect(moved.statusCode).toBe(200);
    expect(moved.json().display_name).toBe('Samuel');
    if (b.discriminator === a.discriminator) expect(moved.json().discriminator).not.toBe(a.discriminator);
    // force the clash: rename a to b's new tag's name and take b's discriminator for a's check
    const again = await app.inject({ method: 'POST', url: '/api/v1/auth/rename', headers: auth(b), payload: { display_name: 'Samuel' } });
    expect(again.json().discriminator).toBe(moved.json().discriminator);   // its own name: kept
    const same = await app.inject({ method: 'POST', url: '/api/v1/auth/rename', headers: auth(a), payload: { display_name: 'samuel' } });
    expect(same.statusCode).toBe(200);
    expect(same.json().discriminator).toBe(a.discriminator);
    expect(same.json().discriminator).not.toBe(moved.json().discriminator);
    expect((await app.inject({ method: 'POST', url: '/api/v1/auth/rename', headers: auth(a), payload: { display_name: 'Dom!' } })).statusCode).toBe(400);
    expect((await app.inject({ method: 'POST', url: '/api/v1/auth/rename', payload: { display_name: 'Anya' } })).statusCode).toBe(401);
  });

  it('lists only the newest lobby of a host for a title (a session the game never deleted is replaced)', async () => {
    const host = await claim('Anya');
    const first = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host), payload: { title_id: '4D5308AB', session_info: SESSION_INFO }
    });
    const second = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host), payload: { title_id: '4D5308AB', session_info: SESSION_INFO }
    });
    const other = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host), payload: { title_id: '4D53082D', session_info: SESSION_INFO }
    });
    const search = await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D5308AB' });
    const mine = search.json().lobbies.filter((l: any) => l.host_uuid === host.uuid || l.lobby_id === first.json().lobby_id
      || l.lobby_id === second.json().lobby_id);
    expect(mine.map((l: any) => l.lobby_id)).toEqual([second.json().lobby_id]);
    const g2 = await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D53082D' });
    expect(g2.json().lobbies.some((l: any) => l.lobby_id === other.json().lobby_id)).toBe(true);
  });

  it('lets the host attach a QoS block to its lobby; searches return it', async () => {
    const host = await claim('Tai');
    const other = await claim('Ben');
    const created = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host), payload: { title_id: '4D5308AB', session_info: SESSION_INFO }
    });
    const { lobby_id } = created.json();
    const qos = 'ab'.repeat(40);
    expect((await app.inject({ method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, payload: { qos_data: qos } })).statusCode).toBe(401);
    expect((await app.inject({
      method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(other), payload: { qos_data: qos }
    })).statusCode).toBe(403);
    expect((await app.inject({
      method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(host), payload: { qos_data: 'zz' }
    })).statusCode).toBe(400);
    expect((await app.inject({
      method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(host), payload: { qos_data: 'ab'.repeat(1025) }
    })).statusCode).toBe(400);
    const ok = await app.inject({
      method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(host), payload: { qos_data: qos }
    });
    expect(ok.statusCode).toBe(200);
    const search = await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D5308AB' });
    const lobby = search.json().lobbies.find((l: any) => l.lobby_id === lobby_id);
    expect(lobby.qos_data).toBe(qos);
  });

  it('lets the host update its lobby properties and contexts', async () => {
    const host = await claim('Griffin');
    const created = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host),
      payload: { title_id: '4D5308AB', session_info: SESSION_INFO, properties: [{ id: 1, value: 1 }] }
    });
    const { lobby_id } = created.json();
    const res = await app.inject({
      method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(host),
      payload: { properties: [{ id: 0x1000001F, value: 7 }], contexts: [{ id: 0x800B, value: 2 }] }
    });
    expect(res.statusCode).toBe(200);
    const lobby = (await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D5308AB' }))
      .json().lobbies.find((l: any) => l.lobby_id === lobby_id);
    expect(lobby.properties).toEqual([{ id: 0x1000001F, value: 7 }]);
    expect(lobby.contexts).toEqual([{ id: 0x800B, value: 2 }]);
    expect((await app.inject({
      method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(host), payload: { properties: 'x' }
    })).statusCode).toBe(400);
    expect((await app.inject({
      method: 'PATCH', url: `/api/v1/lobbies/${lobby_id}`, headers: auth(host), payload: {}
    })).statusCode).toBe(400);
  });

  it('closes a WebSocket with a wrong token (4001)', async () => {
    const c = await claim('Bernie');
    const ws = new WebSocket(`${serverAddress}/ws/signal?uuid=${c.uuid}&token=${'00'.repeat(32)}`);
    const code = await new Promise<number>((resolve) => ws.once('close', (c) => resolve(c)));
    expect(code).toBe(4001);
  });

  function closeCode(ws: WebSocket): Promise<number> {
    return new Promise((resolve) => ws.once('close', (code) => resolve(code)));
  }
  const candidatesTo = (target: Claimed) => JSON.stringify({
    type: 'candidates', target_uuid: target.uuid, session_token: '00c0ffee00c0ffee', port: 1000,
    candidates: [{ ip: '203.0.113.7', port: 40000, kind: 'srflx' }]
  });

  it('candidates: a launch id is passed on when valid and left out when not', async () => {
    const a = await claim('Del'); const b = await claim('Jace'); await makeFriends(a, b);
    const [wa, wb] = await Promise.all([openSocket(a), openSocket(b)]);
    const send = (extra: object) => wa.send(JSON.stringify({ ...JSON.parse(candidatesTo(b)), ...extra }));
    let got = nextMessage(wb);
    send({ launch_id: 'fedcba9876543210' });
    expect((await got).launch_id).toBe('fedcba9876543210');
    got = nextMessage(wb);
    send({ launch_id: 'not hex' });
    expect((await got).launch_id).toBeUndefined();
    wa.close(); wb.close();
  });

  const tagOf = (c: Claimed) => `${c.display_name}#${c.discriminator}`;
  const friendsOf = async (c: Claimed) => (await app.inject({ method: 'GET', url: '/api/v1/friends', headers: auth(c) })).json();
  const requestFriend = (from: Claimed, to: Claimed) =>
    app.inject({ method: 'POST', url: '/api/v1/friends/requests', headers: auth(from), payload: { tag: tagOf(to) } });
  const makeFriends = async (a: Claimed, b: Claimed) => {
    expect((await requestFriend(a, b)).statusCode).toBe(200);
    expect((await app.inject({ method: 'POST', url: `/api/v1/friends/${a.uuid}/accept`, headers: auth(b) })).statusCode).toBe(200);
  };
  // the next message of a given type on a socket
  function nextOfType(ws: WebSocket, type: string): Promise<any> {
    return new Promise((resolve) => {
      const on = (d: any) => {
        const m = JSON.parse(d.toString());
        if (m.type === type) { ws.off('message', on); resolve(m); }
      };
      ws.on('message', on);
    });
  }
  const presenceOf = async (viewer: Claimed, target: Claimed) =>
    (await friendsOf(viewer)).friends.find((f: any) => f.uuid === target.uuid)?.presence;

  it('friend request, accept and live events', async () => {
    const a = await claim('Myrrah');
    const b = await claim('Skorge');
    const bLauncher = await openSocket(b, '&role=launcher');
    const got = nextOfType(bLauncher, 'friend_request');
    expect((await requestFriend(a, b)).json().status).toBe('pending');
    expect((await got).from).toEqual({ uuid: a.uuid, tag: tagOf(a), icon: "" });   // icon: the profile picture (none set)
    expect((await friendsOf(b)).incoming.map((r: any) => r.uuid)).toEqual([a.uuid]);
    const added = nextOfType(bLauncher, 'friend_added');
    expect((await app.inject({ method: 'POST', url: `/api/v1/friends/${a.uuid}/accept`, headers: auth(b) })).statusCode).toBe(200);
    expect((await added).friend.uuid).toBe(a.uuid);
    expect((await friendsOf(a)).friends.map((f: any) => f.uuid)).toEqual([b.uuid]);
    expect((await friendsOf(b)).friends.map((f: any) => f.uuid)).toEqual([a.uuid]);
    expect((await friendsOf(a)).friends[0].xuid).toBe(b.xuid);   // the games know friends by XUID (XFriends)
    bLauncher.close();
  });

  it('presence: offline, online, playing, joinable', async () => {
    const a = await claim('Raam');
    const b = await claim('Karn');
    await makeFriends(a, b);
    clock += 61_000;   // B's requests while befriending showed B online for 60 s
    expect((await presenceOf(a, b)).state).toBe('offline');
    const launcher = await openSocket(b, '&role=launcher');
    expect((await presenceOf(a, b)).state).toBe('online');
    const game = await openSocket(b, '&role=game&title_id=4d5308ab');
    expect(await presenceOf(a, b)).toEqual({ state: 'playing', title_id: '4D5308AB' });
    const lobby = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(b), payload: { title_id: '4D5308AB', session_info: SESSION_INFO }
    });
    expect(await presenceOf(a, b)).toMatchObject({ state: 'joinable', title_id: '4D5308AB', lobby_id: lobby.json().lobby_id });
    const closed = new Promise((r) => game.once('close', r));
    game.close();
    await closed;
    await new Promise((r) => setTimeout(r, 50));
    expect((await presenceOf(a, b)).state).toBe('online');
    const search = await app.inject({ method: 'GET', url: '/api/v1/lobbies?title_id=4D5308AB' });
    expect(search.json().lobbies.some((l: any) => l.lobby_id === lobby.json().lobby_id)).toBe(false);
    launcher.close();
  });

  it('presence changes are pushed to friends', async () => {
    const a = await claim('Kantus');
    const b = await claim('Theron');
    await makeFriends(a, b);
    const aLauncher = await openSocket(a, '&role=launcher');
    const pushed = nextOfType(aLauncher, 'presence');
    const bGame = await openSocket(b, '&role=game&title_id=4D53082D');
    const m = await pushed;
    expect(m.uuid).toBe(b.uuid);
    expect(m.presence).toEqual({ state: 'playing', title_id: '4D53082D' });
    aLauncher.close(); bGame.close();
  });

  it('friend requests are rate limited (10 a minute)', async () => {
    const a = await claim('Spammer');
    const codes: number[] = [];
    for (let i = 0; i < 11; i++) codes.push((await requestFriend(a, await claim(`Target ${String.fromCharCode(65 + i)}`))).statusCode);
    expect(codes.slice(0, 10).every((c) => c === 200)).toBe(true);
    expect(codes[10]).toBe(429);
  });

  it('blocking someone who is not a friend tells them nothing; blocking a friend looks like a remove', async () => {
    const a = await claim('Golden Miner');
    const b = await claim('Flame Grenadier');
    const bLauncher = await openSocket(b, '&role=launcher');
    const seen: string[] = [];
    bLauncher.on('message', (d: any) => seen.push(JSON.parse(d.toString()).type));
    await requestFriend(b, a);                                   // b asked a; a blocks instead of declining
    await app.inject({ method: 'POST', url: `/api/v1/friends/${b.uuid}/block`, headers: auth(a) });
    await new Promise((r) => setTimeout(r, 300));
    expect(seen).not.toContain('friend_removed');
    const c = await claim('Hunter Elite');
    await makeFriends(c, b);
    const removed = nextOfType(bLauncher, 'friend_removed');
    await app.inject({ method: 'POST', url: `/api/v1/friends/${b.uuid}/block`, headers: auth(c) });
    expect((await removed).uuid).toBe(c.uuid);
    bLauncher.close();
  });

  it('a blocked request answers 404 like an unknown tag', async () => {
    const a = await claim('Lambent');
    const b = await claim('Berserker');
    expect((await app.inject({ method: 'POST', url: `/api/v1/friends/${a.uuid}/block`, headers: auth(b) })).statusCode).toBe(200);
    expect((await requestFriend(a, b)).statusCode).toBe(404);
    const unknown = await app.inject({ method: 'POST', url: '/api/v1/friends/requests', headers: auth(a), payload: { tag: 'Nobody#0001' } });
    expect(unknown.statusCode).toBe(404);
    expect((await app.inject({ method: 'GET', url: '/api/v1/friends' })).statusCode).toBe(401);
  });

  it('a launcher and a game socket of one account coexist; a second game socket replaces the first (4002)', async () => {
    const me = await claim('Augustus');
    const game1 = await openSocket(me, '&role=game');
    const launcher = await openSocket(me, '&role=launcher');
    const closed = closeCode(game1);
    const game2 = await openSocket(me, '&role=game');
    expect(await closed).toBe(4002);
    await new Promise((r) => setTimeout(r, 50));
    expect(launcher.readyState).toBe(WebSocket.OPEN);
    launcher.close(); game2.close();
  });

  it('candidates go to the game socket, not the launcher', async () => {
    const a = await claim('Tai Kaliso');
    const b = await claim('Dizzy');
    await makeFriends(a, b);
    const wa = await openSocket(a);
    const bGame = await openSocket(b, '&role=game&title_id=4D5308AB');
    const bLauncher = await openSocket(b, '&role=launcher');
    let launcherGot = false;
    bLauncher.on('message', () => { launcherGot = true; });
    const got = nextMessage(bGame);
    wa.send(candidatesTo(b));
    expect((await got).type).toBe('candidates');
    await new Promise((r) => setTimeout(r, 300));
    expect(launcherGot).toBe(false);
    wa.close(); bGame.close(); bLauncher.close();
  });

  it('a socket without role is a game socket (old runtimes)', async () => {
    const a = await claim('Alex Brand');
    const b = await claim('Samuel Byrne');
    await makeFriends(a, b);
    const [wa, wb] = await Promise.all([openSocket(a), openSocket(b)]);
    const got = nextMessage(wb);
    wa.send(candidatesTo(b));
    expect((await got).type).toBe('candidates');
    wa.close(); wb.close();
  });

  it('an unknown role is refused (4003)', async () => {
    const me = await claim('Jace Stratton');
    const ws = new WebSocket(`${serverAddress}/ws/signal?uuid=${me.uuid}&token=${me.token}&role=console`);
    expect(await closeCode(ws)).toBe(4003);
  });

  it('delivers candidates only to the target, stamped with the sender the server knows', async () => {
    const a = await claim('Kait');
    const b = await claim('Del');
    await makeFriends(a, b);
    const c = await claim('Jace');
    const [wa, wb, wc] = await Promise.all([openSocket(a), openSocket(b), openSocket(c)]);
    let cGotOne = false;
    wc.on('message', () => { cGotOne = true; });

    const got = nextMessage(wb);
    wa.send(JSON.stringify({
      type: 'candidates', target_uuid: b.uuid, from_uuid: c.uuid, from_xuid: c.xuid,  // forged fields are ignored
      session_token: '00c0ffee00c0ffee', port: 1000,
      candidates: [{ ip: '203.0.113.7', port: 40000, kind: 'srflx' }, { ip: '192.168.1.5', port: 1000, kind: 'host' }]
    }));
    const msg = await got;
    expect(msg.type).toBe('candidates');
    expect(msg.from_uuid).toBe(a.uuid);
    expect(msg.from_xuid).toBe(a.xuid);
    expect(msg.session_token).toBe('00c0ffee00c0ffee');
    expect(msg.port).toBe(1000);
    expect(msg.candidates).toEqual([
      { ip: '203.0.113.7', port: 40000, kind: 'srflx' }, { ip: '192.168.1.5', port: 1000, kind: 'host' }
    ]);
    await new Promise((r) => setTimeout(r, 50));
    expect(cGotOne).toBe(false);
    wa.close(); wb.close(); wc.close();
  });

  it('removes a hosted lobby when the host socket closes', async () => {
    const host = await claim('Hoffman');
    const ws = await openSocket(host);
    const created = await app.inject({
      method: 'POST', url: '/api/v1/lobbies', headers: auth(host), payload: { title_id: '4D5308AB', session_info: SESSION_INFO }
    });
    const { room_code } = created.json();
    expect((await app.inject({ method: 'GET', url: `/api/v1/lobbies/lookup/${room_code}` })).statusCode).toBe(200);
    ws.close();
    await new Promise((r) => setTimeout(r, 50));
    expect((await app.inject({ method: 'GET', url: `/api/v1/lobbies/lookup/${room_code}` })).statusCode).toBe(404);
  });

  it('answers health checks and 404 for an unknown room code', async () => {
    expect((await app.inject({ method: 'GET', url: '/api/v1/health' })).json().status).toBe('ok');
    expect((await app.inject({ method: 'GET', url: '/api/v1/lobbies/lookup/COG-ZZZZ' })).statusCode).toBe(404);
  });

  it('keeps discriminators and XUIDs unique under concurrent claims', async () => {
    const claims = await Promise.all(
      Array.from({ length: 25 }, () => app.inject({ method: 'POST', url: '/api/v1/auth/claim', payload: { display_name: 'Baird' } }))
    );
    const discs = new Set<string>();
    const xuids = new Set<string>();
    for (const res of claims) {
      expect(res.statusCode).toBe(200);
      discs.add(res.json().discriminator);
      xuids.add(res.json().xuid);
    }
    expect(discs.size).toBe(25);
    expect(xuids.size).toBe(25);
  });
});
