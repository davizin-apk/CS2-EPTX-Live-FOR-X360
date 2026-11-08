/**
 * Geracao de pacotes STFS.
 *
 * O layout que produzimos e o mesmo dos pacotes LIVE/PIRS de marketplace, que e o
 * que a dash original espera encontrar:
 *
 *   [cabecalho][tabela L0 do grupo 0][170 blocos][tabela L1][tabela L0 g1][170 blocos]...
 *
 * A tabela de nivel 0 de um grupo vem imediatamente antes dos blocos dele, e cada
 * entrada tem 0x14 bytes de SHA-1 mais 4 de controle: status 0x80 e o proximo bloco
 * da cadeia em int24 big-endian, com 0xFFFFFF fechando.
 *
 * Nao assinamos nada. A area de assinatura fica zerada porque num console RGH o
 * freeboot nao valida assinatura de conteudo — e nao teriamos como assinar de todo
 * jeito, a chave e da Microsoft. Num console de fabrica isto nao instalaria.
 *
 * Escrita posicional em vez de sequencial: a tabela de hash de um grupo precisa do
 * SHA-1 de blocos que so serao escritos depois dela, entao gravamos os dados
 * primeiro, guardamos os hashes, e voltamos para preencher as tabelas.
 */

import { createHash } from 'node:crypto';
import { open, type FileHandle } from 'node:fs/promises';
import {
  STFS_BLOCK_SIZE,
  blockToOffset,
  hashBlockCount,
  type StfsVolume,
} from './stfs_files.ts';

const BLOCKS_PER_HASH_TABLE = 0xaa;
const HASH_ENTRY_SIZE = 0x18;
const FILE_ENTRY_SIZE = 0x40;
const MAX_NAME_LENGTH = 0x28;
const END_OF_CHAIN = 0xffffff;

export interface StfsInputEntry {
  /** Nome do arquivo ou diretorio, sem caminho. */
  name: string;
  isDirectory: boolean;
  /** Indice do diretorio pai na lista, ou -1 para a raiz. */
  parentIndex: number;
  /** Tamanho em bytes; 0 para diretorio. */
  size: number;
  /** Lido sob demanda, para nao segurar um pacote de GB na memoria. */
  read?: (offset: number, length: number) => Promise<Buffer>;
  /** Datas do formato; se ausentes usamos o valor abaixo. */
  updatedAt?: number;
  accessedAt?: number;
}

/** Entrada com o bloco inicial ja resolvido pelo layout. */
export interface PlacedEntry extends StfsInputEntry {
  startBlock: number;
  blockCount: number;
}

export interface StfsLayout {
  entries: PlacedEntry[];
  fileTableBlocks: number;
  dataBlocks: number;
}

function blocksFor(size: number): number {
  return size === 0 ? 0 : Math.ceil(size / STFS_BLOCK_SIZE);
}

/**
 * Decide onde cada arquivo comeca.
 *
 * A tabela de arquivos ocupa os primeiros blocos e os arquivos vem depois, na
 * ordem da lista e sem buracos. E o mesmo arranjo dos pacotes de marketplace —
 * conferido contra um XBLA real, onde o primeiro arquivo cai no bloco 9 logo apos
 * os 9 blocos da tabela.
 */
export function layoutPackage(entries: StfsInputEntry[]): StfsLayout {
  const fileTableBlocks = Math.max(
    1,
    Math.ceil((entries.length * FILE_ENTRY_SIZE) / STFS_BLOCK_SIZE),
  );

  let next = fileTableBlocks;
  const placed: PlacedEntry[] = entries.map((entry) => {
    if (entry.isDirectory) {
      return { ...entry, startBlock: 0, blockCount: 0 };
    }
    const blockCount = blocksFor(entry.size);
    const startBlock = next;
    next += blockCount;
    return { ...entry, startBlock, blockCount };
  });

  return { entries: placed, fileTableBlocks, dataBlocks: next };
}

function writeInt24LE(buf: Buffer, offset: number, value: number): void {
  buf[offset] = value & 0xff;
  buf[offset + 1] = (value >> 8) & 0xff;
  buf[offset + 2] = (value >> 16) & 0xff;
}

function writeInt24BE(buf: Buffer, offset: number, value: number): void {
  buf[offset] = (value >> 16) & 0xff;
  buf[offset + 1] = (value >> 8) & 0xff;
  buf[offset + 2] = value & 0xff;
}

/** Data padrao quando a entrada nao traz uma. O formato guarda FILETIME de 32 bits. */
const DEFAULT_TIMESTAMP = 0x2d6f0020;

