import { FastifyPluginAsync } from 'fastify';
import WebSocket from 'ws';
import { DatabaseInstance } from '../db.js';
import { AuthedUser, checkToken } from '../auth.js';
import { Registry, Role } from '../social/registry.js';

interface SignalingPluginOptions {
  db: DatabaseInstance;
  registry?: Registry;
}

interface Candidate { ip: string; port: number; kind: 'host' | 'srflx' | 'upnp' }

function cleanCandidates(v: unknown): Candidate[] | null {
  if (!Array.isArray(v) || v.length > 16) return null;
  const out: Candidate[] = [];
  for (const c of v) {
    if (typeof c !== 'object' || c === null || typeof c.ip !== 'string' || !/^\d{1,3}(\.\d{1,3}){3}$/.test(c.ip) ||
        !Number.isInteger(c.port) || c.port < 1 || c.port > 65535 || !['host', 'srflx', 'upnp'].includes(c.kind)) {
      return null;
    }
    out.push({ ip: c.ip, port: c.port, kind: c.kind });
  }
  return out;
}

// Who may send connection details to whom. Without this any account could make any player's game answer with its
// addresses and open a path to it. Allowed: friends; anyone to a host of a public lobby; two players already in
// contact (the host answering its guest); two players in contact with the same player (guests of one match).
function mayConnect(db: DatabaseInstance, registry: Registry, from: string, to: string): boolean {
  if (from === to) return false;
  if (registry.inContact(from, to)) return true;
  if (db.prepare("SELECT 1 FROM friends WHERE user_uuid = ? AND friend_uuid = ? AND status = 'accepted'").get(from, to)) return true;
  return !!db.prepare('SELECT 1 FROM lobbies WHERE host_uuid = ? AND is_public = 1').get(to);
}

// /ws/signal?uuid=<uuid>&token=<token>[&role=game|launcher][&title_id=<8 hex>]. No role = a game (older runtimes).
// The server stamps every relayed message with the sender it authenticated,
// so a client cannot speak for another uuid. It never carries gameplay.
export const signalingRoutes: FastifyPluginAsync<SignalingPluginOptions> = async (fastify, opts) => {
  const { db } = opts;
  const registry = opts.registry ?? new Registry();

  fastify.get<{ Querystring: { uuid?: string; token?: string; role?: string; title_id?: string; region?: string } }>(
    '/signal', { websocket: true }, (socket, req) => {
    const user: AuthedUser | null = checkToken(db, req.query.uuid, req.query.token);
    if (!user) {
      socket.close(4001, 'uuid and token required');
      return;
    }
    const roleText = req.query.role ?? 'game';
    if (roleText !== 'game' && roleText !== 'launcher') {
      socket.close(4003, 'role must be game or launcher');
      return;
    }
    const role: Role = roleText;
    const titleId = role === 'game' && typeof req.query.title_id === 'string' && /^[0-9A-Fa-f]{8}$/.test(req.query.title_id)
      ? req.query.title_id.toUpperCase() : undefined;
    const region = role === 'game' && typeof req.query.region === 'string' && /^[a-z-]{1,16}$/.test(req.query.region) ? req.query.region : undefined;
    const previous = registry.add(user.uuid, role, socket, titleId, region);
    if (previous) {
      previous.close(4002, 'signed in elsewhere');
    }
    db.prepare('UPDATE users SET last_seen = ? WHERE uuid = ?').run(Date.now(), user.uuid);

    // Candidates and invites are for the target's game, and only between players who have a reason to connect.
    const relay = (target_uuid: unknown, message: object, errorType: string) => {
      const target = typeof target_uuid === 'string' && mayConnect(db, registry, user.uuid, target_uuid)
        ? registry.game(target_uuid)?.socket : undefined;
      if (target && target.readyState === WebSocket.OPEN) {
        registry.addContact(user.uuid, target_uuid as string);
        target.send(JSON.stringify({ ...message, from_uuid: user.uuid, from_xuid: user.xuid }));
      } else {
        socket.send(JSON.stringify({ type: errorType, target_uuid, error: 'Target peer is offline or not found' }));
      }
    };

    socket.on('message', (raw) => {
      let data: any;
      try {
        data = JSON.parse(raw.toString());
      } catch {
        socket.send(JSON.stringify({ type: 'error', error: 'Invalid JSON message' }));
        return;
      }

      switch (data?.type) {
        case 'candidates': {
          const candidates = cleanCandidates(data.candidates);
          if (!candidates || typeof data.session_token !== 'string' || !/^[0-9a-f]{1,16}$/.test(data.session_token) ||
              !Number.isInteger(data.port) || data.port < 1 || data.port > 65535) {
            socket.send(JSON.stringify({ type: 'candidates_error', error: 'session_token (hex), port, candidates' }));
            return;
          }
          // another guest of the match is known to the game only by XUID; mayConnect still decides
          const byXuid = typeof data.target_xuid === 'string'
            ? db.prepare('SELECT uuid FROM users WHERE xuid = ?').get(data.target_xuid) as { uuid: string } | undefined : undefined;
          relay(data.target_uuid ?? byXuid?.uuid, {
            type: 'candidates', session_token: data.session_token, port: data.port, candidates,
            ...(typeof data.launch_id === 'string' && /^[0-9a-f]{1,16}$/.test(data.launch_id) ? { launch_id: data.launch_id } : {})
          }, 'candidates_error');
          return;
        }
        case 'invite':
          relay(data.target_uuid, { type: 'invite', room_code: data.room_code, title_id: data.title_id }, 'invite_error');
          return;
        case 'presence':
          db.prepare('UPDATE users SET last_seen = ? WHERE uuid = ?').run(Date.now(), user.uuid);
          socket.send(JSON.stringify({ type: 'presence_ack', title_id: data.title_id, status: data.status,
                                       timestamp: Date.now() }));
          return;
        case 'ping':
          socket.send(JSON.stringify({ type: 'pong', timestamp: Date.now() }));
          return;
        default:
          socket.send(JSON.stringify({ type: 'error', error: 'Unknown message type' }));
      }
    });

    const drop = () => {
      // A host whose game is gone has no lobby any more (the launcher closing changes nothing there).
      if (registry.remove(user.uuid, role, socket) && role === 'game') {
        db.prepare('DELETE FROM lobbies WHERE host_uuid = ?').run(user.uuid);
        registry.forgetContacts(user.uuid);
      }
    };
    socket.on('close', drop);
    socket.on('error', drop);
  });
};
