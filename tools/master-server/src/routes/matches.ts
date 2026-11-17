import { FastifyPluginAsync, FastifyReply, FastifyRequest } from 'fastify';
import { DatabaseInstance } from '../db.js';
import { checkAuthHeader, AuthedUser } from '../auth.js';
import { Registry } from '../social/registry.js';
import { tellGameToJoin } from './invites.js';

// Open matches (the Social tab's list): public match lobbies of the games with join in progress, while the host's game
// is running that title. Parties and private matches are never public (the runtime lists only matchmaking sessions
// with stats as public). The answer names nobody: no host name, uuid, xuid, room code or address.
export const OPEN_MATCH_TITLES = new Set(['4D53082D', '4D5308AB', '4D530A26']);   // Gears 2, Gears 3, Judgment
const PLAYLIST_PROPERTY = 0x1000000b;   // the game's playlist id (Gears 2 / 3 / Judgment: GearPlaylist.ini)
// Gears 3: Join through the game's own matchmaking when the launcher asks (?via=matchmaking): the game is told to find
// this match with its Versus > Social search, so the host's game seats the joiner as its own party and picks the team,
// as strangers met on Xbox Live (an invite would make them a member of the host's party, on the host's team).
// Runtime: COGBox SDK patch 112 ({type:'prefer'}). Friends' joins and invites keep the invite.
export const MATCHMAKING_JOIN_TITLES = new Set(['4D5308AB']);   // Gears 3

function playlistOf(json: string | null): number | null {
  try {
    const props = JSON.parse(json || '[]');
    const p = Array.isArray(props) ? props.find((e: any) => e && e.id === PLAYLIST_PROPERTY) : undefined;
    return p && Number.isInteger(p.value) ? p.value : null;
  } catch {
    return null;
  }
}

// 0.6.5 DLC light: the host's map, its MapName context (id 1); the launcher checks the player has that map's package.
const MAP_CONTEXT = 1;
function mapOf(json: string | null): number | null {
  try {
    const ctx = JSON.parse(json || '[]');
    const c = Array.isArray(ctx) ? ctx.find((e: any) => e && e.id === MAP_CONTEXT) : undefined;
    return c && Number.isInteger(c.value) && c.value > 0 ? c.value : null;
  } catch {
    return null;
  }
}

export const matchRoutes: FastifyPluginAsync<{ db: DatabaseInstance; registry: Registry }> = async (fastify, opts) => {
  const { db, registry } = opts;
  const me = (req: FastifyRequest, reply: FastifyReply): AuthedUser | null => {
    const u = checkAuthHeader(db, req.headers.authorization);
    if (!u) reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    return u;
  };
  const hostPlaying = (row: { host_uuid: string; title_id: string }) => registry.game(row.host_uuid)?.titleId === row.title_id;
  // a block either way: neither sees nor joins the other's match (as with invites)
  const BLOCKED = `EXISTS (SELECT 1 FROM friends WHERE status = 'blocked' AND
                     ((user_uuid = lobbies.host_uuid AND friend_uuid = @me) OR (user_uuid = @me AND friend_uuid = lobbies.host_uuid)))`;
  const TITLES = [...OPEN_MATCH_TITLES].map((t) => `'${t}'`).join(', ');

  fastify.get('/', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const rows = db.prepare(`SELECT lobby_id, host_uuid, title_id, region, max_players, current_players, properties_json,
                                    contexts_json, in_progress, created_at
                             FROM lobbies WHERE is_public = 1 AND host_uuid != @me AND title_id IN (${TITLES}) AND NOT ${BLOCKED}
                             ORDER BY current_players DESC, created_at DESC LIMIT 200`).all({ me: u.uuid }) as any[];
    const matches = rows.filter((r) => hostPlaying(r)).slice(0, 50).map((r) => ({
      lobby_id: r.lobby_id,
      title_id: r.title_id,
      region: r.region,
      max_players: r.max_players,
      current_players: r.current_players,
      playlist: playlistOf(r.properties_json),
      map: mapOf(r.contexts_json),
      in_progress: Boolean(r.in_progress)
    }));
    return { matches };
  });

  // Join from the list: the caller's game is told the room code (409 with it when that game is not running here: the
  // launcher then starts the game with --net_join_lobby), as Join friend does. With ?via=matchmaking (Gears 3) the game
  // is instead told to find the match through matchmaking: 200 {status:'search', playlist}, or 409 with via and the
  // playlist so the launcher starts it with --net_prefer_lobby.
  fastify.post<{ Params: { lobby_id: string }; Querystring: { via?: string } }>('/:lobby_id/join', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const row = db.prepare(`SELECT host_uuid, title_id, room_code, is_public, max_players, current_players, properties_json FROM lobbies
                            WHERE lobby_id = @id AND NOT ${BLOCKED}`).get({ id: req.params.lobby_id, me: u.uuid }) as any;
    if (!row || !row.is_public || !OPEN_MATCH_TITLES.has(row.title_id) || !hostPlaying(row) || row.host_uuid === u.uuid) {
      return reply.status(404).send({ error: 'that match is no longer open' });
    }
    if (row.current_players >= row.max_players) return reply.status(403).send({ error: 'that match is full' });
    const lobby = { title_id: row.title_id as string, room_code: row.room_code as string };
    if (req.query?.via === 'matchmaking' && MATCHMAKING_JOIN_TITLES.has(row.title_id)) {
      const answer = { ...lobby, via: 'matchmaking', playlist: playlistOf(row.properties_json) };
      const game = registry.game(u.uuid);
      if (!game || game.titleId !== row.title_id) return reply.status(409).send({ error: 'start the game first', ...answer });
      game.socket.send(JSON.stringify({ type: 'prefer', room_code: row.room_code, playlist: answer.playlist }));
      return { status: 'search', ...answer };
    }
    const hostXuid = (db.prepare('SELECT xuid FROM users WHERE uuid = ?').get(row.host_uuid) as { xuid: string }).xuid;
    if (!tellGameToJoin(registry, u.uuid, lobby, hostXuid, false)) {
      return reply.status(409).send({ error: 'start the game first', ...lobby });
    }
    return { status: 'joining', ...lobby };
  });
};
