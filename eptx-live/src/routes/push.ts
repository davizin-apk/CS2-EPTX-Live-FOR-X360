/**
 * Consoles cadastrados e envio de conteudo para eles.
 *
 * Essas rotas sao usadas pelo celular ou pelo PC, nao pelo console: o console nao
 * roda nada nosso, ele so fica com a dash original aberta e o FTP do DashLaunch
 * escutando por baixo.
 */

import type { FastifyInstance } from 'fastify';
import { db, now } from '../lib/db.ts';
import { requireSession, sessionOf } from '../lib/session.ts';
import { createPushJob, runPushJob } from '../lib/push.ts';
import { testConnection } from '../lib/ftp.ts';

interface TargetRow {
  id: number;
  name: string;
  host: string;
  port: number;
  username: string;
  password: string;
  last_ok: number;
  created_at: number;
}

/** Nunca devolvemos a senha do FTP numa listagem. */
function targetDto(row: TargetRow) {
  return {
    id: row.id,
    name: row.name,
    host: row.host,
    port: row.port,
    username: row.username,
    lastOk: row.last_ok,
    createdAt: row.created_at,
  };
}

export function registerPushRoutes(app: FastifyInstance): void {
  const guard = { preHandler: requireSession };

  app.get('/api/v1/targets', guard, async () => {
    const rows = db.prepare('SELECT * FROM push_targets ORDER BY name COLLATE NOCASE').all() as unknown as TargetRow[];
    return { items: rows.map(targetDto) };
  });

  app.post('/api/v1/targets', guard, async (req, reply) => {
    if (!sessionOf(req).isAdmin) return reply.code(403).send({ error: 'apenas_admin' });

    const { name, host, port, username, password } = (req.body ?? {}) as {
      name?: string; host?: string; port?: number; username?: string; password?: string;
    };
    if (typeof name !== 'string' || !name.trim()) return reply.code(400).send({ error: 'nome_obrigatorio' });
    if (typeof host !== 'string' || !host.trim()) return reply.code(400).send({ error: 'host_obrigatorio' });

    try {
      const res = db.prepare(`
        INSERT INTO push_targets (name, host, port, username, password, created_at)
        VALUES (?, ?, ?, ?, ?, ?)`)
        .run(name.trim(), host.trim(), port ?? 21, username ?? 'xbox', password ?? 'xbox', now());
      return reply.code(201).send({ id: Number(res.lastInsertRowid) });
    } catch {
      return reply.code(409).send({ error: 'nome_em_uso' });
    }
  });

  app.delete('/api/v1/targets/:id', guard, async (req, reply) => {
    if (!sessionOf(req).isAdmin) return reply.code(403).send({ error: 'apenas_admin' });
    const { id } = req.params as { id: string };
    db.prepare('DELETE FROM push_targets WHERE id = ?').run(Number(id));
    return { ok: true };
  });

  /** Conecta e autentica sem transferir nada. Diz na hora se o FTP esta de pe. */
  app.post('/api/v1/targets/:id/test', guard, async (req, reply) => {
    const { id } = req.params as { id: string };
    const row = db.prepare('SELECT * FROM push_targets WHERE id = ?').get(Number(id)) as TargetRow | undefined;
    if (!row) return reply.code(404).send({ error: 'console_nao_encontrado' });

    const result = await testConnection({
      host: row.host, port: row.port, user: row.username, password: row.password,
    });
    if (result.ok) db.prepare('UPDATE push_targets SET last_ok = ? WHERE id = ?').run(now(), row.id);
    return result;
  });

  /**
   * Dispara o envio. Responde na hora com o id do job: um GOD leva dezenas de
   * minutos no Wi-Fi do 360, e segurar a resposta HTTP ate o fim so daria timeout.
   */
  app.post('/api/v1/push', guard, async (req, reply) => {
    const { titleId, targetId } = (req.body ?? {}) as { titleId?: number; targetId?: number };
    if (typeof titleId !== 'number' || typeof targetId !== 'number') {
      return reply.code(400).send({ error: 'campos_faltando' });
    }

    const title = db.prepare('SELECT id, kind, name FROM titles WHERE id = ?').get(titleId) as
      { id: number; kind: string; name: string } | undefined;
    if (!title) return reply.code(404).send({ error: 'titulo_nao_encontrado' });
    if (title.kind === 'iso') {
      return reply.code(422).send({
        error: 'iso_nao_instalavel',
        detail: 'FATX nao aceita arquivo maior que 4 GB. Converta o ISO para GOD antes de enviar.',
      });
    }
    if (!db.prepare('SELECT 1 FROM push_targets WHERE id = ?').get(targetId)) {
      return reply.code(404).send({ error: 'console_nao_encontrado' });
    }

    const jobId = createPushJob(title.id, targetId, sessionOf(req).userId);

    runPushJob(jobId).catch((err) => {
      // O estado de falha ja foi gravado no job; aqui so registramos no log.
      app.log.error({ err, jobId }, 'envio para o console falhou');
    });

    return reply.code(202).send({ jobId, title: title.name });
  });

  app.get('/api/v1/push/:id', guard, async (req, reply) => {
    const { id } = req.params as { id: string };
    const row = db.prepare(`
      SELECT j.*, t.name AS title_name, p.name AS target_name
      FROM push_jobs j
      JOIN titles t ON t.id = j.title_pk
      JOIN push_targets p ON p.id = j.target_id
      WHERE j.id = ?`).get(Number(id)) as Record<string, unknown> | undefined;
    if (!row) return reply.code(404).send({ error: 'job_nao_encontrado' });

    const total = Number(row.bytes_total) || 1;
    return { ...row, percent: Math.floor((Number(row.bytes_done) / total) * 100) };
  });

  app.get('/api/v1/push', guard, async () => {
    const rows = db.prepare(`
      SELECT j.id, j.state, j.bytes_done, j.bytes_total, j.files_done, j.files_total,
             j.current, j.error, j.updated_at, t.name AS title_name, p.name AS target_name
      FROM push_jobs j
      JOIN titles t ON t.id = j.title_pk
      JOIN push_targets p ON p.id = j.target_id
      ORDER BY j.updated_at DESC LIMIT 50`).all();
    return { items: rows };
  });
}
