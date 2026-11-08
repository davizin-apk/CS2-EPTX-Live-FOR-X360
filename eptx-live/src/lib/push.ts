/**
 * Envio de conteudo para o console.
 *
 * O console fica na dash original o tempo todo. Quem se mexe e o servidor: conecta
 * no FTP que o plugin do DashLaunch mantem vivo por baixo do dashboard e grava os
 * arquivos direto em Content\0000000000000000\, que e onde a NXE procura jogos.
 *
 * Depois do envio a dash precisa de um reboot para reindexar: ela cacheia a lista
 * de conteudo na inicializacao e nao fica olhando o disco.
 */

import { db, now } from './db.ts';
import { FtpClient, toFtpPath } from './ftp.ts';
import { join, resolve, sep } from 'node:path';
import { config } from './config.ts';

export interface PushTarget {
  id: number;
  name: string;
  host: string;
  port: number;
  username: string;
  password: string;
}

interface JobFile {
  id: number;
  rel_path: string;
  install_path: string;
  size: number;
  root_path: string;
}

const insertJob = db.prepare(`
  INSERT INTO push_jobs (target_id, title_pk, user_id, state, bytes_total, files_total, started_at, updated_at)
  VALUES (?, ?, ?, 'queued', ?, ?, ?, ?)`);
const updateJob = db.prepare(`
  UPDATE push_jobs SET state = ?, bytes_done = ?, files_done = ?, current = ?, error = ?, updated_at = ?
  WHERE id = ?`);
const selectJob = db.prepare('SELECT * FROM push_jobs WHERE id = ?');
const selectTarget = db.prepare('SELECT * FROM push_targets WHERE id = ?');
const markTargetOk = db.prepare('UPDATE push_targets SET last_ok = ? WHERE id = ?');
const selectFiles = db.prepare(`
  SELECT f.id, f.rel_path, f.install_path, f.size, t.root_path
  FROM title_files f JOIN titles t ON t.id = f.title_pk
  WHERE f.title_pk = ? ORDER BY f.ordinal`);

/** Mesma checagem do download HTTP: nada sai da raiz da biblioteca. */
function resolveInsideLibrary(rootPath: string, relPath: string): string | null {
  const abs = resolve(join(rootPath, relPath));
  const root = resolve(config.library);
  if (abs !== root && !abs.startsWith(root + sep)) return null;
  return abs;
}

export function createPushJob(titlePk: number, targetId: number, userId: number | null): number {
  const files = selectFiles.all(titlePk) as unknown as JobFile[];
  if (files.length === 0) throw new Error('titulo sem arquivos');

  const bytesTotal = files.reduce((sum, f) => sum + f.size, 0);
  const ts = now();
  const res = insertJob.run(targetId, titlePk, userId, bytesTotal, files.length, ts, ts);
  return Number(res.lastInsertRowid);
}

/**
 * Executa o envio. Roda em segundo plano: um GOD leva dezenas de minutos no Wi-Fi
 * do 360, entao a rota HTTP responde na hora e o progresso vai para o banco.
 */
export async function runPushJob(jobId: number): Promise<void> {
  const job = selectJob.get(jobId) as { id: number; target_id: number; title_pk: number } | undefined;
  if (!job) throw new Error(`job ${jobId} nao existe`);

  const target = selectTarget.get(job.target_id) as PushTarget | undefined;
  if (!target) throw new Error(`console ${job.target_id} nao existe`);

  const files = selectFiles.all(job.title_pk) as unknown as JobFile[];
  const bytesBefore = files.map(() => 0);

  let bytesDone = 0;
  let filesDone = 0;
  let lastWrite = 0;

  const report = (state: string, current: string, error = '', force = false): void => {
    const ts = Date.now();
    // Progresso no banco no maximo 2x por segundo; a barra nao precisa de mais.
    if (!force && ts - lastWrite < 500) return;
    lastWrite = ts;
    updateJob.run(state, bytesDone, filesDone, current, error, now(), jobId);
  };

  const client = new FtpClient({
    host: target.host,
    port: target.port,
    user: target.username,
    password: target.password,
  });

  report('active', '', '', true);

  try {
    await client.connect();
    markTargetOk.run(now(), target.id);

    for (let i = 0; i < files.length; i++) {
      const file = files[i]!;
      if (!file.install_path) {
        // ISO e afins: catalogados, mas sem destino valido no console.
        throw new Error(`${file.rel_path} nao tem caminho de instalacao (formato nao instalavel)`);
      }

      const localPath = resolveInsideLibrary(file.root_path, file.rel_path);
      if (!localPath) throw new Error(`caminho fora da biblioteca: ${file.rel_path}`);

      const ftpPath = toFtpPath(file.install_path);
      const ftpDir = ftpPath.slice(0, ftpPath.lastIndexOf('/'));

      report('active', file.rel_path, '', true);
      await client.mkdirp(ftpDir);

      const result = await client.storeFile(localPath, ftpPath, file.size, (sent) => {
        bytesDone += sent - bytesBefore[i]!;
        bytesBefore[i] = sent;
        report('active', file.rel_path);
      });

      if (result === 'skipped') {
        // Ja estava la com o tamanho certo; contabiliza o arquivo inteiro.
        bytesDone += file.size - bytesBefore[i]!;
        bytesBefore[i] = file.size;
      }
      filesDone++;
      report('active', file.rel_path, '', true);
    }

    await client.quit();
    updateJob.run('done', bytesDone, filesDone, '', '', now(), jobId);
  } catch (err) {
    client.close();
    updateJob.run('failed', bytesDone, filesDone, '', (err as Error).message, now(), jobId);
    throw err;
  }
}
