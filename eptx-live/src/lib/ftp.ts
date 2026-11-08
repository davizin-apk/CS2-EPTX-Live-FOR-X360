/**
 * Cliente FTP minimo, escrito para os servidores FTP do Xbox 360.
 *
 * Nao usamos uma biblioteca pronta porque os FTPs do 360 (o do DashLaunch, o do
 * XeXMenu) sao implementacoes enxutas: nao tem MLSD, nao tem TLS, o suporte a REST
 * varia de build para build, e varias respondem coisas fora do padrao. Um cliente
 * proprio deixa explicito quais comandos a gente depende, e sao poucos:
 *
 *   USER / PASS / TYPE I / PASV / MKD / SIZE / STOR / QUIT
 *
 * Caminhos: o console expoe os dispositivos como pastas na raiz, entao
 * "Hdd1:\\Content\\..." vira "/Hdd1/Content/...".
 */

import { createConnection, type Socket } from 'node:net';
import { createReadStream } from 'node:fs';
import { pipeline } from 'node:stream/promises';
import { Transform } from 'node:stream';

export interface FtpOptions {
  host: string;
  port?: number;
  user?: string;
  password?: string;
  /** Silencio maximo tolerado no canal de controle. */
  timeoutMs?: number;
}

export interface FtpResponse {
  code: number;
  text: string;
}

/** Converte "Hdd1:\Content\x\y" no caminho FTP "/Hdd1/Content/x/y". */
export function toFtpPath(consolePath: string): string {
  const withoutColon = consolePath.replace(/^([A-Za-z0-9]+):/, '/$1');
  return withoutColon.replace(/\\/g, '/');
}

/** Erro com o codigo FTP preservado, para o chamador decidir o que e fatal. */
export class FtpError extends Error {
  readonly code: number;
  constructor(code: number, text: string) {
    super(`FTP ${code}: ${text}`);
    this.name = 'FtpError';
    this.code = code;
  }
}

export class FtpClient {
  private control: Socket | null = null;
  private buffer = '';
  private pending: ((res: FtpResponse | Error) => void)[] = [];
  private readonly opts: Required<FtpOptions>;

  constructor(opts: FtpOptions) {
    this.opts = {
      host: opts.host,
      port: opts.port ?? 21,
      user: opts.user ?? 'xbox',
      password: opts.password ?? 'xbox',
      timeoutMs: opts.timeoutMs ?? 20_000,
    };
  }

  // --- canal de controle -----------------------------------------------------

  /**
   * Uma resposta termina na linha "NNN " (espaco). "NNN-" abre resposta de varias
   * linhas e nao encerra nada — e por isso que nao da para so olhar a primeira linha.
   */
  private consume(): void {
    for (;;) {
      const idx = this.buffer.indexOf('\n');
      if (idx === -1) return;
      const line = this.buffer.slice(0, idx).replace(/\r$/, '');
      this.buffer = this.buffer.slice(idx + 1);

      const m = /^(\d{3}) (.*)$/.exec(line);
      if (!m) continue; // linha intermediaria de resposta multilinha
      const res: FtpResponse = { code: Number(m[1]), text: m[2] ?? '' };
      const waiter = this.pending.shift();
      if (waiter) waiter(res);
    }
  }

