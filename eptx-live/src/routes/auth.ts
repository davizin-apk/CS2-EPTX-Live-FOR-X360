/**
 * Contas e login.
 *
 * Dois caminhos de entrada, porque digitar senha com o controle e horrivel:
 *
 *   1. Login direto     gamertag + senha, para quem tem Chatpad ou teclado USB.
 *   2. Pareamento       o console mostra um codigo de 6 caracteres na TV e voce
 *                       aprova no celular. E o caminho recomendado.
 *
 * Nenhuma rota daqui exige assinatura HMAC: sao justamente as rotas que
 * estabelecem o segredo usado para assinar todo o resto.
 */

import type { FastifyInstance } from 'fastify';
import { db, now } from '../lib/db.ts';
import { config } from '../lib/config.ts';
import { hashPassword, verifyPassword, randomToken, userCode } from '../lib/crypto.ts';
import { randomBytes } from 'node:crypto';

const GAMERTAG_RE = /^[A-Za-z0-9][A-Za-z0-9 _-]{2,14}$/;

interface UserRow {
  id: number;
  gamertag: string;
  password_hash: string;
  is_admin: number;
}

const findUser = db.prepare('SELECT id, gamertag, password_hash, is_admin FROM users WHERE gamertag_ci = ?');
const countUsers = db.prepare('SELECT COUNT(*) AS n FROM users');
const insertUser = db.prepare(
  'INSERT INTO users (gamertag, gamertag_ci, password_hash, is_admin, created_at) VALUES (?, ?, ?, ?, ?)',
);
const upsertConsole = db.prepare(`
  INSERT INTO consoles (user_id, hw_id, label, created_at) VALUES (?, ?, ?, ?)
  ON CONFLICT(user_id, hw_id) DO UPDATE SET label = excluded.label`);
const findConsole = db.prepare('SELECT id FROM consoles WHERE user_id = ? AND hw_id = ?');
const insertSession = db.prepare(
  'INSERT INTO sessions (token, secret, user_id, console_id, created_at, last_seen) VALUES (?, ?, ?, ?, ?, ?)',
);
const upsertPresence = db.prepare(`
  INSERT INTO presence (user_id, state, updated_at) VALUES (?, 'online', ?)
  ON CONFLICT(user_id) DO UPDATE SET state = 'online', updated_at = excluded.updated_at`);

/** Cria a sessao e devolve o par token/segredo. O segredo so aparece aqui, uma vez. */
function createSession(userId: number, hwId: string | null, label: string) {
  const ts = now();
  let consoleId: number | null = null;
  if (hwId) {
    upsertConsole.run(userId, hwId, label, ts);
    const row = findConsole.get(userId, hwId) as { id: number } | undefined;
    consoleId = row?.id ?? null;
  }
  const token = randomToken(24);
  const secret = randomBytes(32).toString('hex');
  insertSession.run(token, secret, userId, consoleId, ts, ts);
  upsertPresence.run(userId, ts);
  return { token, secret, consoleId, expiresIn: config.sessionTtlSeconds };
}

