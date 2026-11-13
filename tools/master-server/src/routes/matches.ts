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

function playlistOf(json: string | null): number | null {
  try {
    const props = JSON.parse(json || '[]');
    const p = Array.isArray(props) ? props.find((e: any) => e && e.id === PLAYLIST_PROPERTY) : undefined;
    return p && Number.isInteger(p.value) ? p.value : null;
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

  fastify.get('/', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const rows = db.prepare(`SELECT lobby_id, host_uuid, title_id, region, max_players, current_players, properties_json,
                                    in_progress, created_at
                             FROM lobbies WHERE is_public = 1 AND host_uuid != ?
                             ORDER BY current_players DESC, created_at DESC LIMIT 200`).all(u.uuid) as any[];
    const matches = rows.filter((r) => OPEN_MATCH_TITLES.has(r.title_id) && hostPlaying(r)).slice(0, 50).map((r) => ({
      lobby_id: r.lobby_id,
      title_id: r.title_id,
      region: r.region,
      max_players: r.max_players,
      current_players: r.current_players,
      playlist: playlistOf(r.properties_json),
      in_progress: Boolean(r.in_progress)
    }));
    return { matches };
  });

  // Join from the list: the caller's game is told the room code (409 with it when that game is not running here: the
  // launcher then starts the game with --net_join_lobby), as Join friend does.
  fastify.post<{ Params: { lobby_id: string } }>('/:lobby_id/join', async (req, reply) => {
    const u = me(req, reply);
    if (!u) return reply;
    const row = db.prepare('SELECT host_uuid, title_id, room_code, is_public, max_players, current_players FROM lobbies WHERE lobby_id = ?')
      .get(req.params.lobby_id) as any;
    if (!row || !row.is_public || !OPEN_MATCH_TITLES.has(row.title_id) || !hostPlaying(row) || row.host_uuid === u.uuid) {
      return reply.status(404).send({ error: 'that match is no longer open' });
    }
    if (row.current_players >= row.max_players) return reply.status(403).send({ error: 'that match is full' });
    const lobby = { title_id: row.title_id as string, room_code: row.room_code as string };
    const hostXuid = (db.prepare('SELECT xuid FROM users WHERE uuid = ?').get(row.host_uuid) as { xuid: string }).xuid;
    if (!tellGameToJoin(registry, u.uuid, lobby, hostXuid, false)) {
      return reply.status(409).send({ error: 'start the game first', ...lobby });
    }
    return { status: 'joining', ...lobby };
  });
};
