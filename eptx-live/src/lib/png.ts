/**
 * O minimo de PNG e GIF que o avatar precisa: reconhecer o arquivo, ler o tamanho
 * e, no GIF, contar os quadros. Fica fora de routes/avatar.ts para o comando
 * `npm run avatar` nao abrir o banco.
 */

/** A dash decodifica no maximo 512x512; o corpo do avatar do 360 tem 150x300. */
export const AVATAR_MAX_SIDE = 512;
export const AVATAR_MAX_BYTES = 256 * 1024;

/**
 * O GIF animado salvo no 360sona. A dash reduz cada quadro ao entrar, entao o
 * tamanho pode ser maior que o do PNG; o limite de quadros segura a memoria do console.
 */
export const GIF_MAX_SIDE = 1024;
export const GIF_MAX_BYTES = 8 * 1024 * 1024;
export const GIF_MAX_FRAMES = 600;

export type AvatarMime = 'image/png' | 'image/gif';

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

/** Largura e altura do IHDR, ou null se nao for um PNG. */
export function pngSize(png: Buffer): { width: number; height: number } | null {
  if (png.length < 24 || !png.subarray(0, 8).equals(PNG_SIGNATURE)) return null;
  if (png.toString('latin1', 12, 16) !== 'IHDR') return null;
  return { width: png.readUInt32BE(16), height: png.readUInt32BE(20) };
}

/** Tamanho da tela e quantos quadros o GIF tem, ou null se nao for um GIF inteiro. */
export function gifInfo(gif: Buffer): { width: number; height: number; frames: number } | null {
  if (gif.length < 13) return null;
  const magic = gif.toString('latin1', 0, 6);
  if (magic !== 'GIF87a' && magic !== 'GIF89a') return null;
  const width = gif.readUInt16LE(6), height = gif.readUInt16LE(8);
  const flags = gif[10]!;
  let p = 13 + (flags & 0x80 ? 3 * (1 << ((flags & 7) + 1)) : 0);
  let frames = 0;
  // Pula os sub-blocos de dados (tamanho, bytes...) ate o terminador 0.
  const skipBlocks = (): boolean => {
    while (p < gif.length) {
      const size = gif[p++]!;
      if (size === 0) return true;
      p += size;
    }
    return false;
  };
  while (p < gif.length) {
    const kind = gif[p++]!;
    if (kind === 0x3b) return frames > 0 ? { width, height, frames } : null; // fim
    if (kind === 0x21) { // extensao
      p++;
      if (!skipBlocks()) return null;
    } else if (kind === 0x2c) { // um quadro
      if (p + 9 > gif.length) return null;
      const local = gif[p + 8]!;
      p += 9 + (local & 0x80 ? 3 * (1 << ((local & 7) + 1)) : 0);
      p++; // tamanho minimo do LZW
      if (!skipBlocks()) return null;
      frames++;
    } else {
      return null;
    }
  }
  return null;
}

/** Confere um avatar enviado. Devolve o erro para a resposta, ou o tamanho dele. */
export function checkAvatarImage(
  data: Buffer,
  mime: AvatarMime,
): { code: number; error: Record<string, unknown> } | { width: number; height: number } {
  if (mime === 'image/gif') {
    if (data.length > GIF_MAX_BYTES) return { code: 413, error: { error: 'avatar_grande_demais', maxBytes: GIF_MAX_BYTES } };
    const info = gifInfo(data);
    if (!info) return { code: 400, error: { error: 'nao_e_gif' } };
    if (info.width < 1 || info.height < 1 || info.width > GIF_MAX_SIDE || info.height > GIF_MAX_SIDE) {
      return { code: 400, error: { error: 'dimensoes_invalidas', width: info.width, height: info.height, maxSide: GIF_MAX_SIDE } };
    }
    if (info.frames > GIF_MAX_FRAMES) return { code: 400, error: { error: 'quadros_demais', frames: info.frames, maxFrames: GIF_MAX_FRAMES } };
    return { width: info.width, height: info.height };
  }
  if (data.length > AVATAR_MAX_BYTES) return { code: 413, error: { error: 'avatar_grande_demais', maxBytes: AVATAR_MAX_BYTES } };
  const size = pngSize(data);
  if (!size) return { code: 400, error: { error: 'nao_e_png' } };
  if (size.width < 1 || size.height < 1 || size.width > AVATAR_MAX_SIDE || size.height > AVATAR_MAX_SIDE) {
    return { code: 400, error: { error: 'dimensoes_invalidas', ...size, maxSide: AVATAR_MAX_SIDE } };
  }
  return size;
}
