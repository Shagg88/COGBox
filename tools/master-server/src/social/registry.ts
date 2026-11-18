import WebSocket from 'ws';
import { normalizeRegion } from '../regions.js';

// Who is connected: per account at most one launcher socket and one game socket (the game socket also says which
// title it runs). A second socket of the same role replaces the first; different roles coexist.
export type Role = 'game' | 'launcher';

interface Entry { game?: { socket: WebSocket; titleId?: string; region?: string }; launcher?: WebSocket }

export class Registry {
  private entries = new Map<string, Entry>();
  // The launcher polls instead of keeping a socket (social step 3): an account seen by a signed-in request in the last
  // SEEN_MS counts as online.
  private seenAt = new Map<string, number>();
  static readonly SEEN_MS = 60_000;

  constructor(private readonly now: () => number = Date.now) {}

  seen(uuid: string): void {
    this.seenAt.set(uuid, this.now());
  }

  // The launcher closed or left online play: not "seen" any more.
  forgetSeen(uuid: string): void {
    this.seenAt.delete(uuid);
  }

  recentlySeen(uuid: string): boolean {
    const t = this.seenAt.get(uuid);
    return t !== undefined && this.now() - t < Registry.SEEN_MS;
  }
  private listeners: Array<(uuid: string) => void> = [];

  // Who has connected to whom, while their games run: a match's players, directly or through the same player.
  private contacts = new Map<string, Set<string>>();
  addContact(a: string, b: string): void {
    if (!this.contacts.has(a)) this.contacts.set(a, new Set());
    if (!this.contacts.has(b)) this.contacts.set(b, new Set());
    this.contacts.get(a)!.add(b);
    this.contacts.get(b)!.add(a);
  }
  forgetContacts(uuid: string): void {
    for (const other of this.contacts.get(uuid) ?? []) this.contacts.get(other)?.delete(uuid);
    this.contacts.delete(uuid);
  }
  inContact(a: string, b: string): boolean {
    const mine = this.contacts.get(a);
    return !!mine && (mine.has(b) || [...mine].some((x) => this.contacts.get(x)?.has(b)));
  }

  // GitHub #353: players this one's game exchanged connection details with (a match's other players), newest first,
  // for the Social "Recent players" list - so their tags can be added as friends. In memory only (a restart forgets
  // them), at most RECENT_MAX a player, for RECENT_MS.
  private met = new Map<string, Map<string, { at: number; titleId?: string }>>();
  static readonly RECENT_MS = 24 * 3600_000;
  static readonly RECENT_MAX = 30;
  // A meeting is noted only when both games have sent each other their connection details (review r353: one message to
  // a public host is not a match - anyone may send that). from>to -> time of the first direction, for HALF_MS.
  private half = new Map<string, number>();
  static readonly HALF_MS = 5 * 60_000;
  private notes = 0;
  noteRelay(from: string, to: string, titleId?: string): void {
    if (from === to) return;
    const now = this.now();
    const back = this.half.get(`${to}>${from}`);
    if (back !== undefined && now - back <= Registry.HALF_MS) {
      this.half.delete(`${to}>${from}`);
      this.noteMet(from, to, titleId);
    } else {
      this.half.set(`${from}>${to}`, now);
    }
    if (++this.notes % 1000 === 0) this.sweep();
  }
  // Expired halves and meetings go (review r353: players who never ask for their list would keep them until a restart).
  sweep(): void {
    const now = this.now();
    for (const [k, t] of this.half) if (now - t > Registry.HALF_MS) this.half.delete(k);
    for (const [who, m] of this.met) {
      for (const [other, e] of m) if (now - e.at > Registry.RECENT_MS) m.delete(other);
      if (!m.size) this.met.delete(who);
    }
  }
  // A deleted account leaves every list at once (PRIVACY.md); its uuid could otherwise be claimed again and see them.
  forgetMet(uuid: string): void {
    for (const other of this.met.get(uuid)?.keys() ?? []) {
      const m = this.met.get(other);
      m?.delete(uuid);
      if (m && !m.size) this.met.delete(other);
    }
    this.met.delete(uuid);
    for (const k of [...this.half.keys()]) if (k.startsWith(`${uuid}>`) || k.endsWith(`>${uuid}`)) this.half.delete(k);
  }
  noteMet(a: string, b: string, titleId?: string): void {
    if (a === b) return;
    const at = this.now();
    for (const [x, y] of [[a, b], [b, a]]) {
      let m = this.met.get(x);
      if (!m) { m = new Map(); this.met.set(x, m); }
      m.delete(y);   // re-inserted: a Map keeps insertion order, so the oldest is first
      m.set(y, { at, titleId });
      while (m.size > Registry.RECENT_MAX) m.delete(m.keys().next().value as string);
    }
  }
  recentFor(uuid: string): { uuid: string; at: number; titleId?: string }[] {
    const m = this.met.get(uuid);
    if (!m) return [];
    const now = this.now();
    const out: { uuid: string; at: number; titleId?: string }[] = [];
    for (const [other, e] of m) {
      if (now - e.at > Registry.RECENT_MS) m.delete(other);
      else out.push({ uuid: other, at: e.at, titleId: e.titleId });
    }
    if (!m.size) this.met.delete(uuid);
    return out.sort((x, y) => y.at - x.at);
  }

