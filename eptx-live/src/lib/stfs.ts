/**
 * Leitor do cabecalho STFS (os containers do Xbox 360: CON / LIVE / PIRS).
 *
 * So lemos os primeiros ~40 KB do arquivo: tudo que interessa para o catalogo
 * (nome, TitleID, tipo de conteudo, capa) vive no cabecalho, e nao ha razao para
 * tocar em um pacote GOD de 8 GB so para descobrir como ele se chama.
 *
 * Offsets do bloco de metadados (versao 0/1):
 *   0x0000 magic            'CON ' | 'LIVE' | 'PIRS'
 *   0x0344 contentType      uint32 BE
 *   0x0348 metadataVersion  uint32 BE
 *   0x034C contentSize      uint64 BE
 *   0x0360 titleId          uint32 BE
 *   0x0411 displayName      18 idiomas x 0x80 bytes, UTF-16BE
 *   0x0D11 description      idem
 *   0x1611 publisher        0x80 bytes, UTF-16BE
 *   0x1691 titleName        0x80 bytes, UTF-16BE
 *   0x1712 thumbnailSize    uint32 BE
 *   0x1716 titleThumbSize   uint32 BE
 *   0x171A thumbnail        0x4000 bytes (PNG)
 *   0x571A titleThumbnail   0x4000 bytes (PNG)
 */

import { open } from 'node:fs/promises';

export const STFS_HEADER_BYTES = 0x971a; // ate o fim do segundo thumbnail

export interface StfsHeader {
  magic: 'CON' | 'LIVE' | 'PIRS';
  contentType: number;
  /** Nome da pasta que o console usa em Content\0000000000000000\<titleId>\<aqui>\ */
  contentTypeDir: string;
  contentTypeLabel: string;
  metadataVersion: number;
  contentSize: bigint;
  titleId: string;
  displayName: string;
  titleName: string;
  publisher: string;
  description: string;
  /** PNG da capa/icone, quando presente. */
  thumbnail: Buffer | null;
}

/**
 * Rotulos apenas para exibicao. A pasta de instalacao NAO sai daqui: ela vem do
 * proprio campo contentType em hexadecimal, que e exatamente o que o console faz.
 * Assim um tipo desconhecido ainda instala no lugar certo.
 */
const CONTENT_TYPE_LABELS: Record<number, string> = {
  0x00000001: 'Saved Game',
  0x00000002: 'Marketplace Content',
  0x00007000: 'Games on Demand',
  0x00010000: 'Profile',
  0x00020000: 'Gamer Picture',
  0x00030000: 'Theme',
  0x000b0000: 'Title Update',
  0x000d0000: 'Arcade Title',
};

function utf16be(buf: Buffer): string {
  // Buffer nao decodifica UTF-16BE; invertemos os pares de bytes e lemos como LE.
  const swapped = Buffer.from(buf);
  if (swapped.length % 2 === 0) swapped.swap16();
  const s = swapped.toString('utf16le');
  // Strings do STFS sao terminadas em NUL e preenchidas ate o fim do campo.
  let end = s.length;
  for (let i = 0; i < s.length; i++) {
    if (s.charCodeAt(i) === 0) { end = i; break; }
  }
  return s.slice(0, end).trim();
}

/** Le a primeira das 18 entradas de idioma (ingles) de um bloco de nomes. */
function firstLanguage(buf: Buffer, offset: number): string {
  return utf16be(buf.subarray(offset, offset + 0x80));
}

function extractPng(buf: Buffer, offset: number, size: number): Buffer | null {
  if (size <= 0 || size > 0x4000) return null;
  const slice = buf.subarray(offset, offset + size);
  if (slice.length < 8) return null;
  // Assinatura PNG: 89 50 4E 47
  if (slice[0] !== 0x89 || slice[1] !== 0x50 || slice[2] !== 0x4e || slice[3] !== 0x47) return null;
  return Buffer.from(slice);
}

export function parseStfsHeader(buf: Buffer): StfsHeader | null {
  if (buf.length < 0x1720) return null;

  const magicRaw = buf.subarray(0, 4).toString('ascii');
  const magic =
    magicRaw === 'CON ' ? 'CON' : magicRaw === 'LIVE' ? 'LIVE' : magicRaw === 'PIRS' ? 'PIRS' : null;
  if (!magic) return null;

  const contentType = buf.readUInt32BE(0x344);
  const metadataVersion = buf.readUInt32BE(0x348);
  const contentSize = buf.readBigUInt64BE(0x34c);
  const titleId = buf.readUInt32BE(0x360).toString(16).toUpperCase().padStart(8, '0');

  const displayName = firstLanguage(buf, 0x411);
  const description = firstLanguage(buf, 0xd11);
  const publisher = utf16be(buf.subarray(0x1611, 0x1691));
  const titleName = utf16be(buf.subarray(0x1691, 0x1711));

  // Preferimos o icone do titulo (a arte do jogo) ao thumbnail do conteudo.
  let thumbnail: Buffer | null = null;
  if (buf.length >= 0x971a) {
    const titleThumbSize = buf.readUInt32BE(0x1716);
    thumbnail = extractPng(buf, 0x571a, titleThumbSize);
  }
  if (!thumbnail) {
    const thumbSize = buf.readUInt32BE(0x1712);
    thumbnail = extractPng(buf, 0x171a, thumbSize);
  }

  return {
    magic,
    contentType,
    contentTypeDir: contentType.toString(16).toUpperCase().padStart(8, '0'),
    contentTypeLabel: CONTENT_TYPE_LABELS[contentType] ?? `0x${contentType.toString(16).toUpperCase()}`,
    metadataVersion,
    contentSize,
    titleId,
    displayName,
    titleName,
    publisher,
    description,
    thumbnail,
  };
}

/** Le o cabecalho direto do disco, sem carregar o pacote inteiro. */
export async function readStfsHeader(filePath: string): Promise<StfsHeader | null> {
  let fh;
  try {
    fh = await open(filePath, 'r');
    const stat = await fh.stat();
    if (stat.size < 0x1720) return null;
    const len = Math.min(STFS_HEADER_BYTES, Number(stat.size));
    const buf = Buffer.alloc(len);
    await fh.read(buf, 0, len, 0);
    return parseStfsHeader(buf);
  } catch {
    return null;
  } finally {
    await fh?.close();
  }
}

/** Checa so os 4 bytes de magic. Barato o suficiente para rodar em toda a arvore. */
export async function isStfsFile(filePath: string): Promise<boolean> {
  let fh;
  try {
    fh = await open(filePath, 'r');
    const buf = Buffer.alloc(4);
    const { bytesRead } = await fh.read(buf, 0, 4, 0);
    if (bytesRead < 4) return false;
    const m = buf.toString('ascii');
    return m === 'CON ' || m === 'LIVE' || m === 'PIRS';
  } catch {
    return false;
  } finally {
    await fh?.close();
  }
}
