/**
 * Inspeciona e valida um pacote STFS: npm run stfs -- <caminho>
 *
 * Alem de listar o conteudo, confere o SHA-1 de uma amostra de blocos contra os
 * hashes gravados nas tabelas do proprio pacote. Isso valida a formula de offset
 * de bloco de forma criptografica — se um bloco fosse lido no lugar errado, o
 * hash nao bateria.
 *
 * E o teste que vale a pena rodar depois de mexer em stfs_files.ts.
 */

import { open } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { readStfsHeader } from '../lib/stfs.ts';
import {
  openStfsPackage,
  parseVolumeDescriptor,
  blockToOffset,
  extractFile,
  STFS_BLOCK_SIZE,
} from '../lib/stfs_files.ts';

const target = process.argv[2];
if (!target) {
  console.error('Uso: npm run stfs -- "caminho/do/pacote"');
  process.exit(1);
}

const header = await readStfsHeader(target);
if (!header) {
  console.error(`Nao consegui ler o cabecalho de ${target}`);
  process.exit(1);
}

console.log(`${header.displayName || header.titleName}`);
console.log(`  magic       ${header.magic}`);
console.log(`  TitleID     ${header.titleId}`);
console.log(`  tipo        ${header.contentTypeLabel} (pasta ${header.contentTypeDir})`);
console.log(`  publisher   ${header.publisher || '(vazio)'}`);

const pkg = await openStfsPackage(target);
const files = pkg.entries.filter((e) => !e.isDirectory);
const dirs = pkg.entries.filter((e) => e.isDirectory);
const contentBytes = files.reduce((sum, f) => sum + f.size, 0);

console.log(`\nVolume`);
console.log(`  blocos de dados     ${pkg.volume.allocatedBlocks}`);
console.log(`  cabecalho           ${pkg.volume.headerBlocks} blocos`);
console.log(`  tabela de arquivos  bloco ${pkg.volume.fileTableBlockNumber}, ${pkg.volume.fileTableBlockCount} blocos`);
console.log(`  conteudo            ${dirs.length} diretorios, ${files.length} arquivos, ${(contentBytes / 1024 / 1024).toFixed(1)} MB`);

console.log(`\nMaiores arquivos`);
for (const f of [...files].sort((a, b) => b.size - a.size).slice(0, 8)) {
  console.log(`  ${String(f.size).padStart(10)}  ${f.path}`);
}

// --- validacao por hash -----------------------------------------------------

const fh = await open(target, 'r');
const headerBuf = Buffer.alloc(0x1000);
await fh.read(headerBuf, 0, 0x1000, 0);
const volume = parseVolumeDescriptor(headerBuf);

async function blockAt(index: number): Promise<Buffer> {
  const buf = Buffer.alloc(STFS_BLOCK_SIZE);
  await fh.read(buf, 0, STFS_BLOCK_SIZE, index * STFS_BLOCK_SIZE);
  return buf;
}

/** A tabela nivel 0 de um grupo fica no bloco imediatamente antes dele. */
async function storedHash(block: number): Promise<string> {
  const entry = block % 0xaa;
  const groupStart = block - entry;
  const tableIndex = blockToOffset(volume, groupStart) / STFS_BLOCK_SIZE - 1;
  const table = await blockAt(tableIndex);
  return table.subarray(entry * 0x18, entry * 0x18 + 0x14).toString('hex');
}

const last = volume.allocatedBlocks - 1;
// Inclui as fronteiras de 0xAA, que e onde uma formula errada quebra primeiro.
const sample = [0, 1, 169, 170, 171, 339, 340, 1000, 10000, last]
  .filter((b) => b >= 0 && b <= last)
  .filter((b, i, arr) => arr.indexOf(b) === i);

console.log(`\nConferindo ${sample.length} blocos contra as tabelas de hash do pacote`);
let bad = 0;

for (const block of sample) {
  const data = Buffer.alloc(STFS_BLOCK_SIZE);
  await fh.read(data, 0, STFS_BLOCK_SIZE, blockToOffset(volume, block));
  const computed = createHash('sha1').update(data).digest('hex');
  const stored = await storedHash(block);

  if (computed !== stored) {
    bad++;
    console.log(`  FALHA  bloco ${block}: calculado ${computed.slice(0, 16)}, gravado ${stored.slice(0, 16)}`);
  }
}

if (bad === 0) console.log(`  Todos conferem.`);

// Extrai o primeiro arquivo pequeno como prova de ponta a ponta.
const small = files.find((f) => f.size > 0 && f.size < 64 * 1024 && f.contiguous);
if (small) {
  const data = await extractFile(pkg.fh, pkg.volume, small);
  console.log(`\nExtracao de teste: ${small.path} (${data.length} bytes)`);
  console.log(`  primeiros bytes: ${data.subarray(0, 8).toString('hex')}`);
}

await fh.close();
await pkg.close();

if (bad > 0) process.exit(1);
