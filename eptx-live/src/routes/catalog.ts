/**
 * Catalogo: o que existe na biblioteca e, para cada titulo, a lista de arquivos
 * com o caminho de destino ja calculado. O console nao precisa saber nada sobre
 * formatos do 360 — ele so baixa cada arquivo e grava onde o servidor mandou.
 */

import type { FastifyInstance } from 'fastify';
import { readFile } from 'node:fs/promises';
import { db } from '../lib/db.ts';
import { requireSession, sessionOf } from '../lib/session.ts';
import { scanLibrary } from '../lib/scanner.ts';

interface TitleRow {
  id: number;
  title_id: string;
  name: string;
  kind: string;
  content_type: number;
  total_size: number;
  art_path: string | null;
  description: string;
  publisher: string;
  added_at: number;
}

function toDto(row: TitleRow) {
  return {
    id: row.id,
    titleId: row.title_id,
    name: row.name,
    kind: row.kind,
    contentType: row.content_type,
    totalSize: row.total_size,
    hasArt: row.art_path !== null,
    artUrl: row.art_path ? `/api/v1/catalog/${row.id}/art` : null,
    description: row.description,
    publisher: row.publisher,
    addedAt: row.added_at,
    // ISO nao cabe no FATX; o console deve mostrar como indisponivel.
    installable: row.kind !== 'iso',
    /**
     * So pacote de conteudo aparece em "Meus Jogos" da dash original. Rip extraido
     * (pasta com default.xex) vai para Hdd1:\Games e precisa de um dashboard
     * homebrew para ser listado — no sistema original ele fica invisivel.
     */
    showsInOriginalDash: row.kind === 'god' || row.kind === 'stfs',
  };
}

export function registerCatalogRoutes(app: FastifyInstance): void {
  const guard = { preHandler: requireSession };

  app.get('/api/v1/catalog', guard, async (req) => {
    const q = (req.query ?? {}) as { q?: string; kind?: string; limit?: string; offset?: string };
    const limit = Math.min(Math.max(Number(q.limit ?? 50) || 50, 1), 200);
    const offset = Math.max(Number(q.offset ?? 0) || 0, 0);

    const where: string[] = [];
    const params: (string | number)[] = [];
    if (q.q) {
      where.push('(name LIKE ? OR title_id LIKE ?)');
      params.push(`%${q.q}%`, `%${q.q}%`);
    }
    if (q.kind) {
      where.push('kind = ?');
      params.push(q.kind);
    }
    const clause = where.length ? `WHERE ${where.join(' AND ')}` : '';

    const total = (db.prepare(`SELECT COUNT(*) AS n FROM titles ${clause}`).get(...params) as { n: number }).n;
    const rows = db
      .prepare(`SELECT * FROM titles ${clause} ORDER BY name COLLATE NOCASE LIMIT ? OFFSET ?`)
      .all(...params, limit, offset) as unknown as TitleRow[];

    return { total, limit, offset, items: rows.map(toDto) };
  });

  app.get('/api/v1/catalog/:id', guard, async (req, reply) => {
    const { id } = req.params as { id: string };
    const row = db.prepare('SELECT * FROM titles WHERE id = ?').get(Number(id)) as TitleRow | undefined;
    if (!row) return reply.code(404).send({ error: 'titulo_nao_encontrado' });

    const files = db
      .prepare('SELECT id, rel_path, install_path, size, ordinal FROM title_files WHERE title_pk = ? ORDER BY ordinal')
      .all(row.id) as { id: number; rel_path: string; install_path: string; size: number; ordinal: number }[];

    // Progresso ja registrado por este usuario, para o console mostrar "retomar".
    const progress = db
      .prepare('SELECT file_id, bytes_done, state FROM downloads WHERE user_id = ? AND title_pk = ?')
      .all(sessionOf(req).userId, row.id) as { file_id: number; bytes_done: number; state: string }[];
    const byFile = new Map(progress.map((p) => [p.file_id, p]));

    return {
      ...toDto(row),
      files: files.map((f) => ({
        id: f.id,
        installPath: f.install_path,
        size: f.size,
        ordinal: f.ordinal,
        downloadUrl: `/api/v1/download/${f.id}`,
        bytesDone: byFile.get(f.id)?.bytes_done ?? 0,
        state: byFile.get(f.id)?.state ?? 'queued',
      })),
    };
  });

  app.get('/api/v1/catalog/:id/art', guard, async (req, reply) => {
    const { id } = req.params as { id: string };
    const row = db.prepare('SELECT art_path FROM titles WHERE id = ?').get(Number(id)) as
      | { art_path: string | null }
      | undefined;
    if (!row?.art_path) return reply.code(404).send({ error: 'sem_arte' });

    try {
      const png = await readFile(row.art_path);
      return reply.type('image/png').header('Cache-Control', 'public, max-age=86400').send(png);
    } catch {
      return reply.code(404).send({ error: 'sem_arte' });
    }
  });

  /** Revarredura sob demanda. So admin: percorrer a biblioteca inteira custa I/O. */
  app.post('/api/v1/catalog/scan', guard, async (req, reply) => {
    if (!sessionOf(req).isAdmin) return reply.code(403).send({ error: 'apenas_admin' });
    const report = await scanLibrary();
    return report;
  });
}
