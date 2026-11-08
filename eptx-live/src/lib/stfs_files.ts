/**
 * Leitura do conteudo de um pacote STFS: tabela de arquivos e extracao.
 *
 * O formato intercala tabelas de hash no meio dos dados, entao o bloco N nao fica
 * em headerSize + N*0x1000 — e preciso pular as tabelas que aparecem antes dele.
 * Uma tabela de nivel 0 a cada 0xAA blocos, uma de nivel 1 a cada 0xAA tabelas, e
 * assim por diante.
 *
 * Os offsets abaixo foram conferidos contra pacotes reais (um XBLA e um DLC), nao
 * deduzidos de documentacao: bloco 0 caindo exatamente no inicio da tabela de
 * arquivos e a contagem de blocos de hash fechando com o tamanho do arquivo sao as
 * duas checagens que pegam erro de formula.
 *
 * Cuidado com a ordem de bytes: o cabecalho STFS e big-endian, mas tres campos do
 * volume descriptor e quatro da entrada de arquivo sao little-endian. Nao e engano
 * de leitura — o formato e assim mesmo.
 */

import { open, type FileHandle } from 'node:fs/promises';

export const STFS_BLOCK_SIZE = 0x1000;
const BLOCKS_PER_HASH_TABLE = 0xaa;         /* 170 */
const BLOCKS_PER_LEVEL1 = 0xaa * 0xaa;      /* 28900 */

export interface StfsVolume {
  /** Quantos blocos de 0x1000 o cabecalho ocupa. */
  headerBlocks: number;
  blockSeparation: number;
  fileTableBlockCount: number;
  fileTableBlockNumber: number;
  allocatedBlocks: number;
  unallocatedBlocks: number;
}

export interface StfsEntry {
  name: string;
  /** Caminho completo dentro do pacote, com '/' de separador. */
  path: string;
  isDirectory: boolean;
  /** Blocos consecutivos no arquivo (afeta como percorrer a cadeia). */
  contiguous: boolean;
  blocksAllocated: number;
  startBlock: number;
  parentIndex: number;
  size: number;
  /** FILETIME de 32 bits do formato; guardamos cru para reproduzir no round-trip. */
  updatedAt: number;
  accessedAt: number;
}

function readInt24LE(buf: Buffer, offset: number): number {
  return buf[offset]! | (buf[offset + 1]! << 8) | (buf[offset + 2]! << 16);
}

export function parseVolumeDescriptor(header: Buffer): StfsVolume {
  const headerSize = header.readUInt32BE(0x340);
  return {
    headerBlocks: Math.ceil(headerSize / STFS_BLOCK_SIZE),
    blockSeparation: header[0x37b]!,
    // Estes tres sao little-endian, ao contrario de todo o resto do cabecalho.
    fileTableBlockCount: header.readUInt16LE(0x37c),
    fileTableBlockNumber: readInt24LE(header, 0x37e),
    allocatedBlocks: header.readUInt32BE(0x395),
    unallocatedBlocks: header.readUInt32BE(0x399),
  };
}

/**
 * Offset em bytes do bloco de dados N.
 *
 * O "+1" da base e a primeira tabela de hash, que vem logo depois do cabecalho.
 * Cada faixa de 0xAA blocos acrescenta mais uma tabela, e cada faixa de 0xAA*0xAA
 * acrescenta uma de nivel 1.
 *
 * Vale so para pacotes com tabela unica (LIVE e PIRS, que sao somente leitura).
 * Pacotes CON assinados pelo console duplicam as tabelas e usam outro espacamento;
 * veja assertSingleHashTable().
 */
export function blockToOffset(volume: StfsVolume, block: number): number {
  let adjust = 0;
  if (block >= BLOCKS_PER_HASH_TABLE) {
    adjust += Math.floor(block / BLOCKS_PER_HASH_TABLE) + 1;
  }
  if (block >= BLOCKS_PER_LEVEL1) {
    adjust += Math.floor(block / BLOCKS_PER_LEVEL1) + 1;
  }
  return (volume.headerBlocks + 1 + adjust + block) * STFS_BLOCK_SIZE;
}

/** Quantos blocos de hash existem para uma quantidade de blocos de dados. */
export function hashBlockCount(dataBlocks: number): number {
  if (dataBlocks === 0) return 0;
  const level0 = Math.ceil(dataBlocks / BLOCKS_PER_HASH_TABLE);
  // Com uma unica tabela de nivel 0, ela mesma e a tabela de topo: nao ha nivel 1.
  // Nao consegui confirmar isso contra um pacote real pequeno (nenhum a mao), entao
  // pacotes abaixo de ~700 KB usam um layout deduzido, nao verificado.
  const level1 = level0 > 1 ? Math.ceil(level0 / BLOCKS_PER_HASH_TABLE) : 0;
  const level2 = level1 > 1 ? Math.ceil(level1 / BLOCKS_PER_HASH_TABLE) : 0;
  return level0 + level1 + level2;
}

/**
 * Confere que o tamanho do arquivo bate com a formula de layout. Se nao bater, o
 * pacote usa tabelas duplicadas (CON) ou algo que ainda nao entendemos — e melhor
 * parar aqui do que extrair dados deslocados em silencio.
 */
