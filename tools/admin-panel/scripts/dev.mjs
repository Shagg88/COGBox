// Local look at the pages with fake clients. Dev only: not copied into the Docker image.
// Run: npm run build && node scripts/dev.mjs   (then open http://localhost:8787/admin/)
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { buildApp } = require('../dist/app.js');
const { openDb } = require('../dist/db.js');
const { addUser } = require('../dist/users.js');

const now = Date.now();
const lobbies = [
  { lobby_id: 'L1', room_code: 'K7QX', title_id: '4D53082D', host_tag: 'Dom Santiago', is_public: true, created_at: now - 12 * 60_000 },
  { lobby_id: 'L2', room_code: 'Z2PL', title_id: '4D5308AB', host_tag: '<script>x</script>', is_public: false, created_at: now - 3 * 3600_000 },
];
const errors = [{ time: now - 60_000, msg: 'ws: bad frame <b>not bold</b>' }];
const masterUp = process.argv.includes('--master-down') ? false : true;
const master = {
  status: async () => masterUp ? { uptime_s: 93_000, version: '0.5.0', players: { total: 7, by_title: { '4D53082D': 4, '4D5308AB': 3 }, by_region: { EU: 5, NA: 2 } }, lobbies: lobbies.length, pending_invites: 1, errors } : null,
  lobbies: async () => lobbies,
  closeLobby: async (id) => { const i = lobbies.findIndex((l) => l.lobby_id === id); if (i < 0) return false; lobbies.splice(i, 1); return true; },
  backup: async () => ({ name: 'master-20261111120000.db', size: 2_400_000 }),
  backups: async () => [{ name: 'master-20261110120000.db', size: 2_300_000, mtime: now - 86_400_000 }],
};
const dockerDown = process.argv.includes('--docker-down');
const up = { up: true, started_at: new Date(now - 93_000_000).toISOString(), restarts: 1 };
const docker = {
  health: async () => { if (dockerDown) throw new Error('no socket'); return { 'master-server': up, coturn: up, caddy: { up: false, started_at: '', restarts: 3 } }; },
  logs: async (s) => Array.from({ length: 300 }, (_, i) => `${s} line ${i} <i>tag</i>`),
  restart: async (s) => { if (s === 'coturn') throw new Error('coturn: container not found'); },
};

const db = openDb(':memory:');
const c = await addUser(db, 'dev');
// Shown only in this terminal; the database is in memory and gone when the script stops.
console.log(`user dev  password: ${c.password}  secret: ${new URL(c.otpauth).searchParams.get('secret')}`);
const app = await buildApp({ db, master, docker, secureCookie: false });
await app.listen({ host: '127.0.0.1', port: 8787 });
console.log('http://localhost:8787/admin/');
