import { FastifyPluginAsync } from 'fastify';
import { DatabaseInstance } from '../db.js';
import { checkAuthHeader } from '../auth.js';
import { tagsForXuids } from '../social/tags.js';

// GitHub #353: POST /api/v1/players/tags {xuids: ["2533274790395904", ...]} -> {tags: [{xuid, display_name,
// discriminator}]} for the XUIDs that belong to a COGBox account (bots and unknown XUIDs are left out). Signed-in only.
export const playerRoutes: FastifyPluginAsync<{ db: DatabaseInstance }> = async (fastify, opts) => {
  const { db } = opts;
  fastify.post<{ Body: { xuids?: unknown } }>('/tags', async (req, reply) => {
    if (!checkAuthHeader(db, req.headers.authorization))
      return reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    const xuids = Array.isArray(req.body?.xuids) ? req.body.xuids : [];
    return { tags: tagsForXuids(db, xuids) };
  });
};