export function assertSingleHashTable(volume: StfsVolume, fileSize: number, magic: string): void {
  const expected =
    (volume.headerBlocks + volume.allocatedBlocks + hashBlockCount(volume.allocatedBlocks)) * STFS_BLOCK_SIZE;

  if (expected !== fileSize) {
    throw new Error(
      `Layout inesperado no pacote ${magic}: o arquivo tem ${fileSize} bytes, ` +
      `mas ${volume.allocatedBlocks} blocos de dados implicam ${expected}. ` +
      `Pacotes CON com tabelas de hash duplicadas ainda nao sao suportados.`,
    );
  }
}

async function readBlock(fh: FileHandle, volume: StfsVolume, block: number): Promise<Buffer> {
  const buf = Buffer.alloc(STFS_BLOCK_SIZE);
  await fh.read(buf, 0, STFS_BLOCK_SIZE, blockToOffset(volume, block));
  return buf;
}

/**
 * Le a tabela de arquivos. Cada entrada tem 0x40 bytes:
 *
 *   0x00  nome, 0x28 bytes
 *   0x28  tamanho do nome nos 6 bits baixos; 0x80 = diretorio, 0x40 = contiguo
 *   0x29  blocos alocados      (int24 LE)
 *   0x2C  blocos               (int24 LE)
 *   0x2F  bloco inicial        (int24 LE)
 *   0x32  indice do diretorio pai (int16 BE, -1 na raiz)
 *   0x34  tamanho em bytes     (uint32 BE)
 *   0x38  data de modificacao
 *   0x3C  data de acesso
 */
export async function readFileTable(fh: FileHandle, volume: StfsVolume): Promise<StfsEntry[]> {
  const raw: Omit<StfsEntry, 'path'>[] = [];

  for (let i = 0; i < volume.fileTableBlockCount; i++) {
    const block = await readBlock(fh, volume, volume.fileTableBlockNumber + i);

    for (let offset = 0; offset + 0x40 <= block.length; offset += 0x40) {
      const flags = block[offset + 0x28]!;
      const nameLength = flags & 0x3f;
      if (nameLength === 0) continue; // entrada vazia: fim da tabela neste bloco

      raw.push({
        name: block.subarray(offset, offset + nameLength).toString('latin1'),
        isDirectory: (flags & 0x80) !== 0,
        contiguous: (flags & 0x40) !== 0,
        blocksAllocated: readInt24LE(block, offset + 0x29),
        startBlock: readInt24LE(block, offset + 0x2f),
        parentIndex: block.readInt16BE(offset + 0x32),
        size: block.readUInt32BE(offset + 0x34),
        updatedAt: block.readUInt32BE(offset + 0x38),
        accessedAt: block.readUInt32BE(offset + 0x3c),
      });
    }
  }

  // Resolve o caminho completo subindo pela cadeia de pais.
  return raw.map((entry, index) => {
    const parts: string[] = [entry.name];
    let parent = entry.parentIndex;
    let guard = 0;

    while (parent >= 0 && parent < raw.length && guard++ < 64) {
      parts.unshift(raw[parent]!.name);
      parent = raw[parent]!.parentIndex;
    }
    if (guard >= 64) {
      throw new Error(`Ciclo na cadeia de diretorios na entrada ${index} (${entry.name})`);
    }

    return { ...entry, path: parts.join('/') };
  });
}

/**
 * Extrai um arquivo. Para entradas contiguas os blocos sao consecutivos; para as
 * demais seria preciso seguir a cadeia de blocos pelas tabelas de hash, que ainda
 * nao implementamos — e o caso raro, e falhar alto e melhor que devolver lixo.
 */
export async function extractFile(fh: FileHandle, volume: StfsVolume, entry: StfsEntry): Promise<Buffer> {
  if (entry.isDirectory) throw new Error(`${entry.path} e um diretorio`);
  if (!entry.contiguous) {
    throw new Error(`${entry.path} nao e contiguo; leitura por cadeia de blocos ainda nao implementada`);
  }

  const out = Buffer.alloc(entry.size);
  let written = 0;

  for (let i = 0; i < entry.blocksAllocated && written < entry.size; i++) {
    const block = await readBlock(fh, volume, entry.startBlock + i);
    const take = Math.min(STFS_BLOCK_SIZE, entry.size - written);
    block.copy(out, written, 0, take);
    written += take;
  }

  return out;
}

export interface StfsPackage {
  fh: FileHandle;
  volume: StfsVolume;
  entries: StfsEntry[];
  close: () => Promise<void>;
}

export async function openStfsPackage(filePath: string): Promise<StfsPackage> {
  const fh = await open(filePath, 'r');
  try {
    const header = Buffer.alloc(0x1000);
    await fh.read(header, 0, 0x1000, 0);

    const magic = header.subarray(0, 4).toString('ascii');
    if (magic !== 'CON ' && magic !== 'LIVE' && magic !== 'PIRS') {
      throw new Error(`Nao e um pacote STFS (magic "${magic}")`);
    }

    const volume = parseVolumeDescriptor(header);
    const { size } = await fh.stat();
    assertSingleHashTable(volume, Number(size), magic.trim());

    const entries = await readFileTable(fh, volume);

    return {
      fh,
      volume,
      entries,
      close: async () => { await fh.close(); },
    };
  } catch (err) {
    await fh.close();
    throw err;
  }
}
