/**
 * Empacota uma pasta num container STFS instalavel:
 *
 *   npm run stfs:pack -- <pasta> <saida> --title-id 584111F7 --name "Meu App"
 *
 * Depois de gerar, le o proprio resultado de volta e compara cada arquivo com a
 * origem. Sem console para testar, essa e a verificacao que resta — e ela pega
 * qualquer erro de layout, encadeamento ou tabela de hash.
 */

import { readdir, stat, open, readFile, mkdir } from 'node:fs/promises';
import { join, basename, resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { createStfsPackage, type StfsInputEntry, type PlacedEntry, type StfsMetadata } from '../lib/stfs_write.ts';
import { openStfsPackage, extractFile } from '../lib/stfs_files.ts';

function arg(name: string, fallback?: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i === -1 ? fallback : process.argv[i + 1];
}

const sourceDir = process.argv[2];
const outPath = process.argv[3];

if (!sourceDir || !outPath || sourceDir.startsWith('--')) {
  console.error('Uso: npm run stfs:pack -- <pasta> <saida> --title-id <8 hex> --name "Nome"');
  console.error('Opcional: --content-type 0xD0000  --publisher "..."  --thumb capa.png');
  process.exit(1);
}

/** Lista de entradas na ordem da tabela: cada diretorio antes dos filhos dele. */
interface Collected extends StfsInputEntry {
  absolutePath: string;
}

async function collect(dir: string, parentIndex: number, out: Collected[]): Promise<void> {
  const items = (await readdir(dir, { withFileTypes: true }))
    .sort((a, b) => a.name.localeCompare(b.name));

  for (const item of items) {
    const abs = join(dir, item.name);
    if (item.isDirectory()) {
      const index = out.length;
      out.push({ name: item.name, isDirectory: true, parentIndex, size: 0, absolutePath: abs });
      await collect(abs, index, out);
    } else if (item.isFile()) {
      const info = await stat(abs);
      out.push({
        name: item.name,
        isDirectory: false,
        parentIndex,
        size: Number(info.size),
        absolutePath: abs,
      });
    }
  }
}

const entries: Collected[] = [];
await collect(resolve(sourceDir), -1, entries);

if (entries.length === 0) {
  console.error(`Pasta vazia: ${sourceDir}`);
  process.exit(1);
}

const files = entries.filter((e) => !e.isDirectory);
const totalBytes = files.reduce((sum, f) => sum + f.size, 0);

const meta: StfsMetadata = {
  magic: 'LIVE',
  titleId: (arg('title-id') ?? '00000000').toUpperCase().padStart(8, '0'),
  contentType: Number(arg('content-type') ?? '0xD0000'),
  displayName: arg('name') ?? basename(resolve(sourceDir)),
  titleName: arg('name') ?? basename(resolve(sourceDir)),
  publisher: arg('publisher') ?? 'EPTX',
  description: arg('description') ?? '',
};

const thumbPath = arg('thumb');
if (thumbPath) {
  const png = await readFile(thumbPath);
  meta.thumbnail = png;
  meta.titleThumbnail = png;
}

console.log(`Origem : ${resolve(sourceDir)}`);
console.log(`  ${entries.length - files.length} diretorios, ${files.length} arquivos, ${(totalBytes / 1024 / 1024).toFixed(1)} MB`);
console.log(`Destino: ${outPath}`);
console.log(`  TitleID ${meta.titleId}, tipo 0x${meta.contentType.toString(16).toUpperCase()}, "${meta.displayName}"\n`);

// Um handle aberto por arquivo grande seria caro; abrimos sob demanda e fechamos.
const getFileData = async (entry: PlacedEntry, offset: number, length: number): Promise<Buffer> => {
  const source = entries.find((e) => e === entry as unknown as Collected)
    ?? entries.find((e) => e.name === entry.name && e.size === entry.size && e.parentIndex === entry.parentIndex);
  if (!source) throw new Error(`nao achei a origem de ${entry.name}`);

  const fh = await open(source.absolutePath, 'r');
  try {
    const buf = Buffer.alloc(length);
    await fh.read(buf, 0, length, offset);
    return buf;
  } finally {
    await fh.close();
  }
};

await mkdir(resolve(outPath, '..'), { recursive: true }).catch(() => {});
const result = await createStfsPackage(outPath, meta, entries, getFileData);

console.log('Gerado');
console.log(`  ${result.dataBlocks} blocos de dados + ${result.hashBlocks} de hash`);
console.log(`  ${(result.size / 1024 / 1024).toFixed(1)} MB`);
console.log(`  content ID: ${result.contentId}`);

// --- verificacao: ler de volta e comparar -----------------------------------

console.log('\nVerificando o pacote gerado');
let failures = 0;

const pkg = await openStfsPackage(outPath);
const packedFiles = pkg.entries.filter((e) => !e.isDirectory);

if (packedFiles.length !== files.length) {
  failures++;
  console.log(`  FALHA numero de arquivos: gerei ${packedFiles.length}, esperado ${files.length}`);
} else {
  console.log(`  ok    ${packedFiles.length} arquivos na tabela`);
}

let mismatched = 0;
for (const packed of packedFiles) {
  const source = files.find((f) => f.name === packed.name && f.size === packed.size);
  if (!source) {
    mismatched++;
    console.log(`  FALHA ${packed.path} nao existe na origem`);
    continue;
  }
  const extracted = await extractFile(pkg.fh, pkg.volume, packed);
  const original = await readFile(source.absolutePath);
  if (createHash('sha256').update(extracted).digest('hex') !== createHash('sha256').update(original).digest('hex')) {
    mismatched++;
    console.log(`  FALHA ${packed.path}: conteudo difere da origem`);
  }
}

if (mismatched === 0) {
  console.log(`  ok    todos os arquivos conferem byte a byte com a origem`);
} else {
  failures += mismatched;
}

await pkg.close();

console.log(`\n${failures === 0 ? 'PACOTE VALIDO' : `${failures} problema(s)`}`);
process.exit(failures === 0 ? 0 : 1);
