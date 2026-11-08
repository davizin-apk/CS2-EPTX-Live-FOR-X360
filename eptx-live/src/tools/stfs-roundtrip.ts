/**
 * Round-trip de um pacote STFS: npm run stfs:roundtrip -- <caminho>
 *
 * Le um pacote real, remonta a regiao de dados do zero com o gerador, e compara
 * byte a byte com o original.
 *
 * E a unica forma honesta de validar o gerador sem ter um console para testar. Se
 * as tabelas de hash que eu produzo saem identicas as que a Microsoft produziu,
 * entao posicao, encadeamento, bytes de status e ordem de hashing estao todos
 * certos — nao ha como acertar isso por acaso.
 *
 * A area de assinatura do cabecalho fica de fora da comparacao: nao temos a chave
 * privada da Microsoft e nao ha como reproduzi-la. Ela nao e validada num RGH.
 */

import { open, unlink } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import {
  openStfsPackage,
  parseVolumeDescriptor,
  blockToOffset,
  STFS_BLOCK_SIZE,
} from '../lib/stfs_files.ts';
import {
  layoutPackage,
  buildFileTable,
  writeBlocksAndHashes,
  createPackageFile,
  type StfsInputEntry,
} from '../lib/stfs_write.ts';

const target = process.argv[2];
if (!target) {
  console.error('Uso: npm run stfs:roundtrip -- "caminho/do/pacote"');
  process.exit(1);
}

let failures = 0;
function check(label: string, ok: boolean, detail = ''): void {
  if (ok) {
    console.log(`  ok    ${label}`);
  } else {
    failures++;
    console.log(`  FALHA ${label}${detail ? `  ${detail}` : ''}`);
  }
}

const pkg = await openStfsPackage(target);
const original = await open(target, 'r');
const headerBuf = Buffer.alloc(0x1000);
await original.read(headerBuf, 0, 0x1000, 0);
const volume = parseVolumeDescriptor(headerBuf);

console.log(`Pacote: ${target}`);
console.log(`  ${pkg.entries.length} entradas, ${volume.allocatedBlocks} blocos de dados\n`);

// --- 1. o layout independente bate com o original? --------------------------

const inputs: StfsInputEntry[] = pkg.entries.map((e) => ({
  name: e.name,
  isDirectory: e.isDirectory,
  parentIndex: e.parentIndex,
  size: e.size,
  updatedAt: e.updatedAt,
  accessedAt: e.accessedAt,
}));

const layout = layoutPackage(inputs);

console.log('Layout calculado do zero');
check('numero de blocos da tabela de arquivos', layout.fileTableBlocks === volume.fileTableBlockCount,
  `(calculei ${layout.fileTableBlocks}, original ${volume.fileTableBlockCount})`);
check('total de blocos de dados', layout.dataBlocks === volume.allocatedBlocks,
  `(calculei ${layout.dataBlocks}, original ${volume.allocatedBlocks})`);

let blockMismatch = 0;
let firstMismatch = '';
layout.entries.forEach((placed, i) => {
  const originalEntry = pkg.entries[i]!;
  if (originalEntry.isDirectory) return;
  if (placed.startBlock !== originalEntry.startBlock) {
    if (blockMismatch === 0) {
      firstMismatch = `${originalEntry.path}: calculei ${placed.startBlock}, original ${originalEntry.startBlock}`;
    }
    blockMismatch++;
  }
});
check('bloco inicial de cada arquivo', blockMismatch === 0, firstMismatch);

// --- 2. a tabela de arquivos gerada e igual a original? ---------------------

const generatedTable = buildFileTable(layout);
const originalTable = Buffer.alloc(layout.fileTableBlocks * STFS_BLOCK_SIZE);
for (let i = 0; i < layout.fileTableBlocks; i++) {
  const block = Buffer.alloc(STFS_BLOCK_SIZE);
  await original.read(block, 0, STFS_BLOCK_SIZE, blockToOffset(volume, volume.fileTableBlockNumber + i));
  block.copy(originalTable, i * STFS_BLOCK_SIZE);
}

