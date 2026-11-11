import { buildApp } from './app.js';
import { openDb } from './db.js';
import { createMasterClient } from './master_client.js';
import { createDockerClient } from './docker_client.js';

async function main(): Promise<void> {
  const secret = process.env.ADMIN_SHARED_SECRET;
  if (!secret) throw new Error('ADMIN_SHARED_SECRET is not set');
  const app = await buildApp({
    db: openDb(process.env.ADMIN_DB || '/data/admin.db'),
    master: createMasterClient(process.env.MASTER_URL || 'http://master-server:8090', secret),
    docker: createDockerClient(process.env.DOCKER_URL || 'http://docker-proxy:2375')
  });
  // 0.0.0.0 inside the container only: compose does not publish 8080, caddy is the single way in.
  await app.listen({ host: '0.0.0.0', port: 8080 });
  console.log('[COGBox admin] listening on 8080');
}

main().catch((e) => { console.error((e as Error).message); process.exit(1); });
