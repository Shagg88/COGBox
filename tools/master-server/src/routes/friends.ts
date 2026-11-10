import { FastifyPluginAsync, FastifyReply, FastifyRequest } from 'fastify';
import { DatabaseInstance } from '../db.js';
import { AuthedUser, checkAuthHeader } from '../auth.js';
import { Registry } from '../social/registry.js';
import { notify, notifyPresence, presenceOf } from '../social/presence.js';
import { accept, block, decline, findByTag, listFor, remove, request } from '../social/friends.js';
import { Friend, FriendRef } from '../social/types.js';
import { joinableLobby, tellGameToJoin } from './invites.js';

// Friends API (social design step 2). Every call needs `Authorization: COGBox <uuid>:<token>`.
export const REQUESTS_PER_MINUTE = 10;

export const friendRoutes: FastifyPluginAsync<{ db: DatabaseInstance; registry: Registry }> = async (fastify, opts) => {
  const { db, registry } = opts;
  const sent = new Map<string, number[]>();   // uuid -> request times in the last minute

  const me = (req: FastifyRequest, reply: FastifyReply): AuthedUser | null => {
    const u = checkAuthHeader(db, req.headers.authorization);
    if (!u) reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    return u;
  };
  const refOf = (uuid: string): FriendRef => {
    const r = db.prepare('SELECT display_name, discriminator, icon FROM users WHERE uuid = ?').get(uuid) as
      { display_name: string; discriminator: string; icon: string };
    return { uuid, tag: `${r.display_name}#${r.discriminator}`, icon: r.icon };
  };
  const friendOf = (uuid: string): Friend => ({
    ...refOf(uuid),
    xuid: (db.prepare('SELECT xuid FROM users WHERE uuid = ?').get(uuid) as { xuid: string }).xuid,
    presence: presenceOf(db, registry, uuid),
  });
  const exists = (uuid: string) => !!db.prepare('SELECT 1 FROM users WHERE uuid = ?').get(uuid);
  const becameFriends = (a: string, b: string) => {
    notify(registry, a, { type: 'friend_added', friend: friendOf(b) });
    notify(registry, b, { type: 'friend_added', friend: friendOf(a) });
  };

  fastify.get('/', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const l = listFor(db, u.uuid);
    return { friends: l.friends.map((f) => friendOf(f.uuid)), incoming: l.incoming.map((r) => refOf(r.uuid)),
      outgoing: l.outgoing.map((r) => refOf(r.uuid)) };
  });

  fastify.post<{ Body: { tag?: unknown } }>('/requests', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const now = Date.now();
    const recent = (sent.get(u.uuid) ?? []).filter((t) => now - t < 60_000);
    if (recent.length >= REQUESTS_PER_MINUTE) {
      sent.set(u.uuid, recent);
      return reply.status(429).send({ error: `at most ${REQUESTS_PER_MINUTE} friend requests a minute` });
    }
    recent.push(now);
    sent.set(u.uuid, recent);
    if (typeof req.body?.tag !== 'string') return reply.status(400).send({ error: 'tag: Name#1234' });
    const target = findByTag(db, req.body.tag);
    if (!target) return reply.status(404).send({ error: 'no player with that tag' });
    const result = request(db, u.uuid, target.uuid);
    switch (result) {
      case 'self': return reply.status(400).send({ error: 'that is you' });
      case 'not_found': return reply.status(404).send({ error: 'no player with that tag' });
      case 'limit': return reply.status(409).send({ error: 'friend or request limit reached' });
      case 'pending':
        notify(registry, target.uuid, { type: 'friend_request', from: refOf(u.uuid) });
        return { status: 'pending' };
      case 'accepted':
        becameFriends(u.uuid, target.uuid);
        return { status: 'accepted' };
    }
  });

  // The launcher is closing or left online play: offline at once, not after the 60 s without a poll.
  fastify.post('/offline', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    registry.forgetSeen(u.uuid);
    notifyPresence(db, registry, u.uuid);
    return { status: 'offline' };
  });

  fastify.post<{ Params: { uuid: string } }>('/:uuid/accept', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    if (!accept(db, u.uuid, req.params.uuid)) return reply.status(404).send({ error: 'no request from that player' });
    becameFriends(u.uuid, req.params.uuid);
    return { status: 'accepted' };
  });

  fastify.post<{ Params: { uuid: string } }>('/:uuid/decline', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    decline(db, u.uuid, req.params.uuid);
    return { status: 'declined' };
  });

  fastify.post<{ Params: { uuid: string } }>('/:uuid/block', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    if (!exists(req.params.uuid) || req.params.uuid === u.uuid) return reply.status(404).send({ error: 'no such player' });
    // A friend sees the same as a remove; anyone else is told nothing (a block is never revealed).
    const wereFriends = listFor(db, u.uuid).friends.some((f) => f.uuid === req.params.uuid);
    block(db, u.uuid, req.params.uuid);
    if (wereFriends) notify(registry, req.params.uuid, { type: 'friend_removed', uuid: u.uuid });
    return { status: 'blocked' };
  });

  fastify.delete<{ Params: { uuid: string } }>('/:uuid', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    remove(db, u.uuid, req.params.uuid);
    notify(registry, req.params.uuid, { type: 'friend_removed', uuid: u.uuid });
    return { status: 'removed' };
  });

  // Join friend: the caller's game is told the room code (409 with it when that game is not running: the launcher
  // then starts the game with --net_join_lobby).
  fastify.post<{ Params: { uuid: string } }>('/:uuid/join', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const friends = listFor(db, u.uuid).friends.some((f) => f.uuid === req.params.uuid);
    const lobby = friends ? joinableLobby(db, req.params.uuid, false) : null;
    if (!lobby) return reply.status(404).send({ error: 'nothing to join' });
    const hostXuid = (db.prepare('SELECT xuid FROM users WHERE uuid = ?').get(req.params.uuid) as { xuid: string }).xuid;
    if (!tellGameToJoin(registry, u.uuid, lobby, hostXuid, false)) {
      return reply.status(409).send({ error: 'start the game first', ...lobby });
    }
    return { status: 'joining', ...lobby };
  });

  // Presence follows the connections.
  registry.onChange((uuid) => notifyPresence(db, registry, uuid));
};
