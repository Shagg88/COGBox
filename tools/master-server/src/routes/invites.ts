import { FastifyPluginAsync, FastifyReply, FastifyRequest } from 'fastify';
import crypto from 'node:crypto';
import { DatabaseInstance } from '../db.js';
import { AuthedUser, checkAuthHeader } from '../auth.js';
import { Registry } from '../social/registry.js';
import { notify } from '../social/presence.js';
import { JOINABLE_TITLES } from '../social/joinable_titles.js';
import { FriendRef } from '../social/types.js';

// Game invites between friends (social step 4). Kept in memory for 10 minutes; declining tells the sender nothing.
export const INVITE_LIFETIME_MS = 10 * 60 * 1000;

interface Invite { id: string; from: string; to: string; title_id: string; room_code: string; created: number; met: boolean }

export const areFriends = (db: DatabaseInstance, a: string, b: string): boolean =>
  !!db.prepare("SELECT 1 FROM friends WHERE user_uuid = ? AND friend_uuid = ? AND status = 'accepted'").get(a, b);

// The lobby a host's friends may join: the newest one, in a game confirmed for joins. An Invite Only party takes
// invites but not Join friend.
export function joinableLobby(db: DatabaseInstance, host: string, invited: boolean): { title_id: string; room_code: string } | null {
  const row = db.prepare('SELECT title_id, room_code, invite_only FROM lobbies WHERE host_uuid = ? ORDER BY created_at DESC, rowid DESC')
    .get(host) as { title_id: string; room_code: string; invite_only: number } | undefined;
  return row && JOINABLE_TITLES.has(row.title_id) && (invited || !row.invite_only) ? row : null;
}

// Tells the joiner's game to join (it must run that title); false when it does not.
export function tellGameToJoin(registry: Registry, joiner: string, lobby: { title_id: string; room_code: string },
                               inviterXuid: string, fromGameInvite: boolean): boolean {
  const game = registry.game(joiner);
  if (!game || game.titleId !== lobby.title_id) return false;
  game.socket.send(JSON.stringify({ type: 'join', room_code: lobby.room_code, inviter_xuid: inviterXuid,
                                    from_game_invite: fromGameInvite }));
  return true;
}

export const inviteRoutes: FastifyPluginAsync<{ db: DatabaseInstance; registry: Registry; now: () => number }> =
  async (fastify, opts) => {
  const { db, registry, now } = opts;
  const invites = new Map<string, Invite>();

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
  const xuidOf = (uuid: string): string =>
    (db.prepare('SELECT xuid FROM users WHERE uuid = ?').get(uuid) as { xuid: string }).xuid;
  const live = (i: Invite | undefined): i is Invite => !!i && now() - i.created <= INVITE_LIFETIME_MS;
  const prune = () => { for (const [id, i] of invites) if (!live(i)) invites.delete(id); };
  const view = (i: Invite) => ({ id: i.id, from: refOf(i.from), title_id: i.title_id, room_code: i.room_code });

  fastify.post<{ Body: { to_uuid?: unknown; to_xuid?: unknown } }>('/', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const byXuid = typeof req.body?.to_xuid === 'string'
      ? db.prepare('SELECT uuid FROM users WHERE xuid = ?').get(req.body.to_xuid) as { uuid: string } | undefined : undefined;
    const to = byXuid?.uuid ?? (typeof req.body?.to_uuid === 'string' ? req.body.to_uuid : '');
    const lobby = joinableLobby(db, u.uuid, true);
    const friends = !!to && areFriends(db, u.uuid, to);
    // a player met in a match can be invited too, unless either has blocked the other
    const blocked = !!to && !!db.prepare("SELECT 1 FROM friends WHERE status = 'blocked' AND ((user_uuid = ? AND friend_uuid = ?) OR (user_uuid = ? AND friend_uuid = ?))").get(u.uuid, to, to, u.uuid);
    const met = !friends && !blocked && !!to && registry.inContact(u.uuid, to);
    if (!to || (!friends && !met) || !lobby) {
      return reply.status(404).send({ error: 'no friend to invite, or nothing hosted to invite to' });
    }
    prune();
    const inv: Invite = { id: crypto.randomUUID(), from: u.uuid, to, title_id: lobby.title_id, room_code: lobby.room_code,
                          created: now(), met };
    invites.set(inv.id, inv);
    notify(registry, to, { type: 'invite', ...view(inv) });
    return { id: inv.id };
  });

  fastify.get('/', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    prune();
    return { invites: [...invites.values()].filter((i) => i.to === u.uuid).map(view) };
  });

  fastify.post<{ Params: { id: string } }>('/:id/accept', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const inv = invites.get(req.params.id);
    // an invite from someone who is no longer a friend (removed or blocked since) is gone
    if (!live(inv) || inv.to !== u.uuid || (!inv.met && !areFriends(db, inv.from, u.uuid))) {
      if (inv) invites.delete(inv.id);
      return reply.status(404).send({ error: 'no such invite' });
    }
    invites.delete(inv.id);
    const lobby = { title_id: inv.title_id, room_code: inv.room_code };
    if (!tellGameToJoin(registry, u.uuid, lobby, xuidOf(inv.from), true)) {
      return reply.status(409).send({ error: 'start the game first', ...lobby });
    }
    return { status: 'joining', ...lobby };
  });

  fastify.post<{ Params: { id: string } }>('/:id/decline', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const inv = invites.get(req.params.id);
    if (inv && inv.to === u.uuid) invites.delete(inv.id);
    return { status: 'declined' };
  });
};
