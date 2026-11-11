import type { Db } from './db.js';

export type AuditAction = 'login' | 'login_failed' | 'logout' | 'password_changed' | 'restart' | 'backup' | 'lobby_close';

export function audit(db: Db, name: string, action: AuditAction, detail = '', now = Date.now()): void {
  db.prepare('INSERT INTO audit (time, name, action, detail) VALUES (?, ?, ?, ?)').run(now, name, action, detail);
}