/** Monta os blocos da tabela de arquivos. */
export function buildFileTable(layout: StfsLayout): Buffer {
  const table = Buffer.alloc(layout.fileTableBlocks * STFS_BLOCK_SIZE);

  layout.entries.forEach((entry, index) => {
    const at = index * FILE_ENTRY_SIZE;
    const name = Buffer.from(entry.name, 'latin1');
    if (name.length > MAX_NAME_LENGTH) {
      throw new Error(`Nome longo demais para o formato (${MAX_NAME_LENGTH} bytes): ${entry.name}`);
    }
    name.copy(table, at);

    // Bits baixos = tamanho do nome; 0x80 = diretorio; 0x40 = blocos consecutivos.
    // O 0x40 vai tambem nos diretorios: nos pacotes reais eles aparecem como 0xC3,
    // nao 0x83. Marcar so os arquivos gera uma tabela que difere da original.
    let flags = (name.length & 0x3f) | 0x40;
    if (entry.isDirectory) flags |= 0x80;
    table[at + 0x28] = flags;

    writeInt24LE(table, at + 0x29, entry.blockCount);
    writeInt24LE(table, at + 0x2c, entry.blockCount);
    writeInt24LE(table, at + 0x2f, entry.startBlock);
    table.writeInt16BE(entry.parentIndex, at + 0x32);
    table.writeUInt32BE(entry.size, at + 0x34);
    table.writeUInt32BE(entry.updatedAt ?? DEFAULT_TIMESTAMP, at + 0x38);
    table.writeUInt32BE(entry.accessedAt ?? DEFAULT_TIMESTAMP, at + 0x3c);
  });

  return table;
}

/**
 * Para cada bloco de dados, qual e o proximo bloco da mesma cadeia.
 *
 * A tabela de arquivos e uma cadeia propria (0 -> 1 -> ... -> fim), e cada arquivo
 * e outra. Blocos consecutivos ainda assim precisam do encadeamento explicito: e
 * assim que o console percorre um arquivo.
 */
export function buildChain(layout: StfsLayout): Int32Array {
  const next = new Int32Array(layout.dataBlocks).fill(END_OF_CHAIN);

  for (let i = 0; i < layout.fileTableBlocks - 1; i++) next[i] = i + 1;

  for (const entry of layout.entries) {
    if (entry.isDirectory || entry.blockCount === 0) continue;
    for (let i = 0; i < entry.blockCount - 1; i++) {
      next[entry.startBlock + i] = entry.startBlock + i + 1;
    }
  }

  return next;
}

/** Posicao, em blocos do arquivo, da tabela nivel 0 que cobre um bloco de dados. */
export function level0TableIndex(volume: StfsVolume, block: number): number {
  const groupStart = block - (block % BLOCKS_PER_HASH_TABLE);
  return blockToOffset(volume, groupStart) / STFS_BLOCK_SIZE - 1;
}

/**
 * Posicao da tabela nivel 1, que cobre ate 0xAA tabelas de nivel 0.
 *
 * Ela nao fica antes do primeiro grupo — fica depois dos dados dele, colada na
 * tabela L0 do grupo seguinte. O arranjo real e:
 *
 *   [cabecalho][L0 g0][170 blocos][L1][L0 g1][170 blocos][L0 g2]...
 *
 * Por isso ancoramos no grupo sg*0xAA+1, e nao no sg*0xAA. Ancorar no primeiro
 * grupo joga a tabela para dentro do cabecalho.
 */
export function level1TableIndex(volume: StfsVolume, superGroup: number): number {
  const anchorGroup = superGroup * BLOCKS_PER_HASH_TABLE + 1;
  const anchorBlock = anchorGroup * BLOCKS_PER_HASH_TABLE;
  return blockToOffset(volume, anchorBlock) / STFS_BLOCK_SIZE - 2;
}

export interface WriteResult {
  dataBlocks: number;
  hashBlocks: number;
  totalBytes: number;
  /** Hash da tabela de topo, que vai no volume descriptor. */
  topHash: Buffer;
}

/**
 * Grava blocos e tabelas de hash a partir do fim do cabecalho.
 *
 * getBlock devolve o conteudo do bloco de dados N ja preenchido ate 0x1000 bytes.
 */
