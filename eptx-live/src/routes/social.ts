/**
 * Amigos e presenca.
 *
 * Presenca e por heartbeat: o console reporta a cada ~30s. Se parar de reportar
 * por mais que presenceTimeoutSeconds, tratamos como offline sem precisar que ele
 * avise — o que e o caso comum, ja que desligar o 360 nao manda nenhum "tchau".
 */

import type { FastifyInstance } from 'fastify';
import { db, now } from '../lib/db.ts';
import { config } from '../lib/config.ts';
import { requireSession, sessionOf } from '../lib/session.ts';
import { avatarInfo } from './avatar.ts';

interface PresenceRow {
  state: string;
  title_id: string;
  title_name: string;
  detail: string;
  updated_at: number;
}

/** Presenca gravada, mas rebaixada para offline se o heartbeat esfriou. */
function effectivePresence(p: PresenceRow | undefined) {
  const ts = now();
  if (!p || ts - p.updated_at > config.presenceTimeoutSeconds) {
    return { state: 'offline', titleId: '', titleName: '', detail: '', updatedAt: p?.updated_at ?? 0 };
  }
  return {
    state: p.state,
    titleId: p.title_id,
    titleName: p.title_name,
    detail: p.detail,
    updatedAt: p.updated_at,
  };
}

export function registerSocialRoutes(app: FastifyInstance): void {
  const guard = { preHandler: requireSession };

  const findUserByTag = db.prepare('SELECT id, gamertag FROM users WHERE gamertag_ci = ?');
  const findPresence = db.prepare('SELECT * FROM presence WHERE user_id = ?');

  const listFriends = db.prepare(`
    SELECT f.id, f.status, f.requester, f.addressee,
           u.id AS other_id, u.gamertag, u.motto
    FROM friendships f
    JOIN users u ON u.id = CASE WHEN f.requester = ? THEN f.addressee ELSE f.requester END
    WHERE (f.requester = ? OR f.addressee = ?) AND f.status = 'accepted'
    ORDER BY u.gamertag COLLATE NOCASE`);

  app.get('/api/v1/friends', guard, async (req) => {
    const me = sessionOf(req).userId;
    const rows = listFriends.all(me, me, me) as
      { id: number; other_id: number; gamertag: string; motto: string }[];

    return {
      items: rows.map((r) => ({
        friendshipId: r.id,
        userId: r.other_id,
        gamertag: r.gamertag,
        motto: r.motto,
        ...avatarInfo(r.other_id),
        presence: effectivePresence(findPresence.get(r.other_id) as PresenceRow | undefined),
      })),
    };
  });

  app.get('/api/v1/friends/requests', guard, async (req) => {
    const me = sessionOf(req).userId;
    const incoming = db.prepare(`
      SELECT f.id, u.id AS user_id, u.gamertag, f.created_at
      FROM friendships f JOIN users u ON u.id = f.requester
      WHERE f.addressee = ? AND f.status = 'pending'`).all(me);
    const outgoing = db.prepare(`
      SELECT f.id, u.id AS user_id, u.gamertag, f.created_at
      FROM friendships f JOIN users u ON u.id = f.addressee
      WHERE f.requester = ? AND f.status = 'pending'`).all(me);
    return { incoming, outgoing };
  });

  app.post('/api/v1/friends/requests', guard, async (req, reply) => {
    const { gamertag } = (req.body ?? {}) as { gamertag?: string };
    if (typeof gamertag !== 'string') return reply.code(400).send({ error: 'campos_faltando' });

    const me = sessionOf(req).userId;
    const other = findUserByTag.get(gamertag.toLowerCase()) as { id: number; gamertag: string } | undefined;
    if (!other) return reply.code(404).send({ error: 'gamertag_nao_encontrada' });
    if (other.id === me) return reply.code(400).send({ error: 'nao_pode_adicionar_a_si_mesmo' });

    const existing = db.prepare(`
      SELECT id, status, requester FROM friendships
      WHERE (requester = ? AND addressee = ?) OR (requester = ? AND addressee = ?)`)
      .get(me, other.id, other.id, me) as { id: number; status: string; requester: number } | undefined;

    if (existing) {
      if (existing.status === 'accepted') return reply.code(409).send({ error: 'ja_sao_amigos' });
      if (existing.status === 'blocked') return reply.code(403).send({ error: 'bloqueado' });
      // Pedido cruzado: os dois se adicionaram. Vira amizade direto.
      if (existing.requester === other.id) {
        db.prepare("UPDATE friendships SET status = 'accepted', updated_at = ? WHERE id = ?").run(now(), existing.id);
        return { status: 'accepted', friendshipId: existing.id };
      }
      return reply.code(409).send({ error: 'pedido_ja_enviado' });
    }

    const ts = now();
    const res = db.prepare(
      "INSERT INTO friendships (requester, addressee, status, created_at, updated_at) VALUES (?, ?, 'pending', ?, ?)",
    ).run(me, other.id, ts, ts);
    return reply.code(201).send({ status: 'pending', friendshipId: Number(res.lastInsertRowid) });
  });

  app.post('/api/v1/friends/requests/:id/accept', guard, async (req, reply) => {
    const { id } = req.params as { id: string };
    const me = sessionOf(req).userId;
    const row = db.prepare('SELECT id, addressee, status FROM friendships WHERE id = ?').get(Number(id)) as
      { id: number; addressee: number; status: string } | undefined;

    if (!row || row.addressee !== me) return reply.code(404).send({ error: 'pedido_nao_encontrado' });
    if (row.status !== 'pending') return reply.code(409).send({ error: 'pedido_ja_resolvido' });

    db.prepare("UPDATE friendships SET status = 'accepted', updated_at = ? WHERE id = ?").run(now(), row.id);
    return { status: 'accepted' };
  });

  /** Serve tanto para recusar um pedido quanto para remover um amigo. */
  app.delete('/api/v1/friends/:id', guard, async (req, reply) => {
    const { id } = req.params as { id: string };
    const me = sessionOf(req).userId;
    const row = db.prepare('SELECT id, requester, addressee FROM friendships WHERE id = ?').get(Number(id)) as
      { id: number; requester: number; addressee: number } | undefined;

    if (!row || (row.requester !== me && row.addressee !== me)) {
      return reply.code(404).send({ error: 'amizade_nao_encontrada' });
    }
    db.prepare('DELETE FROM friendships WHERE id = ?').run(row.id);
    return { ok: true };
  });

  /** Heartbeat. O console chama isto a cada ~30s enquanto estiver ligado. */
  app.post('/api/v1/presence', guard, async (req, reply) => {
    const { state, titleId, titleName, detail } = (req.body ?? {}) as {
      state?: string; titleId?: string; titleName?: string; detail?: string;
    };
    const allowed = ['online', 'away', 'offline'];
    if (typeof state !== 'string' || !allowed.includes(state)) {
      return reply.code(400).send({ error: 'estado_invalido', allowed });
    }

    db.prepare(`
      INSERT INTO presence (user_id, state, title_id, title_name, detail, updated_at)
      VALUES (?, ?, ?, ?, ?, ?)
      ON CONFLICT(user_id) DO UPDATE SET
        state = excluded.state, title_id = excluded.title_id,
        title_name = excluded.title_name, detail = excluded.detail,
        updated_at = excluded.updated_at`)
      .run(sessionOf(req).userId, state, titleId ?? '', titleName ?? '', detail ?? '', now());

    return { ok: true, heartbeatSeconds: Math.floor(config.presenceTimeoutSeconds / 4) };
  });

  app.get('/api/v1/me', guard, async (req) => {
    const s = sessionOf(req);
    const user = db.prepare('SELECT id, gamertag, motto, avatar, is_admin, created_at FROM users WHERE id = ?')
      .get(s.userId) as { id: number; gamertag: string; motto: string; avatar: string; is_admin: number; created_at: number };
    return {
      ...user,
      isAdmin: user.is_admin === 1,
      ...avatarInfo(s.userId),
      presence: effectivePresence(findPresence.get(s.userId) as PresenceRow | undefined),
    };
  });
}
