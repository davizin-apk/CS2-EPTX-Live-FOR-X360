/**
 * Chat por texto.
 *
 * Usa long-polling em vez de WebSocket. Nao e nostalgia: a stack de rede do 360 nao
 * tem cliente WebSocket, e implementar o handshake e o enquadramento em cima de
 * sockets crus, no console, e muito mais codigo para manter do que um GET que fica
 * pendurado 25 segundos. Com dezenas de usuarios isso aguenta tranquilo.
 *
 * Fluxo do cliente:
 *   GET /api/v1/chat/<id>/messages?since=<ultimoId>&wait=25
 *   -> volta na hora se ja houver mensagem nova
 *   -> senao segura ate 25s e volta vazio; o cliente repete
 */

import type { FastifyInstance } from 'fastify';
import { db, now } from '../lib/db.ts';
import { requireSession, sessionOf } from '../lib/session.ts';

/** Quem esta com um long-poll pendurado, por conversa. */
const waiters = new Map<number, Set<() => void>>();

function notify(conversationId: number): void {
  const set = waiters.get(conversationId);
  if (!set) return;
  for (const resolve of set) resolve();
  set.clear();
}

function waitForMessage(conversationId: number, seconds: number): Promise<void> {
  return new Promise((resolve) => {
    let set = waiters.get(conversationId);
    if (!set) {
      set = new Set();
      waiters.set(conversationId, set);
    }
    const done = () => {
      clearTimeout(timer);
      set!.delete(done);
      resolve();
    };
    const timer = setTimeout(done, seconds * 1000);
    set.add(done);
  });
}

export function registerChatRoutes(app: FastifyInstance): void {
  const guard = { preHandler: requireSession };

  const isMember = db.prepare('SELECT 1 FROM conversation_members WHERE conversation_id = ? AND user_id = ?');
  const findUserByTag = db.prepare('SELECT id, gamertag FROM users WHERE gamertag_ci = ?');

  app.get('/api/v1/chat/conversations', guard, async (req) => {
    const me = sessionOf(req).userId;
    const rows = db.prepare(`
      SELECT c.id, c.kind, c.name, c.created_at, cm.last_read_id,
             (SELECT COUNT(*) FROM messages m
              WHERE m.conversation_id = c.id AND m.id > cm.last_read_id AND m.sender_id != ?) AS unread,
             (SELECT m.body FROM messages m WHERE m.conversation_id = c.id ORDER BY m.id DESC LIMIT 1) AS last_body,
             (SELECT m.created_at FROM messages m WHERE m.conversation_id = c.id ORDER BY m.id DESC LIMIT 1) AS last_at
      FROM conversations c
      JOIN conversation_members cm ON cm.conversation_id = c.id AND cm.user_id = ?
      ORDER BY COALESCE(last_at, c.created_at) DESC`).all(me, me) as Record<string, unknown>[];

    // Numa DM o nome util e o do outro participante, nao o campo name (vazio).
    const withNames = rows.map((r) => {
      if (r.kind !== 'dm') return r;
      const other = db.prepare(`
        SELECT u.gamertag FROM conversation_members cm
        JOIN users u ON u.id = cm.user_id
        WHERE cm.conversation_id = ? AND cm.user_id != ? LIMIT 1`).get(Number(r.id), me) as { gamertag: string } | undefined;
      return { ...r, name: other?.gamertag ?? '(desconhecido)' };
    });

    return { items: withNames };
  });

  /** Abre (ou reaproveita) a conversa privada com alguem. */
  app.post('/api/v1/chat/dm', guard, async (req, reply) => {
    const { gamertag } = (req.body ?? {}) as { gamertag?: string };
    if (typeof gamertag !== 'string') return reply.code(400).send({ error: 'campos_faltando' });

    const me = sessionOf(req).userId;
    const other = findUserByTag.get(gamertag.toLowerCase()) as { id: number } | undefined;
    if (!other) return reply.code(404).send({ error: 'gamertag_nao_encontrada' });
    if (other.id === me) return reply.code(400).send({ error: 'nao_pode_conversar_consigo' });

    // Uma DM e a conversa 'dm' com exatamente esses dois membros.
    const existing = db.prepare(`
      SELECT c.id FROM conversations c
      JOIN conversation_members a ON a.conversation_id = c.id AND a.user_id = ?
      JOIN conversation_members b ON b.conversation_id = c.id AND b.user_id = ?
      WHERE c.kind = 'dm' LIMIT 1`).get(me, other.id) as { id: number } | undefined;

    if (existing) return { conversationId: existing.id, created: false };

    const ts = now();
    const res = db.prepare("INSERT INTO conversations (kind, name, created_at) VALUES ('dm', '', ?)").run(ts);
    const convId = Number(res.lastInsertRowid);
    const addMember = db.prepare('INSERT INTO conversation_members (conversation_id, user_id) VALUES (?, ?)');
    addMember.run(convId, me);
    addMember.run(convId, other.id);

    return reply.code(201).send({ conversationId: convId, created: true });
  });

  app.get('/api/v1/chat/:id/messages', guard, async (req, reply) => {
    const conversationId = Number((req.params as { id: string }).id);
    const me = sessionOf(req).userId;
    if (!isMember.get(conversationId, me)) return reply.code(403).send({ error: 'nao_e_membro' });

    const q = (req.query ?? {}) as { since?: string; wait?: string; limit?: string };
    const since = Math.max(Number(q.since ?? 0) || 0, 0);
    const limit = Math.min(Math.max(Number(q.limit ?? 50) || 50, 1), 200);
    // Teto de 25s: acima disso proxies e o proprio console tendem a derrubar a conexao.
    const wait = Math.min(Math.max(Number(q.wait ?? 0) || 0, 0), 25);

    const fetch = () => db.prepare(`
      SELECT m.id, m.sender_id, u.gamertag AS sender, m.body, m.created_at
      FROM messages m JOIN users u ON u.id = m.sender_id
      WHERE m.conversation_id = ? AND m.id > ?
      ORDER BY m.id LIMIT ?`).all(conversationId, since, limit) as Record<string, unknown>[];

    let rows = fetch();
    if (rows.length === 0 && wait > 0) {
      await waitForMessage(conversationId, wait);
      rows = fetch();
    }

    if (rows.length > 0) {
      const lastId = Number(rows[rows.length - 1]!.id);
      db.prepare(`
        UPDATE conversation_members SET last_read_id = ?
        WHERE conversation_id = ? AND user_id = ? AND last_read_id < ?`)
        .run(lastId, conversationId, me, lastId);
    }

    return { conversationId, items: rows, cursor: rows.length ? Number(rows[rows.length - 1]!.id) : since };
  });

  app.post('/api/v1/chat/:id/messages', guard, async (req, reply) => {
    const conversationId = Number((req.params as { id: string }).id);
    const me = sessionOf(req);
    if (!isMember.get(conversationId, me.userId)) return reply.code(403).send({ error: 'nao_e_membro' });

    const { body } = (req.body ?? {}) as { body?: string };
    const text = typeof body === 'string' ? body.trim() : '';
    if (!text) return reply.code(400).send({ error: 'mensagem_vazia' });
    if (text.length > 1000) return reply.code(400).send({ error: 'mensagem_longa', max: 1000 });

    const ts = now();
    const res = db.prepare(
      'INSERT INTO messages (conversation_id, sender_id, body, created_at) VALUES (?, ?, ?, ?)',
    ).run(conversationId, me.userId, text, ts);

    notify(conversationId);

    return reply.code(201).send({
      id: Number(res.lastInsertRowid),
      sender: me.gamertag,
      body: text,
      createdAt: ts,
    });
  });
}
