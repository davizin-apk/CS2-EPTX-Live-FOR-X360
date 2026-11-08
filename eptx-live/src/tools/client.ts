/**
 * Cliente de referencia do protocolo EPTX.
 *
 * Serve para testar o servidor sem console, e vale como especificacao executavel
 * para o lado C++: a ordem dos campos na string canonica e o unico detalhe que
 * precisa bater byte a byte entre os dois lados.
 */

import { randomBytes, createHmac, createHash } from 'node:crypto';

export interface Session {
  token: string;
  secret: string;
  userId: number;
  gamertag: string;
  isAdmin: boolean;
}

export class EptxClient {
  private session: Session | null = null;
  /** Diferenca entre o relogio do servidor e o nosso, em segundos. */
  private clockOffset = 0;

  private readonly baseUrl: string;

  constructor(baseUrl: string) {
    this.baseUrl = baseUrl;
  }

  get currentSession(): Session | null {
    return this.session;
  }

  setSession(session: Session): void {
    this.session = session;
  }

  /**
   * Alinha o relogio com o servidor. No console isto e obrigatorio na inicializacao:
   * um 360 com a bateria fraca pode estar anos fora, e toda assinatura falharia.
   */
  async syncClock(): Promise<number> {
    const res = await fetch(`${this.baseUrl}/api/v1/time`);
    const { serverTime } = (await res.json()) as { serverTime: number };
    this.clockOffset = serverTime - Math.floor(Date.now() / 1000);
    return this.clockOffset;
  }

  private timestamp(): string {
    return String(Math.floor(Date.now() / 1000) + this.clockOffset);
  }

  /** Requisicao sem assinatura: login, cadastro, pareamento. */
  async open(method: string, path: string, body?: unknown): Promise<Response> {
    const payload = body === undefined ? undefined : JSON.stringify(body);
    return fetch(`${this.baseUrl}${path}`, {
      method,
      headers: payload ? { 'content-type': 'application/json' } : {},
      body: payload,
    });
  }

  /**
   * Requisicao assinada. Tudo depois do login passa por aqui. Um Buffer vai como
   * esta (o content-type vem em extraHeaders); qualquer outra coisa vira JSON.
   */
  async signed(method: string, path: string, body?: unknown, extraHeaders: Record<string, string> = {}): Promise<Response> {
    if (!this.session) throw new Error('sem sessao: chame login() antes');

    const raw = Buffer.isBuffer(body);
    const payload =
      body === undefined ? Buffer.alloc(0) : Buffer.isBuffer(body) ? body : Buffer.from(JSON.stringify(body), 'utf8');
    const ts = this.timestamp();
    const nonce = randomBytes(16).toString('hex');
    const bodyHash = createHash('sha256').update(payload).digest('hex');

    // METODO \n CAMINHO \n TIMESTAMP \n NONCE \n sha256(corpo)
    const canonical = [method.toUpperCase(), path, ts, nonce, bodyHash].join('\n');
    const signature = createHmac('sha256', Buffer.from(this.session.secret, 'hex'))
      .update(canonical)
      .digest('hex');

    const headers: Record<string, string> = {
      'X-EPTX-Session': this.session.token,
      'X-EPTX-Timestamp': ts,
      'X-EPTX-Nonce': nonce,
      'X-EPTX-Signature': signature,
      ...extraHeaders,
    };
    if (body !== undefined && !raw) headers['content-type'] = 'application/json';

    return fetch(`${this.baseUrl}${path}`, {
      method,
      headers,
      body: body === undefined ? undefined : payload,
    });
  }

  async json<T>(method: string, path: string, body?: unknown): Promise<T> {
    const res = await this.signed(method, path, body);
    const text = await res.text();
    if (!res.ok) throw new Error(`${method} ${path} -> ${res.status} ${text}`);
    return JSON.parse(text) as T;
  }

  async register(gamertag: string, password: string): Promise<void> {
    const res = await this.open('POST', '/api/v1/auth/register', { gamertag, password });
    if (!res.ok && res.status !== 409) throw new Error(`register -> ${res.status} ${await res.text()}`);
  }

  async login(gamertag: string, password: string, hwId: string): Promise<Session> {
    const res = await this.open('POST', '/api/v1/auth/login', { gamertag, password, hwId });
    if (!res.ok) throw new Error(`login -> ${res.status} ${await res.text()}`);
    const data = (await res.json()) as { token: string; secret: string; userId: number; gamertag: string; isAdmin: boolean };
    this.session = data;
    return data;
  }

  /** Baixa um pedaco do arquivo. E assim que o console retoma de onde parou. */
  async downloadRange(fileId: number, start: number, end: number): Promise<Buffer> {
    const res = await this.signed('GET', `/api/v1/download/${fileId}`, undefined, {
      Range: `bytes=${start}-${end}`,
    });
    if (res.status !== 206) throw new Error(`range -> ${res.status} ${await res.text()}`);
    return Buffer.from(await res.arrayBuffer());
  }
}