console.log('\nTabela de arquivos');
if (generatedTable.equals(originalTable)) {
  check('bytes identicos aos do original', true);
} else {
  let diff = -1;
  for (let i = 0; i < originalTable.length; i++) {
    if (generatedTable[i] !== originalTable[i]) { diff = i; break; }
  }
  const entryIndex = Math.floor(diff / 0x40);
  check('bytes identicos aos do original', false,
    `primeira diferenca no byte ${diff} (entrada ${entryIndex}, campo 0x${(diff % 0x40).toString(16)})`);
  console.log(`        gerado:   ${generatedTable.subarray(entryIndex * 0x40, entryIndex * 0x40 + 0x40).toString('hex')}`);
  console.log(`        original: ${originalTable.subarray(entryIndex * 0x40, entryIndex * 0x40 + 0x40).toString('hex')}`);
}

// --- 3. remontar dados e tabelas de hash ------------------------------------

const outPath = join(tmpdir(), `eptx-roundtrip-${process.pid}.bin`);
const out = await createPackageFile(outPath);

// Alimentamos o gerador com exatamente os blocos do original: o que esta sendo
// testado e o layout e as tabelas de hash, nao a leitura dos dados.
const getBlock = async (block: number): Promise<Buffer> => {
  const buf = Buffer.alloc(STFS_BLOCK_SIZE);
  await original.read(buf, 0, STFS_BLOCK_SIZE, blockToOffset(volume, block));
  return buf;
};

console.log('\nRemontando dados e tabelas de hash...');
const result = await writeBlocksAndHashes(out, volume, layout, getBlock);

check('contagem de blocos de hash', result.hashBlocks ===
  (Number((await original.stat()).size) / STFS_BLOCK_SIZE) - volume.headerBlocks - volume.allocatedBlocks,
  `(gerei ${result.hashBlocks})`);

// --- 4. comparar a regiao de dados byte a byte ------------------------------

const regionStart = volume.headerBlocks * STFS_BLOCK_SIZE;
const originalSize = Number((await original.stat()).size);
const totalBlocks = (originalSize - regionStart) / STFS_BLOCK_SIZE;

let mismatched = 0;
let firstBad = -1;
const a = Buffer.alloc(STFS_BLOCK_SIZE);
const b = Buffer.alloc(STFS_BLOCK_SIZE);

for (let i = 0; i < totalBlocks; i++) {
  const at = regionStart + i * STFS_BLOCK_SIZE;
  await original.read(a, 0, STFS_BLOCK_SIZE, at);
  await out.read(b, 0, STFS_BLOCK_SIZE, at);
  if (!a.equals(b)) {
    if (firstBad === -1) firstBad = i;
    mismatched++;
  }
}

console.log(`\nComparacao byte a byte de ${totalBlocks} blocos`);
check('regiao de dados e tabelas identica ao original', mismatched === 0,
  mismatched > 0 ? `${mismatched} blocos diferentes, primeiro no indice ${firstBad}` : '');

if (mismatched > 0 && firstBad >= 0) {
  const at = regionStart + firstBad * STFS_BLOCK_SIZE;
  await original.read(a, 0, STFS_BLOCK_SIZE, at);
  await out.read(b, 0, STFS_BLOCK_SIZE, at);
  console.log(`        original: ${a.subarray(0, 32).toString('hex')}`);
  console.log(`        gerado:   ${b.subarray(0, 32).toString('hex')}`);
}

await out.close();
await original.close();
await pkg.close();
await unlink(outPath).catch(() => {});

console.log(`\n${failures === 0 ? 'ROUND-TRIP EXATO' : `${failures} verificacao(oes) falhou(aram)`}`);
process.exit(failures === 0 ? 0 : 1);
