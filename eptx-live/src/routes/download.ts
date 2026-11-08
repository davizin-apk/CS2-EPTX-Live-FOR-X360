/**
 * Entrega dos arquivos.
 *
 * Suporte a Range e obrigatorio aqui, nao opcional: um GOD tem partes de centenas
 * de MB, o Wi-Fi do 360 e fraco, e a conexao vai cair no meio. Sem Range, uma queda
 * significa recomecar do zero.
 */

import type { FastifyInstance } from 'fastify';
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import { join, resolve, sep } from 'node:path';
import { config } from '../lib/config.ts';
import { db, now } from '../lib/db.ts';
import { requireSession, sessionOf } from '../lib/session.ts';

interface FileRow {
  id: number;
  rel_path: string;
  install_path: string;
  size: number;
  root_path: string;
  title_pk: number;
  name: string;
}

const findFile = db.prepare(`
  SELECT f.id, f.rel_path, f.install_path, f.size, f.title_pk, t.root_path, t.name
  FROM title_files f JOIN titles t ON t.id = f.title_pk
  WHERE f.id = ?`);

const upsertProgress = db.prepare(`
  INSERT INTO downloads (user_id, console_id, title_pk, file_id, bytes_done, state, updated_at)
  VALUES (?, ?, ?, ?, ?, ?, ?)
  ON CONFLICT(user_id, file_id) DO UPDATE SET
    bytes_done = excluded.bytes_done,
    state      = excluded.state,
    console_id = excluded.console_id,
    updated_at = excluded.updated_at`);

/**
 * Impede que um rel_path malicioso ("..\\..\\Windows\\System32") escape da
 * biblioteca. O caminho resolvido tem que continuar debaixo da raiz configurada.
 */
function resolveInsideLibrary(rootPath: string, relPath: string): string | null {
  const abs = resolve(join(rootPath, relPath));
  const root = resolve(config.library);
  if (abs !== root && !abs.startsWith(root + sep)) return null;
  return abs;
}

/** Interpreta "bytes=inicio-fim". Devolve null se o cabecalho for invalido. */
function parseRange(header: string, size: number): { start: number; end: number } | null {
  const m = /^bytes=(\d*)-(\d*)$/.exec(header.trim());
  if (!m) return null;
  const [, rawStart, rawEnd] = m;

  let start: number, end: number;
  if (rawStart === '') {
    // "bytes=-500" = os ultimos 500 bytes.
    const suffix = Number(rawEnd);
    if (!Number.isFinite(suffix) || suffix <= 0) return null;
    start = Math.max(size - suffix, 0);
    end = size - 1;
  } else {
    start = Number(rawStart);
    end = rawEnd === '' ? size - 1 : Number(rawEnd);
  }

  if (!Number.isFinite(start) || !Number.isFinite(end)) return null;
  if (start < 0 || start >= size || end < start) return null;
  return { start, end: Math.min(end, size - 1) };
}

export function registerDownloadRoutes(app: FastifyInstance): void {
  const guard = { preHandler: requireSession };

  app.get('/api/v1/download/:fileId', guard, async (req, reply) => {
    const { fileId } = req.params as { fileId: string };
    const row = findFile.get(Number(fileId)) as FileRow | undefined;
    if (!row) return reply.code(404).send({ error: 'arquivo_nao_encontrado' });

    const abs = resolveInsideLibrary(row.root_path, row.rel_path);
    if (!abs) return reply.code(403).send({ error: 'caminho_fora_da_biblioteca' });

    let size: number;
    try {
      size = Number((await stat(abs)).size);
    } catch {
      return reply.code(410).send({ error: 'arquivo_sumiu_do_servidor', detail: 'Rode uma nova varredura.' });
    }

    reply.header('Accept-Ranges', 'bytes');
    reply.header('Content-Type', 'application/octet-stream');
    reply.header('X-EPTX-Install-Path', row.install_path);

    const rangeHeader = req.headers.range;
    if (typeof rangeHeader === 'string') {
      const range = parseRange(rangeHeader, size);
      if (!range) {
        // O Content-Type binario ja foi definido acima; este erro volta como JSON.
        return reply
          .code(416)
          .type('application/json')
          .header('Content-Range', `bytes */${size}`)
          .send({ error: 'range_invalido' });
      }
      const length = range.end - range.start + 1;
      reply.code(206);
      reply.header('Content-Range', `bytes ${range.start}-${range.end}/${size}`);
      reply.header('Content-Length', String(length));
      return reply.send(createReadStream(abs, { start: range.start, end: range.end }));
    }

    reply.header('Content-Length', String(size));
    return reply.send(createReadStream(abs));
  });

  /**
   * O console avisa quanto ja gravou. Serve para a UI dele mostrar "retomar" e para
   * o painel web acompanhar o que esta acontecendo.
   */
  app.post('/api/v1/downloads/progress', guard, async (req, reply) => {
    const { fileId, bytesDone, state } = (req.body ?? {}) as
      { fileId?: number; bytesDone?: number; state?: string };

    if (typeof fileId !== 'number' || typeof bytesDone !== 'number' || bytesDone < 0) {
      return reply.code(400).send({ error: 'campos_invalidos' });
    }
    const allowed = ['queued', 'active', 'done', 'failed'];
    const st = typeof state === 'string' && allowed.includes(state) ? state : 'active';

    const row = findFile.get(fileId) as FileRow | undefined;
    if (!row) return reply.code(404).send({ error: 'arquivo_nao_encontrado' });

    const s = sessionOf(req);
    upsertProgress.run(s.userId, s.consoleId, row.title_pk, fileId, Math.min(bytesDone, row.size), st, now());
    return { ok: true };
  });

  app.get('/api/v1/downloads', guard, async (req) => {
    const rows = db
      .prepare(`
        SELECT d.file_id, d.bytes_done, d.state, d.updated_at,
               f.size, f.install_path, t.id AS title_pk, t.name
        FROM downloads d
        JOIN title_files f ON f.id = d.file_id
        JOIN titles t ON t.id = d.title_pk
        WHERE d.user_id = ? AND d.state != 'done'
        ORDER BY d.updated_at DESC`)
      .all(sessionOf(req).userId);
    return { items: rows };
  });
}