export async function writeBlocksAndHashes(
  fh: FileHandle,
  volume: StfsVolume,
  layout: StfsLayout,
  getBlock: (block: number) => Promise<Buffer>,
): Promise<WriteResult> {
  const chain = buildChain(layout);
  const blockHashes: Buffer[] = new Array(layout.dataBlocks);

  // Passo 1: dados, guardando o SHA-1 de cada bloco para as tabelas.
  for (let block = 0; block < layout.dataBlocks; block++) {
    const data = await getBlock(block);
    if (data.length !== STFS_BLOCK_SIZE) {
      throw new Error(`bloco ${block} tem ${data.length} bytes, esperado ${STFS_BLOCK_SIZE}`);
    }
    await fh.write(data, 0, STFS_BLOCK_SIZE, blockToOffset(volume, block));
    blockHashes[block] = createHash('sha1').update(data).digest();
  }

  // Passo 2: tabelas de nivel 0, uma por grupo de 0xAA blocos.
  const groups = Math.ceil(layout.dataBlocks / BLOCKS_PER_HASH_TABLE);
  const level0Hashes: Buffer[] = new Array(groups);

  for (let group = 0; group < groups; group++) {
    const table = Buffer.alloc(STFS_BLOCK_SIZE);
    const first = group * BLOCKS_PER_HASH_TABLE;

    for (let i = 0; i < BLOCKS_PER_HASH_TABLE; i++) {
      const block = first + i;
      if (block >= layout.dataBlocks) break;
      const at = i * HASH_ENTRY_SIZE;
      blockHashes[block]!.copy(table, at);
      table[at + 0x14] = 0x80; // alocado
      writeInt24BE(table, at + 0x15, chain[block]!);
    }

    const index = level0TableIndex(volume, first);
    await fh.write(table, 0, STFS_BLOCK_SIZE, index * STFS_BLOCK_SIZE);
    level0Hashes[group] = createHash('sha1').update(table).digest();
  }

  /*
   * Passo 3: tabela de nivel 1, indexando as de nivel 0.
   *
   * Com um unico grupo nao ha nivel 1: a propria tabela de nivel 0 e o topo, e e o
   * hash dela que vai no volume descriptor. Nesse caso ela tambem carrega o total
   * de blocos no fim, como faria a tabela de topo.
   *
   * Atencao: essa variante nao pode ser conferida contra um pacote real — nao havia
   * nenhum pacote pequeno o bastante a mao. Se um pacote abaixo de ~700 KB nao
   * instalar no console, este e o primeiro lugar para olhar.
   */
  if (groups <= 1) {
    const table = Buffer.alloc(STFS_BLOCK_SIZE);
    await fh.read(table, 0, STFS_BLOCK_SIZE, level0TableIndex(volume, 0) * STFS_BLOCK_SIZE);
    table.writeUInt32BE(layout.dataBlocks, BLOCKS_PER_HASH_TABLE * HASH_ENTRY_SIZE);
    await fh.write(table, 0, STFS_BLOCK_SIZE, level0TableIndex(volume, 0) * STFS_BLOCK_SIZE);

    return {
      dataBlocks: layout.dataBlocks,
      hashBlocks: hashBlockCount(layout.dataBlocks),
      totalBytes: (volume.headerBlocks + layout.dataBlocks + hashBlockCount(layout.dataBlocks)) * STFS_BLOCK_SIZE,
      topHash: createHash('sha1').update(table).digest(),
    };
  }

  const superGroups = Math.ceil(groups / BLOCKS_PER_HASH_TABLE);
  const level1Hashes: Buffer[] = new Array(superGroups);

  for (let sg = 0; sg < superGroups; sg++) {
    const table = Buffer.alloc(STFS_BLOCK_SIZE);
    const first = sg * BLOCKS_PER_HASH_TABLE;

    for (let i = 0; i < BLOCKS_PER_HASH_TABLE; i++) {
      const group = first + i;
      if (group >= groups) break;
      // Os 4 bytes de controle ficam zerados nos niveis acima do zero: eles
      // indexam tabelas, nao blocos de dados, entao nao ha cadeia nem status de
      // alocacao para registrar. Conferido nas 170 entradas de um pacote real.
      level0Hashes[group]!.copy(table, i * HASH_ENTRY_SIZE);
    }

    // Logo apos as 0xAA entradas (0xAA * 0x18 = 0xFF0) a tabela de topo carrega o
    // total de blocos de dados do pacote. As tabelas de nivel 0 deixam isso zerado.
    table.writeUInt32BE(layout.dataBlocks, BLOCKS_PER_HASH_TABLE * HASH_ENTRY_SIZE);

    const index = level1TableIndex(volume, sg);
    await fh.write(table, 0, STFS_BLOCK_SIZE, index * STFS_BLOCK_SIZE);
    level1Hashes[sg] = createHash('sha1').update(table).digest();
  }

  if (superGroups > 1) {
    throw new Error(
      `Pacote grande demais: ${layout.dataBlocks} blocos exigem tabela de nivel 2, ainda nao implementada.`,
    );
  }

  const hashBlocks = hashBlockCount(layout.dataBlocks);
  return {
    dataBlocks: layout.dataBlocks,
    hashBlocks,
    totalBytes: (volume.headerBlocks + layout.dataBlocks + hashBlocks) * STFS_BLOCK_SIZE,
    topHash: level1Hashes[0] ?? Buffer.alloc(20),
  };
}

