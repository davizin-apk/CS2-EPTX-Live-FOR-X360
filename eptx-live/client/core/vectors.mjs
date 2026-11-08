/**
 * Gera os vetores de teste do autoteste em C, a partir do codigo real do servidor.
 * Se o protocolo mudar, rode isto de novo e atualize selftest.c.
 *
 *   node client/core/vectors.mjs
 */
import { canonicalString, sign, sha256Hex } from '../../src/lib/crypto.ts';
import crypto from 'node:crypto';

console.log('--- SHA-256 ---');
for (const s of ['', 'abc', 'EPTX']) console.log(JSON.stringify(s), '=', sha256Hex(s));

console.log('\n--- HMAC-SHA256 (chave = bytes 00..1f) ---');
const key = Buffer.from(Array.from({ length: 32 }, (_, i) => i));
for (const s of ['', 'abc']) {
  console.log(JSON.stringify(s), '=', crypto.createHmac('sha256', key).update(s).digest('hex'));
}

console.log('\n--- requisicao assinada, segredo = 32 bytes 0xAA ---');
const secret = 'aa'.repeat(32);
const c1 = canonicalString('GET', '/api/v1/catalog?limit=50', '1788000000', 'deadbeef', '');
console.log('canonica  =', JSON.stringify(c1));
console.log('assinatura=', sign(secret, c1));

const body = '{"titleId":2}';
const c2 = canonicalString('POST', '/api/v1/push', '1788000000', 'cafe1234', body);
console.log('\ncorpo     =', body);
console.log('canonica  =', JSON.stringify(c2));
console.log('assinatura=', sign(secret, c2));
