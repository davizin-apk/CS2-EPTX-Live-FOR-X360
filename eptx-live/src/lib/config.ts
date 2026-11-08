import { resolve } from 'node:path';

function req(name: string, fallback?: string): string {
  const v = process.env[name] ?? fallback;
  if (v === undefined) throw new Error(`Variavel de ambiente ${name} nao definida (veja .env.example)`);
  return v;
}

export const config = {
  port: Number(req('EPTX_PORT', '8360')),
  host: req('EPTX_HOST', '0.0.0.0'),
  /** Raiz da biblioteca de jogos que o scanner varre. */
  library: resolve(req('EPTX_LIBRARY', './library')),
  dbPath: resolve(req('EPTX_DB', './data/eptx.db')),
  secret: req('EPTX_SECRET', 'dev-inseguro-troque-isto'),
  /**
   * O relogio do Xbox 360 frequentemente esta errado (bateria da placa, sem NTP).
   * Tolerancia larga no timestamp assinado evita que a assinatura HMAC falhe por isso.
   */
  clockSkewSeconds: Number(req('EPTX_CLOCK_SKEW', '900')),
  /** Sessao do console expira em 30 dias sem uso. */
  sessionTtlSeconds: 60 * 60 * 24 * 30,
  /** Codigo de pareamento (o de 6 caracteres na tela da TV) vive 10 minutos. */
  deviceCodeTtlSeconds: 600,
  /** Depois desse silencio, o amigo aparece como offline. */
  presenceTimeoutSeconds: 120,
} as const;

if (config.secret === 'dev-inseguro-troque-isto') {
  console.warn('[eptx] AVISO: EPTX_SECRET esta no valor padrao. Gere um proprio antes de expor na internet.');
}