/** Abre o arquivo de saida, trunca e devolve o handle. */
export async function createPackageFile(path: string): Promise<FileHandle> {
  return open(path, 'w+');
}

/* ------------------------------------------------------------------------- */
/* Cabecalho                                                                  */
/* ------------------------------------------------------------------------- */

/** Tamanho declarado no campo headerSize dos pacotes de marketplace. */
const HEADER_SIZE = 0xad0e;
/** O cabecalho ocupa blocos inteiros; 0xAD0E arredonda para 0xB000. */
const HEADER_BLOCKS = Math.ceil(HEADER_SIZE / STFS_BLOCK_SIZE);

export interface StfsMetadata {
  magic: 'LIVE' | 'PIRS' | 'CON';
  /** 8 digitos hex, ex: '584111F7'. */
  titleId: string;
  /** Ex: 0xD0000 para XBLA, 0x7000 para Games on Demand. */
  contentType: number;
  displayName: string;
  titleName: string;
  publisher?: string;
  description?: string;
  /** PNG de ate 0x4000 bytes. */
  thumbnail?: Buffer;
  titleThumbnail?: Buffer;
  mediaId?: number;
  version?: number;
  baseVersion?: number;
  transferFlags?: number;
}

/** Escreve uma string em UTF-16BE num campo de tamanho fixo, preenchido com zeros. */
function writeUtf16be(buf: Buffer, offset: number, text: string, fieldBytes: number): void {
  const le = Buffer.from(text, 'utf16le');
  const be = Buffer.from(le);
  if (be.length % 2 === 0) be.swap16();
  be.copy(buf, offset, 0, Math.min(be.length, fieldBytes - 2));
}

/**
 * Monta o cabecalho completo, ja com o volume descriptor preenchido.
 *
 * O SHA-1 do cabecalho, em 0x32C, cobre de 0x344 ate o fim do bloco (0xB000) — nao
 * ate headerSize. Isso foi descoberto por forca bruta contra dois pacotes reais; a
 * diferenca entre 0xAD0E e 0xB000 e justamente o padding, que entra no hash.
 */
export function buildHeader(
  meta: StfsMetadata,
  layout: StfsLayout,
  hashBlocks: number,
  topHash: Buffer,
): Buffer {
  const header = Buffer.alloc(HEADER_BLOCKS * STFS_BLOCK_SIZE);

  header.write(meta.magic === 'CON' ? 'CON ' : meta.magic, 0, 4, 'ascii');

  /*
   * Area de assinatura (0x004-0x22B) fica zerada. Nao temos a chave privada da
   * Microsoft e nao ha como produzir uma assinatura valida. Num RGH o freeboot
   * nao verifica; num console de fabrica isto nao instalaria.
   */

  // Licenca sem restricao: vale para qualquer console.
  header.writeBigUInt64BE(0xffffffffffffffffn, 0x22c);
  header.writeUInt32BE(1, 0x234);
  header.writeUInt32BE(0, 0x238);

  header.writeUInt32BE(HEADER_SIZE, 0x340);
  header.writeUInt32BE(meta.contentType, 0x344);
  header.writeUInt32BE(2, 0x348); // metadata version

  const contentBlocks = layout.dataBlocks + hashBlocks;
  header.writeBigUInt64BE(BigInt(contentBlocks) * BigInt(STFS_BLOCK_SIZE), 0x34c);

  header.writeUInt32BE(meta.mediaId ?? 0, 0x354);
  header.writeUInt32BE(meta.version ?? 1, 0x358);
  header.writeUInt32BE(meta.baseVersion ?? 1, 0x35c);
  header.writeUInt32BE(parseInt(meta.titleId, 16), 0x360);
  header[0x364] = 0; // platform
  header[0x365] = 0; // executable type

  // Volume descriptor.
  header[0x379] = 0x24;
  header[0x37a] = 0x00;
  header[0x37b] = 0x01; // block separation
  header.writeUInt16LE(layout.fileTableBlocks, 0x37c);
  writeInt24LE(header, 0x37e, 0); // tabela de arquivos comeca no bloco 0
  topHash.copy(header, 0x381);
  header.writeUInt32BE(layout.dataBlocks, 0x395);
  header.writeUInt32BE(0, 0x399); // nenhum bloco nao alocado

  header.writeUInt32BE(0, 0x39d); // data file count
  header.writeBigUInt64BE(0n, 0x3a1); // data file combined size
  header.writeUInt32BE(0, 0x3a9); // descriptor type: STFS

  writeUtf16be(header, 0x411, meta.displayName, 0x80);
  writeUtf16be(header, 0xd11, meta.description ?? '', 0x80);
  writeUtf16be(header, 0x1611, meta.publisher ?? '', 0x80);
  writeUtf16be(header, 0x1691, meta.titleName, 0x80);

  header[0x1711] = meta.transferFlags ?? 0xc0;

  if (meta.thumbnail) {
    if (meta.thumbnail.length > 0x4000) throw new Error('thumbnail maior que 0x4000 bytes');
    header.writeUInt32BE(meta.thumbnail.length, 0x1712);
    meta.thumbnail.copy(header, 0x171a);
  }
  if (meta.titleThumbnail) {
    if (meta.titleThumbnail.length > 0x4000) throw new Error('titleThumbnail maior que 0x4000 bytes');
    header.writeUInt32BE(meta.titleThumbnail.length, 0x1716);
    meta.titleThumbnail.copy(header, 0x571a);
  }

  // Por ultimo, o hash de tudo que veio acima.
  const headerHash = createHash('sha1')
    .update(header.subarray(0x344, HEADER_BLOCKS * STFS_BLOCK_SIZE))
    .digest();
  headerHash.copy(header, 0x32c);

  return header;
}