export function registerAuthRoutes(app: FastifyInstance): void {
  /** O console consulta isto antes do login para alinhar o relogio com o servidor. */
  app.get('/api/v1/time', async () => ({ serverTime: now(), clockSkew: config.clockSkewSeconds }));

  app.post('/api/v1/auth/register', async (req, reply) => {
    const { gamertag, password } = (req.body ?? {}) as { gamertag?: string; password?: string };

    if (typeof gamertag !== 'string' || !GAMERTAG_RE.test(gamertag)) {
      return reply.code(400).send({
        error: 'gamertag_invalida',
        detail: '3 a 15 caracteres: letras, numeros, espaco, hifen ou underscore, comecando com letra ou numero.',
      });
    }
    if (typeof password !== 'string' || password.length < 6) {
      return reply.code(400).send({ error: 'senha_curta', detail: 'Minimo de 6 caracteres.' });
    }
    if (findUser.get(gamertag.toLowerCase())) {
      return reply.code(409).send({ error: 'gamertag_em_uso' });
    }

    // O primeiro cadastro do servidor vira admin; nao ha outro jeito de criar o primeiro.
    const isFirst = (countUsers.get() as { n: number }).n === 0;
    const res = insertUser.run(gamertag, gamertag.toLowerCase(), hashPassword(password), isFirst ? 1 : 0, now());

    return reply.code(201).send({
      userId: Number(res.lastInsertRowid),
      gamertag,
      isAdmin: isFirst,
    });
  });

  app.post('/api/v1/auth/login', async (req, reply) => {
    const { gamertag, password, hwId, label } = (req.body ?? {}) as {
      gamertag?: string; password?: string; hwId?: string; label?: string;
    };
    if (typeof gamertag !== 'string' || typeof password !== 'string') {
      return reply.code(400).send({ error: 'campos_faltando' });
    }

    const user = findUser.get(gamertag.toLowerCase()) as UserRow | undefined;
    // Verificamos a senha mesmo sem usuario para nao vazar quais gamertags existem
    // pela diferenca de tempo de resposta.
    const ok = user
      ? verifyPassword(password, user.password_hash)
      : verifyPassword(password, hashPassword('placeholder-para-tempo-constante'));

    if (!user || !ok) return reply.code(401).send({ error: 'credenciais_invalidas' });

    const session = createSession(user.id, typeof hwId === 'string' ? hwId : null, typeof label === 'string' ? label : '');
    return {
      userId: user.id,
      gamertag: user.gamertag,
      isAdmin: user.is_admin === 1,
      ...session,
    };
  });

  // --- Pareamento por codigo -------------------------------------------------

  const insertCode = db.prepare(
    'INSERT INTO device_codes (device_code, user_code, hw_id, expires_at, created_at) VALUES (?, ?, ?, ?, ?)',
  );
  const findByDeviceCode = db.prepare('SELECT * FROM device_codes WHERE device_code = ?');
  const findByUserCode = db.prepare('SELECT * FROM device_codes WHERE user_code = ?');
  const approveCode = db.prepare('UPDATE device_codes SET approved = 1, user_id = ? WHERE device_code = ?');
  const claimCode = db.prepare('UPDATE device_codes SET claimed = 1 WHERE device_code = ?');
  const purgeCodes = db.prepare('DELETE FROM device_codes WHERE expires_at < ?');

  interface CodeRow {
    device_code: string; user_code: string; hw_id: string;
    user_id: number | null; approved: number; claimed: number; expires_at: number;
  }

  /** Passo 1, no console: gera o codigo que aparece na TV. */
  app.post('/api/v1/auth/device/start', async (req, reply) => {
    const { hwId } = (req.body ?? {}) as { hwId?: string };
    if (typeof hwId !== 'string' || hwId.length < 4) {
      return reply.code(400).send({ error: 'hwid_invalido' });
    }
    purgeCodes.run(now());

    const deviceCode = randomToken(24);
    // Improvavel, mas se o codigo curto colidir com um vivo, geramos outro.
    let code = userCode();
    for (let i = 0; i < 10 && findByUserCode.get(code); i++) code = userCode();

    const ts = now();
    insertCode.run(deviceCode, code, hwId, ts + config.deviceCodeTtlSeconds, ts);

    return reply.code(201).send({
      deviceCode,
      userCode: code,
      expiresIn: config.deviceCodeTtlSeconds,
      pollInterval: 5,
    });
  });

  /** Passo 2, no celular: aprova o codigo mostrado na TV. */
  app.post('/api/v1/auth/device/approve', async (req, reply) => {
    const { userCode: code, gamertag, password } = (req.body ?? {}) as {
      userCode?: string; gamertag?: string; password?: string;
    };
    if (typeof code !== 'string' || typeof gamertag !== 'string' || typeof password !== 'string') {
      return reply.code(400).send({ error: 'campos_faltando' });
    }

    const user = findUser.get(gamertag.toLowerCase()) as UserRow | undefined;
    if (!user || !verifyPassword(password, user.password_hash)) {
      return reply.code(401).send({ error: 'credenciais_invalidas' });
    }

    const row = findByUserCode.get(code.trim().toUpperCase()) as CodeRow | undefined;
    if (!row) return reply.code(404).send({ error: 'codigo_nao_encontrado' });
    if (row.expires_at < now()) return reply.code(410).send({ error: 'codigo_expirado' });
    if (row.approved === 1) return reply.code(409).send({ error: 'codigo_ja_usado' });

    approveCode.run(user.id, row.device_code);
    return { ok: true, gamertag: user.gamertag };
  });

  /** Passo 3, no console: pergunta de tempos em tempos se ja aprovaram. */
  app.post('/api/v1/auth/device/poll', async (req, reply) => {
    const { deviceCode } = (req.body ?? {}) as { deviceCode?: string };
    if (typeof deviceCode !== 'string') return reply.code(400).send({ error: 'campos_faltando' });

    const row = findByDeviceCode.get(deviceCode) as CodeRow | undefined;
    if (!row) return reply.code(404).send({ error: 'codigo_nao_encontrado' });
    if (row.expires_at < now()) return reply.code(410).send({ error: 'codigo_expirado' });
    if (row.approved !== 1) return { status: 'pending', pollInterval: 5 };
    if (row.claimed === 1) return reply.code(409).send({ error: 'codigo_ja_usado' });

    claimCode.run(deviceCode);
    const user = db.prepare('SELECT id, gamertag, is_admin FROM users WHERE id = ?').get(row.user_id) as
      { id: number; gamertag: string; is_admin: number } | undefined;
    if (!user) return reply.code(500).send({ error: 'usuario_sumiu' });

    const session = createSession(user.id, row.hw_id, '');
    return {
      status: 'approved',
      userId: user.id,
      gamertag: user.gamertag,
      isAdmin: user.is_admin === 1,
      ...session,
    };
  });
}
