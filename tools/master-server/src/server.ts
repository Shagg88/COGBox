import Fastify, { FastifyInstance } from 'fastify';
import cors from '@fastify/cors';
import rateLimit from '@fastify/rate-limit';
import fastifyWebsocket from '@fastify/websocket';
import { initDb, DatabaseInstance } from './db.js';
import { authRoutes } from './routes/auth.js';
import { lobbyRoutes } from './routes/lobbies.js';
import { signalingRoutes } from './ws/signaling.js';
import { friendRoutes } from './routes/friends.js';
import { inviteRoutes } from './routes/invites.js';
import { checkAuthHeader } from './auth.js';
import { Registry } from './social/registry.js';
import { notifyPresence } from './social/presence.js';

export interface ServerOptions {
  dbPath?: string;
  logger?: boolean;
  rateLimitMax?: number;   // requests per minute per address (tests raise it)
  now?: () => number;      // the clock (tests move it)
}

export async function buildServer(opts: ServerOptions = {}): Promise<FastifyInstance> {
  const server = Fastify({
    logger: opts.logger ?? false,
    trustProxy: '127.0.0.1'   // Caddy on this machine says who the player is; nobody else can
  });

  const db: DatabaseInstance = initDb(opts.dbPath);

  // Decorate server with db
  server.decorate('db', db);

  // Register CORS
  await server.register(cors, {
    origin: true
  });

  // Register Rate Limit
  await server.register(rateLimit, {
    max: opts.rateLimitMax ?? 120,
    timeWindow: '1 minute',
    // Per signed-in account (players can share an address: a household, CGNAT), checked against the token so made-up
    // accounts cannot dodge the limit; everything else per address.
    keyGenerator: (request) => {
      const u = request.headers.authorization ? checkAuthHeader(db, request.headers.authorization) : null;
      return u ? `account:${u.uuid}` : request.ip;
    }
  });

  // Register WebSocket plugin
  await server.register(fastifyWebsocket, {
    options: {
      maxPayload: 1048576 // 1MB limit for signaling frames
    }
  });

  // Healthcheck endpoint for OCI monitoring
  server.get('/api/v1/health', async () => {
    return {
      status: 'ok',
      timestamp: Date.now()
    };
  });

  // Register API routes
  await server.register(authRoutes, { prefix: '/api/v1/auth', db });
  // Who is connected (launcher and game per account): signaling, presence and the friends API share it.
  const registry = new Registry(opts.now ?? Date.now);
  // Every signed-in request marks its account as seen (the launcher's 5 s poll keeps it "Online - In Launcher").
  server.addHook('onRequest', async (request) => {
    if (!request.headers.authorization) return;
    // A game has its socket for that: its requests must not keep a closed game online for another 60 s.
    if (request.headers['user-agent']?.startsWith('COGBox-online/')) return;
    const u = checkAuthHeader(db, request.headers.authorization);
    if (u) registry.seen(u.uuid);
  });
  await server.register(lobbyRoutes, {
    prefix: '/api/v1/lobbies', db, onHostChange: (uuid: string) => notifyPresence(db, registry, uuid),
    inContact: (a: string, b: string) => registry.inContact(a, b)
  });
  await server.register(signalingRoutes, { prefix: '/ws', db, registry });
  server.get('/api/v1/population/regions', async () => registry.regionCounts());
  server.get<{ Querystring: { title_id?: string; region?: string } }>('/api/v1/population', async (req) =>
    registry.population(String(req.query.title_id ?? '').toUpperCase(), req.query.region));
  await server.register(friendRoutes, { prefix: '/api/v1/friends', db, registry });
  await server.register(inviteRoutes, { prefix: '/api/v1/invites', db, registry, now: opts.now ?? Date.now });

  // Clean shutdown hook
  server.addHook('onClose', async () => {
    db.close();
  });

  return server;
}

// Start standalone server if executed directly
if (process.argv[1] && (process.argv[1].endsWith('server.ts') || process.argv[1].endsWith('server.js'))) {
  const port = Number(process.env.PORT) || 443;
  const host = process.env.HOST || '0.0.0.0';
  const dbPath = process.env.DB_PATH;

  buildServer({ dbPath, logger: true }).then(async (app) => {
    try {
      await app.listen({ port, host });
      console.log(`[COGBox] Master Server coordinator listening on ${host}:${port}`);
    } catch (err) {
      app.log.error(err);
      process.exit(1);
    }
  });
}
