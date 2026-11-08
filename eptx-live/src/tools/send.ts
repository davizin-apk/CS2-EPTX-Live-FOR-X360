/**
 * Envia um titulo para o console pelo terminal, sem precisar do servidor HTTP.
 *
 *   npm run send -- --list
 *   npm run send -- --consoles
 *   npm run send -- --add-console "Sala" 192.168.0.15
 *   npm run send -- --title 3 --console 1
 */

import { db } from '../lib/db.ts';
import { now } from '../lib/db.ts';
import { createPushJob, runPushJob } from '../lib/push.ts';
import { testConnection } from '../lib/ftp.ts';

const args = process.argv.slice(2);

function flag(name: string): string | null {
  const i = args.indexOf(`--${name}`);
  return i === -1 ? null : (args[i + 1] ?? '');
}
function has(name: string): boolean {
  return args.includes(`--${name}`);
}

function gb(bytes: number): string {
  return `${(bytes / 1024 ** 3).toFixed(2)} GB`;
}

if (has('list')) {
  const rows = db.prepare(`
    SELECT id, title_id, name, kind, total_size FROM titles ORDER BY name COLLATE NOCASE`).all() as
    { id: number; title_id: string; name: string; kind: string; total_size: number }[];
  console.log('Titulos na biblioteca:\n');
  for (const r of rows) {
    // A coluna que importa: se o jogo vai ou nao aparecer na dash original.
    const dash =
      r.kind === 'god' || r.kind === 'stfs' ? 'dash OK  '
      : r.kind === 'iso' ? 'ISO      '
      : 'rip      ';
    console.log(`  #${String(r.id).padEnd(4)} ${r.title_id}  ${dash} ${r.kind.padEnd(5)} ${gb(r.total_size).padStart(9)}  ${r.name}`);
  }
  console.log(`
  dash OK  pacote de conteudo: aparece em "Meus Jogos" na dash original
  rip      pasta extraida: vai para Hdd1:\\Games, a dash original NAO lista
  ISO      nao cabe no FATX (limite de 4 GB por arquivo); converta para GOD`);
  db.close();
  process.exit(0);
}

if (has('consoles')) {
  const rows = db.prepare('SELECT id, name, host, port, last_ok FROM push_targets ORDER BY name').all() as
    { id: number; name: string; host: string; port: number; last_ok: number }[];
  if (rows.length === 0) {
    console.log('Nenhum console cadastrado. Use: npm run send -- --add-console "Sala" 192.168.0.15');
  } else {
    console.log('Consoles cadastrados:\n');
    for (const r of rows) {
      const seen = r.last_ok ? new Date(r.last_ok * 1000).toLocaleString('pt-BR') : 'nunca';
      console.log(`  #${String(r.id).padEnd(4)} ${r.name.padEnd(20)} ${r.host}:${r.port}   ultimo contato: ${seen}`);
    }
  }
  db.close();
  process.exit(0);
}

if (has('add-console')) {
  const i = args.indexOf('--add-console');
  const name = args[i + 1];
  const host = args[i + 2];
  if (!name || !host) {
    console.error('Uso: npm run send -- --add-console "Nome" 192.168.0.15 [porta] [usuario] [senha]');
    process.exit(1);
  }
  const port = Number(args[i + 3] ?? 21);
  const user = args[i + 4] ?? 'xbox';
  const pass = args[i + 5] ?? 'xbox';

  console.log(`Testando ${host}:${port}...`);
  const test = await testConnection({ host, port, user, password: pass });
  if (!test.ok) {
    console.error(`Nao conectou: ${test.error}`);
    console.error('Confira se o FTP do DashLaunch esta ativo e se o console esta ligado.');
    process.exit(1);
  }

  const res = db.prepare(`
    INSERT INTO push_targets (name, host, port, username, password, last_ok, created_at)
    VALUES (?, ?, ?, ?, ?, ?, ?)`).run(name, host, port, user, pass, now(), now());
  console.log(`Console "${name}" cadastrado como #${res.lastInsertRowid}.`);
  db.close();
  process.exit(0);
}

const titleArg = flag('title');
const consoleArg = flag('console');

if (!titleArg || !consoleArg) {
  console.log(`Uso:
  npm run send -- --list                                 lista os titulos
  npm run send -- --consoles                             lista os consoles
  npm run send -- --add-console "Sala" 192.168.0.15      cadastra um console
  npm run send -- --title 3 --console 1                  envia o titulo #3 para o console #1`);
  db.close();
  process.exit(1);
}

const title = db.prepare('SELECT id, name, kind, total_size FROM titles WHERE id = ?').get(Number(titleArg)) as
  { id: number; name: string; kind: string; total_size: number } | undefined;
if (!title) {
  console.error(`Titulo #${titleArg} nao existe. Rode: npm run send -- --list`);
  process.exit(1);
}
if (title.kind === 'iso') {
  console.error(`"${title.name}" e um ISO. FATX nao aceita arquivo maior que 4 GB — converta para GOD antes.`);
  process.exit(1);
}

const target = db.prepare('SELECT id, name, host FROM push_targets WHERE id = ?').get(Number(consoleArg)) as
  { id: number; name: string; host: string } | undefined;
if (!target) {
  console.error(`Console #${consoleArg} nao existe. Rode: npm run send -- --consoles`);
  process.exit(1);
}

console.log(`Enviando "${title.name}" (${gb(title.total_size)}) para ${target.name} (${target.host})\n`);

const jobId = createPushJob(title.id, target.id, null);

// Barra de progresso simples, lendo o mesmo job que a API expoe.
const readJob = db.prepare('SELECT state, bytes_done, bytes_total, files_done, files_total, current, error FROM push_jobs WHERE id = ?');
const ticker = setInterval(() => {
  const j = readJob.get(jobId) as
    { state: string; bytes_done: number; bytes_total: number; files_done: number; files_total: number; current: string } | undefined;
  if (!j) return;
  const pct = j.bytes_total ? Math.floor((j.bytes_done / j.bytes_total) * 100) : 0;
  const bar = '#'.repeat(Math.floor(pct / 2)).padEnd(50, '.');
  process.stdout.write(`\r  [${bar}] ${String(pct).padStart(3)}%  ${j.files_done}/${j.files_total}  ${j.current.slice(0, 28).padEnd(28)}`);
}, 500);

try {
  await runPushJob(jobId);
  clearInterval(ticker);
  process.stdout.write('\n');
  console.log('\nEnvio concluido.');
  console.log('Reinicie o console: a dash cacheia a lista de conteudo e so reindexa no boot.');
  console.log(`Depois o jogo aparece em "Meus Jogos" na dash original.`);
} catch (err) {
  clearInterval(ticker);
  process.stdout.write('\n');
  console.error(`\nFalhou: ${(err as Error).message}`);
  process.exitCode = 1;
}

db.close();
