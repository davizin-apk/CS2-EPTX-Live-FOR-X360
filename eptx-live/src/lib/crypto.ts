import { createHash, createHmac, randomBytes, scryptSync, timingSafeEqual } from 'node:crypto';

const SCRYPT_N = 16384, SCRYPT_R = 8, SCRYPT_P = 1, KEYLEN = 32;

export function hashPassword(password: string): string {
  const salt = randomBytes(16);
  const hash = scryptSync(password, salt, KEYLEN, { N: SCRYPT_N, r: SCRYPT_R, p: SCRYPT_P });
  return `${salt.toString('hex')}:${hash.toString('hex')}`;
}

export function verifyPassword(password: string, stored: string): boolean {
  const [saltHex, hashHex] = stored.split(':');
  if (!saltHex || !hashHex) return false;
  const salt = Buffer.from(saltHex, 'hex');
  const expected = Buffer.from(hashHex, 'hex');
  const actual = scryptSync(password, salt, expected.length, { N: SCRYPT_N, r: SCRYPT_R, p: SCRYPT_P });
  return timingSafeEqual(actual, expected);
}

export function randomToken(bytes = 24): string {
  return randomBytes(bytes).toString('base64url');
}

export function sha256Hex(data: string | Buffer): string {
  return createHash('sha256').update(data).digest('hex');
}

/**
 * Codigo que aparece na TV. Sem 0/O/1/I/L, que sao indistinguiveis
 * na fonte do dashboard a dois metros de distancia.
 */
const CODE_ALPHABET = '23456789ABCDEFGHJKMNPQRSTUVWXYZ';
export function userCode(len = 6): string {
  const buf = randomBytes(len);
  let out = '';
  for (let i = 0; i < len; i++) out += CODE_ALPHABET[buf[i]! % CODE_ALPHABET.length];
  return out;
}

/**
 * String canonica que console e servidor assinam. Qualquer divergencia de
 * formatacao aqui quebra o login inteiro, entao mantenha os dois lados identicos:
 *
 *   METODO \n CAMINHO_COM_QUERY \n TIMESTAMP \n NONCE \n sha256hex(corpo)
 */
export function canonicalString(
  method: string,
  pathWithQuery: string,
  timestamp: string,
  nonce: string,
  body: Buffer | string,
): string {
  return [method.toUpperCase(), pathWithQuery, timestamp, nonce, sha256Hex(body)].join('\n');
}

export function sign(secret: string, canonical: string): string {
  return createHmac('sha256', Buffer.from(secret, 'hex')).update(canonical).digest('hex');
}

export function signatureMatches(secret: string, canonical: string, provided: string): boolean {
  const expected = Buffer.from(sign(secret, canonical), 'hex');
  let got: Buffer;
  try { got = Buffer.from(provided, 'hex'); } catch { return false; }
  if (got.length !== expected.length) return false;
  return timingSafeEqual(got, expected);
}
