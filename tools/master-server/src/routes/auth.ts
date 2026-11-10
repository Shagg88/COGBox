import { FastifyInstance, FastifyPluginAsync } from 'fastify';
import crypto from 'node:crypto';
import { DatabaseInstance } from '../db.js';
import { checkAuthHeader, checkToken, hashToken, newToken } from '../auth.js';
import { NAME_RULE, normalizeName } from '../names.js';

interface ClaimBody {
  uuid?: string;
  token?: string;
  display_name: string;
}

// A free discriminator for a name (case-insensitive), random first; '' when all 9999 are taken.
function pickDiscriminator(db: DatabaseInstance, name: string, exceptUuid = ''): string {
  const rows = db.prepare('SELECT discriminator FROM users WHERE display_name = ? COLLATE NOCASE AND uuid <> ?')
    .all(name, exceptUuid) as { discriminator: string }[];
  const used = new Set(rows.map(r => r.discriminator));
  if (used.size >= 9999) return '';
  for (let attempt = 0; attempt < 50; attempt++) {
    const candidate = String(crypto.randomInt(1, 10000)).padStart(4, '0');
    if (!used.has(candidate)) return candidate;
  }
  for (let i = 1; i <= 9999; i++) {
    const candidate = String(i).padStart(4, '0');
    if (!used.has(candidate)) return candidate;
  }
  return '';
}

export const authRoutes: FastifyPluginAsync<{ db: DatabaseInstance }> = async (fastify, opts) => {
  const { db } = opts;

  // Rename keeps the account (uuid, XUID, token); the discriminator stays unless another account already has the new
  // name with it.
  fastify.post<{ Body: { display_name?: unknown } }>('/rename', async (request, reply) => {
    const me = checkAuthHeader(db, request.headers.authorization);
    if (!me) {
      return reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    }
    const name = normalizeName(request.body?.display_name);
    if (!name) {
      return reply.status(400).send({ error: NAME_RULE });
    }
    const row = db.prepare('SELECT * FROM users WHERE uuid = ?').get(me.uuid) as any;
    const taken = db.prepare('SELECT 1 FROM users WHERE display_name = ? COLLATE NOCASE AND discriminator = ? AND uuid <> ?')
      .get(name, row.discriminator, me.uuid);
    const discriminator = taken ? pickDiscriminator(db, name, me.uuid) : row.discriminator;
    if (!discriminator) {
      return reply.status(409).send({ error: 'Discriminator namespace full for this display name' });
    }
    db.prepare('UPDATE users SET display_name = ?, discriminator = ?, last_seen = ? WHERE uuid = ?')
      .run(name, discriminator, Date.now(), me.uuid);
    return reply.send({ uuid: row.uuid, display_name: name, discriminator, tag: `${name}#${discriminator}`, xuid: row.xuid });
  });

  // The profile picture friends see: the name of one of the games' own pictures (every PC has the same names), or ''
  // for the letter. Never a path or a file of the player's.
  fastify.post<{ Body: { icon?: unknown } }>('/icon', async (request, reply) => {
    const me = checkAuthHeader(db, request.headers.authorization);
    if (!me) {
      return reply.status(401).send({ error: 'Authorization: COGBox <uuid>:<token> required' });
    }
    const icon = request.body?.icon;
    if (typeof icon !== 'string' || (icon !== '' && !/^icons\/[0-9A-F]{8}\/[0-9]{1,6}\.png$/.test(icon))) {
      return reply.status(400).send({ error: "icon: '' or icons/<TitleID>/<number>.png" });
    }
    db.prepare('UPDATE users SET icon = ?, last_seen = ? WHERE uuid = ?').run(icon, Date.now(), me.uuid);
    return reply.send({ icon });
  });

  fastify.post<{ Body: ClaimBody }>('/claim', async (request, reply) => {
    const { uuid, token, display_name } = request.body || {};

    const targetUuid = uuid && uuid.trim().length > 0 ? uuid.trim() : crypto.randomUUID();

    // An existing uuid is only answered for its own token (the uuid alone is not a secret).
    const existingUser = db.prepare('SELECT * FROM users WHERE uuid = ?').get(targetUuid) as any;
    if (existingUser) {
      if (!checkToken(db, targetUuid, token)) {
        return reply.status(401).send({ error: 'token required for an existing uuid' });
      }
      const now = Date.now();
      db.prepare('UPDATE users SET last_seen = ? WHERE uuid = ?').run(now, targetUuid);
      return reply.send({
        uuid: existingUser.uuid,
        display_name: existingUser.display_name,
        discriminator: existingUser.discriminator,
        tag: `${existingUser.display_name}#${existingUser.discriminator}`,
        xuid: existingUser.xuid
      });
    }

    // Only a new account's name is checked: a returning one keeps its stored name whatever its local file says.
    const cleanName = normalizeName(display_name);
    if (!cleanName) {
      return reply.status(400).send({ error: NAME_RULE });
    }

    // Atomic allocation transaction
    const claimTx = db.transaction(() => {
      const chosenDiscriminator = pickDiscriminator(db, cleanName);
      if (!chosenDiscriminator) {
        throw new Error('NamespaceExhausted');
      }

      // Allocate unique non-zero 64-bit authoritative XUID
      let chosenXuid = '';
      for (let attempt = 0; attempt < 20; attempt++) {
        const high = BigInt(0x00090000 | crypto.randomInt(1, 0xffff));
        const low = BigInt(crypto.randomInt(1, 0xffffffff));
        const xuidCandidate = ((high << 32n) | low).toString();
        const existingXuid = db.prepare('SELECT 1 FROM users WHERE xuid = ?').get(xuidCandidate);
        if (!existingXuid) {
          chosenXuid = xuidCandidate;
          break;
        }
      }

      const now = Date.now();
      const issued = newToken();
      db.prepare(`
        INSERT INTO users (uuid, display_name, discriminator, xuid, token_hash, created_at, last_seen)
        VALUES (?, ?, ?, ?, ?, ?, ?)
      `).run(targetUuid, cleanName, chosenDiscriminator, chosenXuid, hashToken(issued), now, now);

      // The token is sent only here; the server keeps its hash.
      return {
        uuid: targetUuid,
        display_name: cleanName,
        discriminator: chosenDiscriminator,
        tag: `${cleanName}#${chosenDiscriminator}`,
        xuid: chosenXuid,
        token: issued
      };
    });

    // Execute with retry on concurrent race condition
    for (let retry = 0; retry < 5; retry++) {
      try {
        const result = claimTx();
        return reply.status(200).send(result);
      } catch (err: any) {
        if (err.message === 'NamespaceExhausted') {
          return reply.status(409).send({ error: 'Discriminator namespace full for this display name' });
        }
        if (err.code?.startsWith('SQLITE_CONSTRAINT') || err.code === 'SQLITE_BUSY') {
          continue; // retry
        }
        throw err;
      }
    }

    return reply.status(500).send({ error: 'Failed to allocate identity after multiple attempts' });
  });
};
