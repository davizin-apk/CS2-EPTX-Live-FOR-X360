/**
 * Autenticacao das requisicoes vindas do console.
 *
 * O Xbox 360 nao tem uma stack TLS que a gente possa confiar: a cadeia de
 * certificados dele e a da Microsoft, e nao da para instalar uma raiz propria sem
 * patchear o xam.xex. Entao em vez de TLS, cada requisicao vai assinada:
 *
 *   X-EPTX-Session     token opaco devolvido no login
 *   X-EPTX-Timestamp   unix seconds do console
 *   X-EPTX-Nonce       aleatorio, unico por requisicao
 *   X-EPTX-Signature   HMAC-SHA256 hex da string canonica, com o segredo da sessao
 *
 * O segredo da sessao trafega uma unica vez, na resposta do login. Depois disso
 * nada sensivel vai em claro, e um atacante na LAN nao consegue nem replayar
 * (nonce de uso unico) nem forjar (nao tem o segredo).
 *
 * A janela de timestamp e larga de proposito: o relogio do 360 anda errado com
 * frequencia, e uma janela apertada quebraria o login sem motivo.
 */

import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import { config } from './config.ts';
import { db, now } from './db.ts';
import { canonicalString, signatureMatches } from './crypto.ts';

export interface SessionUser {
  userId: number;
  gamertag: string;
  isAdmin: boolean;
  sessionToken: string;
  consoleId: number | null;
}

declare module 'fastify' {
  interface FastifyRequest {
    rawBody: Buffer;
    session?: SessionUser;
  }
}

/**
 * Guarda o corpo cru antes de fazer o parse do JSON. A assinatura cobre os bytes
 * exatos que chegaram; reserializar o objeto daria um hash diferente.
 */
export function registerRawBody(app: FastifyInstance): void {
  app.addContentTypeParser(
    'application/json',
    { parseAs: 'buffer' },
    (req: FastifyRequest, body: Buffer, done) => {
      req.rawBody = body;
      if (body.length === 0) return done(null, {});
      try {
        done(null, JSON.parse(body.toString('utf8')));
      } catch {
        done(Object.assign(new Error('JSON invalido'), { statusCode: 400 }), undefined);
      }
    },
  );
  // O avatar sobe como PNG ou GIF cru; a assinatura cobre esses bytes do mesmo jeito.
  app.addContentTypeParser(['image/png', 'image/gif'], { parseAs: 'buffer' }, (req: FastifyRequest, body: Buffer, done) => {
    req.rawBody = body;
    done(null, body);
  });
  app.addHook('onRequest', async (req) => {
    if (!req.rawBody) req.rawBody = Buffer.alloc(0);
  });
}

const selectSession = db.prepare(`
  SELECT s.token, s.secret, s.console_id, u.id AS user_id, u.gamertag, u.is_admin
  FROM sessions s JOIN users u ON u.id = s.user_id
  WHERE s.token = ?`);
const touchSession = db.prepare('UPDATE sessions SET last_seen = ? WHERE token = ?');
const insertNonce = db.prepare('INSERT INTO used_nonces (nonce, expires_at) VALUES (?, ?)');
const purgeNonces = db.prepare('DELETE FROM used_nonces WHERE expires_at < ?');

let lastPurge = 0;

function header(req: FastifyRequest, name: string): string | null {
  const v = req.headers[name];
  if (typeof v === 'string') return v;
  if (Array.isArray(v)) return v[0] ?? null;
  return null;
}

/** preHandler do Fastify: exige uma requisicao assinada e valida. */
export async function requireSession(req: FastifyRequest, reply: FastifyReply): Promise<void> {
  const token = header(req, 'x-eptx-session');
  const timestamp = header(req, 'x-eptx-timestamp');
  const nonce = header(req, 'x-eptx-nonce');
  const signature = header(req, 'x-eptx-signature');

  if (!token || !timestamp || !nonce || !signature) {
    return reply.code(401).send({ error: 'assinatura_ausente' });
  }

  const ts = Number(timestamp);
  const current = now();
  if (!Number.isFinite(ts) || Math.abs(current - ts) > config.clockSkewSeconds) {
    // O cliente deve reler o horario do servidor em /api/v1/time e tentar de novo.
    return reply.code(401).send({ error: 'timestamp_fora_da_janela', serverTime: current });
  }

  const row = selectSession.get(token) as
    | { token: string; secret: string; console_id: number | null; user_id: number; gamertag: string; is_admin: number }
    | undefined;
  if (!row) return reply.code(401).send({ error: 'sessao_invalida' });

  const canonical = canonicalString(req.method, req.url, timestamp, nonce, req.rawBody ?? Buffer.alloc(0));
  if (!signatureMatches(row.secret, canonical, signature)) {
    return reply.code(401).send({ error: 'assinatura_invalida' });
  }

  // Nonce de uso unico. A chave primaria faz o trabalho: colisao = replay.
  if (current - lastPurge > 60) {
    purgeNonces.run(current);
    lastPurge = current;
  }
  try {
    insertNonce.run(nonce, current + config.clockSkewSeconds * 2);
  } catch {
    return reply.code(401).send({ error: 'nonce_repetido' });
  }

  touchSession.run(current, token);
  req.session = {
    userId: row.user_id,
    gamertag: row.gamertag,
    isAdmin: row.is_admin === 1,
    sessionToken: row.token,
    consoleId: row.console_id,
  };
}

/** Usa depois de requireSession; lanca se chamada fora de uma rota protegida. */
export function sessionOf(req: FastifyRequest): SessionUser {
  if (!req.session) throw new Error('rota sem requireSession no preHandler');
  return req.session;
}