  private nextResponse(): Promise<FtpResponse> {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const i = this.pending.indexOf(handler);
        if (i !== -1) this.pending.splice(i, 1);
        reject(new Error(`FTP: sem resposta em ${this.opts.timeoutMs}ms`));
      }, this.opts.timeoutMs);

      const handler = (res: FtpResponse | Error) => {
        clearTimeout(timer);
        if (res instanceof Error) reject(res);
        else resolve(res);
      };
      this.pending.push(handler);
    });
  }

  /** Escreve um comando sem registrar waiter. Use quando voce mesmo cuidar deles. */
  private write(command: string): void {
    if (!this.control) throw new Error('FTP: nao conectado');
    this.control.write(`${command}\r\n`);
  }

  /** Envia um comando e devolve a resposta. Nao interpreta o codigo. */
  private async send(command: string): Promise<FtpResponse> {
    if (!this.control) throw new Error('FTP: nao conectado');
    const waiter = this.nextResponse();
    this.control.write(`${command}\r\n`);
    return waiter;
  }

  /** Igual a send, mas exige que o codigo esteja entre os aceitos. */
  private async expect(command: string, ...accepted: number[]): Promise<FtpResponse> {
    const res = await this.send(command);
    if (!accepted.includes(res.code)) throw new FtpError(res.code, res.text);
    return res;
  }

  async connect(): Promise<void> {
    await new Promise<void>((resolve, reject) => {
      const socket = createConnection({ host: this.opts.host, port: this.opts.port });
      const onError = (err: Error) => reject(err);

      socket.setTimeout(this.opts.timeoutMs);
      socket.once('error', onError);
      socket.once('timeout', () => reject(new Error('FTP: timeout ao conectar')));
      socket.once('connect', () => {
        socket.off('error', onError);
        socket.setTimeout(0);
        socket.on('data', (chunk) => {
          this.buffer += chunk.toString('latin1');
          this.consume();
        });
        // Quedas viram erro em quem estiver esperando, em vez de travar para sempre.
        socket.on('error', (err) => this.failAllPending(err));
        socket.on('close', () => this.failAllPending(new Error('FTP: conexao fechada')));
        this.control = socket;
        resolve();
      });
    });

    await this.nextResponse(); // banner 220
    const user = await this.expect(`USER ${this.opts.user}`, 230, 331);
    // 230 direto significa que o servidor nem pediu senha; 331 e o pedido de PASS.
    if (user.code === 331) {
      await this.expect(`PASS ${this.opts.password}`, 230, 202);
    }
    await this.expect('TYPE I', 200);
  }

  private failAllPending(err: Error): void {
    const waiters = this.pending.splice(0, this.pending.length);
    for (const w of waiters) w(err);
  }

  // --- operacoes -------------------------------------------------------------

  /**
   * Cria a arvore de diretorios. Um 550 aqui quase sempre e "ja existe", que e
   * exatamente o caso comum quando se manda o segundo arquivo de um GOD.
   */
  async mkdirp(ftpDir: string): Promise<void> {
    const parts = ftpDir.split('/').filter(Boolean);
    let path = '';
    for (const part of parts) {
      path += `/${part}`;
      const res = await this.send(`MKD ${path}`);
      if (res.code !== 257 && res.code !== 550 && res.code !== 521) {
        throw new FtpError(res.code, res.text);
      }
    }
  }

  /** Tamanho do arquivo remoto, ou null se ele nao existe. */
  async size(ftpPath: string): Promise<number | null> {
    const res = await this.send(`SIZE ${ftpPath}`);
    if (res.code !== 213) return null;
    const n = Number(res.text.trim());
    return Number.isFinite(n) ? n : null;
  }

  /** Abre a conexao de dados em modo passivo. */
  private async openDataConnection(): Promise<Socket> {
    const res = await this.expect('PASV', 227);
    // "227 Entering Passive Mode (192,168,0,10,195,80)"
    const m = /(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)/.exec(res.text);
    if (!m) throw new Error(`FTP: PASV incompreensivel: ${res.text}`);

    const port = Number(m[5]) * 256 + Number(m[6]);
    // Alguns FTPs do 360 anunciam um IP interno errado no PASV; o host do
    // controle e sempre o certo, entao ignoramos o que veio na resposta.
    const host = this.opts.host;

    return new Promise((resolve, reject) => {
      const data = createConnection({ host, port });
      data.setTimeout(this.opts.timeoutMs);
      data.once('error', reject);
      data.once('timeout', () => reject(new Error('FTP: timeout na conexao de dados')));
      data.once('connect', () => {
        data.setTimeout(0);
        data.off('error', reject);
        resolve(data);
      });
    });
  }

  /**
   * Envia um arquivo. Se o remoto ja tiver exatamente o mesmo tamanho, pula — o que
   * transforma reenviar um GOD de 8 GB numa operacao de segundos quando so faltava
   * uma parte.
   */
  async storeFile(
    localPath: string,
    ftpPath: string,
    localSize: number,
    onProgress?: (bytes: number) => void,
  ): Promise<'sent' | 'skipped'> {
    const remoteSize = await this.size(ftpPath);
    if (remoteSize === localSize) return 'skipped';

    const data = await this.openDataConnection();

    // STOR gera DUAS respostas: 150 ao abrir a transferencia e 226 ao terminar.
    // Registramos os dois waiters antes de escrever o comando — na ordem certa, e
    // sem janela para o 226 chegar antes de alguem estar esperando por ele.
    const opened = this.nextResponse();
    const finished = this.nextResponse();
    this.write(`STOR ${ftpPath}`);

    const started = await opened;
    if (started.code !== 150 && started.code !== 125) {
      data.destroy();
      throw new FtpError(started.code, started.text);
    }

    // Contamos por um Transform em vez de escutar 'data' no arquivo: um listener
    // de 'data' poe o stream em modo fluente e o pipeline perderia pedacos.
    let sent = 0;
    const counter = new Transform({
      transform(chunk, _enc, cb) {
        sent += chunk.length;
        onProgress?.(sent);
        cb(null, chunk);
      },
    });

    try {
      await pipeline(createReadStream(localPath), counter, data);
    } catch (err) {
      data.destroy();
      throw err;
    }

    const done = await finished;
    if (done.code !== 226 && done.code !== 250) throw new FtpError(done.code, done.text);
    return 'sent';
  }

  async quit(): Promise<void> {
    try {
      if (this.control) await this.send('QUIT');
    } catch { /* fechar assim mesmo */ }
    this.close();
  }

  close(): void {
    this.control?.destroy();
    this.control = null;
    this.failAllPending(new Error('FTP: cliente fechado'));
  }
}

/** Conecta, autentica e desconecta. Serve para o botao "testar console". */
export async function testConnection(opts: FtpOptions): Promise<{ ok: true } | { ok: false; error: string }> {
  const client = new FtpClient(opts);
  try {
    await client.connect();
    await client.quit();
    return { ok: true };
  } catch (err) {
    client.close();
    return { ok: false, error: (err as Error).message };
  }
}
