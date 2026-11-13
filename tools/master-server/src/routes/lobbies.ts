import { FastifyPluginAsync } from 'fastify';
import crypto from 'node:crypto';
import { DatabaseInstance } from '../db.js';
import { checkAuthHeader } from '../auth.js';
import { normalizeRegion } from '../regions.js';

interface IdValue { id: number; value: number }

interface CreateLobbyBody {
  title_id: string;
  session_info: string;  // XSESSION_INFO as 120 hex characters (XNKID 8, XNADDR 36, XNKEY 16)
  region?: string;
  is_public?: boolean;
  max_players?: number;
  current_players?: number;
  properties?: IdValue[];
  contexts?: IdValue[];
}

interface LobbyQuery {
  title_id?: string;
  region?: string;
}

const ROOM_CODE_CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';

function generateRoomCode(db: DatabaseInstance): string {
  for (let attempt = 0; attempt < 50; attempt++) {
    let suffix = '';
    for (let i = 0; i < 4; i++) {
      suffix += ROOM_CODE_CHARS[crypto.randomInt(0, ROOM_CODE_CHARS.length)];
    }
    const code = `COG-${suffix}`;
    if (!db.prepare('SELECT 1 FROM lobbies WHERE lobbies.room_code = ?').get(code)) {
      return code;
    }
  }
  throw new Error('Unable to generate unique room code');
}

function idValues(v: unknown): IdValue[] | null {
  if (v === undefined) return [];
  if (!Array.isArray(v) || v.length > 64) return null;
  for (const e of v) {
    if (typeof e !== 'object' || e === null || !Number.isInteger(e.id) || typeof e.value !== 'number') return null;
  }
  return v.map((e: any) => ({ id: e.id, value: e.value }));
}

function parseList(json: string | null): IdValue[] {
  try {
    const v = JSON.parse(json || '[]');
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
  }
}

function toLobby(row: any) {
  return {
    lobby_id: row.lobby_id,
    host_uuid: row.host_uuid,
    host_xuid: row.host_xuid,
    host_name: row.host_name ?? '',  // XAM adds it to search results as X_PROPERTY_GAMER_HOSTNAME
    title_id: row.title_id,
    room_code: row.room_code,
    session_info: row.session_info,
    region: row.region,
    is_public: Boolean(row.is_public),
    max_players: row.max_players,
    current_players: row.current_players,
    properties: parseList(row.properties_json),
    contexts: parseList(row.contexts_json),
    qos_data: row.qos_data ?? '',
    created_at: row.created_at,
    updated_at: row.updated_at
  };
}

