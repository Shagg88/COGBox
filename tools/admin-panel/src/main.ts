import { buildApp } from './app.js';
import { openDb } from './db.js';
import { createMasterClient } from './master_client.js';
import { createDockerClient } from './docker_client.js';

async function main(): Promise<void> {
  const secret = process.env.ADMIN_SHARED_SECRET;
  if (!secret) throw new Error('ADMIN_SHARED_SECRET is not set');
  const app = await buildApp({
    db: openDb(process.env.ADMIN_DB || '/data/admin.db'),
    master: createMasterClient(process.env.MASTER_URL || 'http://127.0.0.1:8090', secret),
    docker: createDockerClient(process.env.DOCKER_URL || 'http://127.0.0.1:2375')
  });
  // Loopback only (host networking): caddy on the same host is the single way in.
  const port = Number(process.env.PORT) || 8080;
  await app.listen({ host: process.env.HOST || '127.0.0.1', port });
  console.log(`[COGBox admin] listening on port ${port}`);
}

main().catch((e) => { console.error((e as Error).message); process.exit(1); });
