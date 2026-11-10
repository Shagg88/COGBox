import WebSocket from 'ws';

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

  // Returns the socket this one replaced, if any (the caller closes it).
  add(uuid: string, role: Role, socket: WebSocket, titleId?: string, region?: string): WebSocket | undefined {
    const e = this.entries.get(uuid) ?? {};
    let replaced: WebSocket | undefined;
    if (role === 'game') {
      replaced = e.game?.socket;
      e.game = { socket, titleId, region };
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
    const out: Record<string, number> = { 'us-e': 0, 'us-w': 0, oceania: 0, europe: 0 };
    for (const e of this.entries.values()) {
      const r = e.game?.region;
      if (r && r in out) ++out[r];
    }
    return out;
  }

  // Games online in a title: everywhere, and in a region (no region, or Any: the same as everywhere).
  population(titleId: string, region?: string): { worldwide: number; region: number } {
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
