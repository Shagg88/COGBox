import WebSocket from 'ws';
import { DatabaseInstance } from '../db.js';
import { Registry } from './registry.js';
import { Presence } from './types.js';
import { JOINABLE_TITLES } from './joinable_titles.js';

// Presence from who is connected (social design step 2): nothing is stored.
export function presenceOf(db: DatabaseInstance, registry: Registry, uuid: string): Presence {
  const game = registry.game(uuid);
  if (game) {
    const p: Presence = { state: 'playing' };
    if (game.titleId) {
      p.title_id = game.titleId;
      // Public or friends-only alike: presence is only ever shown to friends. Only games confirmed for joins.
      const lobby = JOINABLE_TITLES.has(game.titleId)
        ? db.prepare('SELECT lobby_id, room_code, session_info, invite_only FROM lobbies WHERE host_uuid = ? AND title_id = ? ORDER BY created_at DESC, rowid DESC')
            .get(uuid, game.titleId) as { lobby_id: string; room_code: string; session_info: string; invite_only: number } | undefined
        : undefined;
      if (lobby && !lobby.invite_only) {
        p.state = 'joinable';
        p.lobby_id = lobby.lobby_id;
        // The game's own Join reads the session id from the friend record and looks that session up.
        p.session_id = lobby.session_info.slice(0, 16);
        p.room_code = lobby.room_code;
      }
    }
    return p;
  }
  return { state: registry.hasLauncher(uuid) || registry.recentlySeen(uuid) ? 'online' : 'offline' };
}

// An event to every open socket of an account (launcher and game).
export function notify(registry: Registry, uuid: string, event: object): void {
  const text = JSON.stringify(event);
  for (const s of registry.sockets(uuid)) {
    if (s.readyState === WebSocket.OPEN) s.send(text);
  }
}

// The account's presence to every accepted friend.
export function notifyPresence(db: DatabaseInstance, registry: Registry, uuid: string): void {
  const presence = presenceOf(db, registry, uuid);
  const friends = db.prepare("SELECT friend_uuid FROM friends WHERE user_uuid = ? AND status = 'accepted'")
    .all(uuid) as { friend_uuid: string }[];
  for (const f of friends) notify(registry, f.friend_uuid, { type: 'presence', uuid, presence });
}