  // Returns the socket this one replaced, if any (the caller closes it).
  add(uuid: string, role: Role, socket: WebSocket, titleId?: string, region?: string): WebSocket | undefined {
    const e = this.entries.get(uuid) ?? {};
    let replaced: WebSocket | undefined;
    if (role === 'game') {
      replaced = e.game?.socket;
      e.game = { socket, titleId, region: normalizeRegion(region) };
    } else {
      replaced = e.launcher;
      e.launcher = socket;
    }
    this.entries.set(uuid, e);
    this.changed(uuid);
    return replaced !== socket ? replaced : undefined;
  }

  // Removes the socket if it is still the current one for that role (a replaced socket's late close is ignored).
  // Returns true when it was.
  remove(uuid: string, role: Role, socket: WebSocket): boolean {
    const e = this.entries.get(uuid);
    if (!e) return false;
    let removed = false;
    if (role === 'game' && e.game?.socket === socket) { e.game = undefined; removed = true; }
    if (role === 'launcher' && e.launcher === socket) { e.launcher = undefined; removed = true; }
    if (!e.game && !e.launcher) this.entries.delete(uuid);
    if (removed) this.changed(uuid);
    return removed;
  }

  // Games online per region, across every game (the launcher's Region dropdown).
  regionCounts(): Record<string, number> {
    const out: Record<string, number> = { us: 0, oceania: 0, europe: 0 };
    for (const e of this.entries.values()) {
      const r = e.game?.region;
      if (r && r in out) ++out[r];
    }
    return out;
  }

  // Games online per title (the admin overview).
  titleCounts(): Record<string, number> {
    const out: Record<string, number> = {};
    for (const e of this.entries.values()) {
      const t = e.game?.titleId;
      if (t) out[t] = (out[t] ?? 0) + 1;
    }
    return out;
  }

  // Games online in a title: everywhere, and in a region (no region, or Any: the same as everywhere).
  population(titleId: string, region?: string): { worldwide: number; region: number } {
    region = normalizeRegion(region);
    let worldwide = 0, inRegion = 0;
    for (const e of this.entries.values()) {
      if (e.game?.titleId !== titleId) continue;
      ++worldwide;
      if (!region || region === 'global' || e.game.region === region) ++inRegion;
    }
    return { worldwide, region: inRegion };
  }

  game(uuid: string): { socket: WebSocket; titleId?: string } | undefined {
    return this.entries.get(uuid)?.game;
  }

  hasLauncher(uuid: string): boolean {
    return !!this.entries.get(uuid)?.launcher;
  }

  sockets(uuid: string): WebSocket[] {
    const e = this.entries.get(uuid);
    if (!e) return [];
    return [e.game?.socket, e.launcher].filter((s): s is WebSocket => !!s);
  }

  onChange(cb: (uuid: string) => void): void {
    this.listeners.push(cb);
  }

  private changed(uuid: string): void {
    for (const cb of this.listeners) cb(uuid);
  }
}