export const lobbyRoutes: FastifyPluginAsync<{ db: DatabaseInstance; onHostChange?: (uuid: string) => void; inContact?: (a: string, b: string) => boolean;
                                             searchRequiresSignIn?: boolean }> =
  async (fastify, opts) => {
  const { db } = opts;
  const hostChanged = (uuid: string) => opts.onHostChange?.(uuid);
  const searches = new Map<string, { until: number; body: string }>();
  fastify.addHook('onResponse', async (request) => {
    if (request.method !== 'GET') searches.clear();  // a host's own change shows straight away
  });

  // Public lobbies of a title (and region).
  fastify.get<{ Querystring: LobbyQuery }>('/', async (request, reply) => {
    if (opts.searchRequiresSignIn && !checkAuthHeader(db, request.headers.authorization)) {
      return reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    }
    const { title_id } = request.query;
    const region = normalizeRegion(request.query.region);
    // Players searching ask every few seconds: one answer per second and per search is enough.
    const key = `${title_id ?? ''}|${region ?? ''}`;
    const cached = searches.get(key);
    if (cached && cached.until > Date.now()) return reply.type('application/json').send(cached.body);
    let query = 'SELECT lobbies.*, users.display_name AS host_name FROM lobbies LEFT JOIN users ON users.uuid = lobbies.host_uuid WHERE lobbies.is_public = 1';
    const params: any[] = [];
    if (title_id) {
      query += ' AND lobbies.title_id = ?';
      params.push(title_id.toUpperCase());
    }
    if (region) {
      query += ' AND lobbies.region = ?';
      params.push(region);
    }
    query += ' ORDER BY lobbies.updated_at DESC LIMIT 50';
    const rows = db.prepare(query).all(...params) as any[];
    // no room code here: a search never needs it (a found lobby is joined by its session info), and it would let
    // anyone list every public lobby's join code. The lookup by code (below) still answers with it.
    const body = JSON.stringify({ lobbies: rows.map((r) => { const { room_code: _, ...l } = toLobby(r); return l; }) });
    if (searches.size >= 1000) searches.clear();
    searches.set(key, { until: Date.now() + 1000, body });
    return reply.type('application/json').send(body);
  });

  // A lobby by its room code (COG-XXXX, any case).
  fastify.get<{ Params: { room_code: string } }>('/lookup/:room_code', async (request, reply) => {
    const code = (request.params.room_code || '').trim().toUpperCase();
    const row = db.prepare('SELECT lobbies.*, users.display_name AS host_name FROM lobbies LEFT JOIN users ON users.uuid = lobbies.host_uuid WHERE lobbies.room_code = ?').get(code) as any;
    if (!row) {
      return reply.status(404).send({ error: 'Lobby not found' });
    }
    if (!row.is_public) {   // friends-only: the host and the host's friends, and nobody else learns it exists
      const asker = checkAuthHeader(db, request.headers.authorization);
      const allowed = asker && (asker.uuid === row.host_uuid || opts.inContact?.(asker.uuid, row.host_uuid) ||
        db.prepare("SELECT 1 FROM friends WHERE user_uuid = ? AND friend_uuid = ? AND status = 'accepted'")
          .get(row.host_uuid, asker.uuid));
      if (!allowed) return reply.status(404).send({ error: 'Lobby not found' });
    }
    return reply.send({ lobby: toLobby(row) });
  });

  // Register a lobby; the host is whoever the Authorization header proves.
  fastify.post<{ Body: CreateLobbyBody }>('/', async (request, reply) => {
    const host = checkAuthHeader(db, request.headers.authorization);
    if (!host) {
      return reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    }
    const body = request.body || ({} as CreateLobbyBody);
    const properties = idValues(body.properties);
    const contexts = idValues(body.contexts);
    if (typeof body.title_id !== 'string' || !/^[0-9A-Fa-f]{8}$/.test(body.title_id) ||
        typeof body.session_info !== 'string' || !/^[0-9A-Fa-f]{120}$/.test(body.session_info) ||
        !properties || !contexts) {
      return reply.status(400).send({ error: 'title_id (8 hex), session_info (120 hex), properties, contexts' });
    }
    // One listed lobby per host and title: a game that registers a new match session without deleting the old one
    // (seen in the two-PC test) would otherwise leave a dead lobby that searches find first.
    // a host's party and its match are separate lobbies: only one of the same kind is replaced
    db.prepare('DELETE FROM lobbies WHERE host_uuid = ? AND title_id = ? AND is_public = ?').run(host.uuid, body.title_id.toUpperCase(), body.is_public === false ? 0 : 1);
    const lobbyId = crypto.randomUUID();
    const roomCode = generateRoomCode(db);
    const now = Date.now();
    db.prepare(`
      INSERT INTO lobbies (
        lobby_id, host_uuid, host_xuid, title_id, room_code, session_info, region,
        is_public, max_players, current_players, properties_json, contexts_json, created_at, updated_at
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    `).run(
      lobbyId, host.uuid, host.xuid, body.title_id.toUpperCase(), roomCode, body.session_info.toLowerCase(),
      normalizeRegion(body.region) ?? 'global', body.is_public === false ? 0 : 1, body.max_players ?? 8, body.current_players ?? 1,
      JSON.stringify(properties), JSON.stringify(contexts), now, now
    );
    hostChanged(host.uuid);
    return reply.status(200).send({ lobby_id: lobbyId, room_code: roomCode });
  });

  // The host's QoS block (what XNetQosListen published: Gears puts the host's name there), up to 1 KB as hex.
  fastify.patch<{ Params: { lobby_id: string }; Body: any }>('/:lobby_id', async (request, reply) => {
    const user = checkAuthHeader(db, request.headers.authorization);
    if (!user) {
      return reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    }
    const body = (request.body || {}) as { qos_data?: unknown; properties?: unknown; contexts?: unknown; invite_only?: unknown; current_players?: unknown; in_progress?: unknown };
    const qos = body.qos_data;
    if (qos !== undefined &&
        (typeof qos !== 'string' || qos.length > 2048 || qos.length % 2 !== 0 || !/^[0-9A-Fa-f]*$/.test(qos))) {
      return reply.status(400).send({ error: 'qos_data: hex, at most 1024 bytes' });
    }
    const inviteOnly = body.invite_only;
    if (inviteOnly !== undefined && typeof inviteOnly !== 'boolean') {
      return reply.status(400).send({ error: 'invite_only: true or false' });
    }
    const inProgress = body.in_progress;
    if (inProgress !== undefined && typeof inProgress !== 'boolean') {
      return reply.status(400).send({ error: 'in_progress: true or false' });
    }
    const players = body.current_players;
    if (players !== undefined && (!Number.isInteger(players) || (players as number) < 1 || (players as number) > 64)) {
      return reply.status(400).send({ error: 'current_players: 1 to 64' });
    }
    const properties = body.properties === undefined ? undefined : idValues(body.properties);
    const contexts = body.contexts === undefined ? undefined : idValues(body.contexts);
    if (properties === null || contexts === null ||
        (qos === undefined && properties === undefined && contexts === undefined && inviteOnly === undefined && players === undefined && inProgress === undefined)) {
      return reply.status(400).send({ error: 'qos_data (hex) and/or properties / contexts ([{ id, value }])' });
    }
    const row = db.prepare('SELECT host_uuid FROM lobbies WHERE lobby_id = ?').get(request.params.lobby_id) as any;
    if (!row) {
      return reply.status(404).send({ error: 'Lobby not found' });
    }
    if (row.host_uuid !== user.uuid) {
      return reply.status(403).send({ error: 'not your lobby' });
    }
    const now = Date.now();
    const id = request.params.lobby_id;
    if (typeof players === 'number') {
      db.prepare('UPDATE lobbies SET current_players = ?, updated_at = ? WHERE lobby_id = ?').run(players, now, id);
    }
    if (typeof inProgress === 'boolean') {
      db.prepare('UPDATE lobbies SET in_progress = ?, updated_at = ? WHERE lobby_id = ?').run(inProgress ? 1 : 0, now, id);
    }
    if (typeof inviteOnly === 'boolean') {
      db.prepare('UPDATE lobbies SET invite_only = ?, updated_at = ? WHERE lobby_id = ?').run(inviteOnly ? 1 : 0, now, id);
      hostChanged(user.uuid);
    }
    if (typeof qos === 'string') {
      db.prepare('UPDATE lobbies SET qos_data = ?, updated_at = ? WHERE lobby_id = ?').run(qos.toLowerCase(), now, id);
    }
    if (properties) {
      db.prepare('UPDATE lobbies SET properties_json = ?, updated_at = ? WHERE lobby_id = ?')
        .run(JSON.stringify(properties), now, id);
    }
    if (contexts) {
      db.prepare('UPDATE lobbies SET contexts_json = ?, updated_at = ? WHERE lobby_id = ?')
        .run(JSON.stringify(contexts), now, id);
    }
    return reply.status(200).send({ success: true });
  });

  // Remove a lobby: only its host.
  fastify.delete<{ Params: { lobby_id: string } }>('/:lobby_id', async (request, reply) => {
    const user = checkAuthHeader(db, request.headers.authorization);
    if (!user) {
      return reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    }
    const row = db.prepare('SELECT host_uuid FROM lobbies WHERE lobby_id = ?').get(request.params.lobby_id) as any;
    if (row && row.host_uuid !== user.uuid) {
      return reply.status(403).send({ error: 'not your lobby' });
    }
    db.prepare('DELETE FROM lobbies WHERE lobby_id = ?').run(request.params.lobby_id);
    hostChanged(user.uuid);
    return reply.status(200).send({ success: true });
  });
};