export interface PackageResult {
  path: string;
  size: number;
  dataBlocks: number;
  hashBlocks: number;
  /** SHA-1 do cabecalho em hex. Os pacotes de marketplace usam isto como nome. */
  contentId: string;
}

/**
 * Gera um pacote STFS completo e instalavel.
 *
 * getFileData entrega o conteudo de uma entrada; e chamado por pedaco para nao
 * exigir que um arquivo de centenas de MB caiba na memoria.
 */
export async function createStfsPackage(
  outPath: string,
  meta: StfsMetadata,
  entries: StfsInputEntry[],
  getFileData: (entry: PlacedEntry, offset: number, length: number) => Promise<Buffer>,
): Promise<PackageResult> {
  const layout = layoutPackage(entries);
  const volume: StfsVolume = {
    headerBlocks: HEADER_BLOCKS,
    blockSeparation: 1,
    fileTableBlockCount: layout.fileTableBlocks,
    fileTableBlockNumber: 0,
    allocatedBlocks: layout.dataBlocks,
    unallocatedBlocks: 0,
  };

  const fileTable = buildFileTable(layout);

  // Mapeia bloco de dados -> entrada dona dele, para servir getBlock rapidamente.
  const owner = new Array<PlacedEntry | null>(layout.dataBlocks).fill(null);
  for (const entry of layout.entries) {
    for (let i = 0; i < entry.blockCount; i++) owner[entry.startBlock + i] = entry;
  }

  const getBlock = async (block: number): Promise<Buffer> => {
    if (block < layout.fileTableBlocks) {
      return fileTable.subarray(block * STFS_BLOCK_SIZE, (block + 1) * STFS_BLOCK_SIZE);
    }
    const entry = owner[block]!;
    const within = (block - entry.startBlock) * STFS_BLOCK_SIZE;
    const length = Math.min(STFS_BLOCK_SIZE, entry.size - within);
    const data = await getFileData(entry, within, length);

    // O ultimo bloco de um arquivo quase nunca fecha em 0x1000; o resto vai zerado.
    if (data.length === STFS_BLOCK_SIZE) return data;
    const padded = Buffer.alloc(STFS_BLOCK_SIZE);
    data.copy(padded);
    return padded;
  };

  const fh = await createPackageFile(outPath);
  try {
    const result = await writeBlocksAndHashes(fh, volume, layout, getBlock);
    const header = buildHeader(meta, layout, result.hashBlocks, result.topHash);
    await fh.write(header, 0, header.length, 0);

    return {
      path: outPath,
      size: (HEADER_BLOCKS + layout.dataBlocks + result.hashBlocks) * STFS_BLOCK_SIZE,
      dataBlocks: layout.dataBlocks,
      hashBlocks: result.hashBlocks,
      contentId: createHash('sha1')
        .update(header.subarray(0x344, HEADER_BLOCKS * STFS_BLOCK_SIZE))
        .digest('hex')
        .toUpperCase(),
    };
  } finally {
    await fh.close();
  }
}
