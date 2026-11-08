/** Varredura manual da biblioteca, sem subir o servidor: npm run scan */

import { config } from '../lib/config.ts';
import { db } from '../lib/db.ts';
import { scanLibrary } from '../lib/scanner.ts';

const report = await scanLibrary();

console.log(`Biblioteca: ${config.library}`);
console.log(`Encontrados: ${report.found}  novos: ${report.added}  atualizados: ${report.updated}  removidos: ${report.removed}`);
console.log(`Tempo: ${report.durationMs}ms`);

if (report.skipped.length) {
  console.log(`\nIgnorados (${report.skipped.length}):`);
  for (const s of report.skipped.slice(0, 20)) console.log(`  - ${s}`);
  if (report.skipped.length > 20) console.log(`  ... e mais ${report.skipped.length - 20}`);
}

const rows = db.prepare(`
  SELECT t.title_id, t.name, t.kind, t.total_size, COUNT(f.id) AS files
  FROM titles t LEFT JOIN title_files f ON f.title_pk = t.id
  GROUP BY t.id ORDER BY t.name COLLATE NOCASE`).all() as
  { title_id: string; name: string; kind: string; total_size: number; files: number }[];

if (rows.length) {
  console.log('\nCatalogo:');
  for (const r of rows) {
    const gb = (r.total_size / 1024 ** 3).toFixed(2);
    console.log(`  ${r.title_id}  ${r.kind.padEnd(5)} ${gb.padStart(7)} GB  ${String(r.files).padStart(3)} arq  ${r.name}`);
  }
}

db.close();
