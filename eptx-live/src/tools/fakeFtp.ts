/**
 * Servidor FTP de mentira, so para teste.
 *
 * Implementa o subconjunto que o cliente de push usa — USER, PASS, TYPE, PASV,
 * MKD, SIZE, STOR, QUIT — gravando numa pasta local. Serve para exercitar o envio
 * inteiro sem ter um Xbox ligado do lado.
 *
 * Nao use isto para nada real: nao tem autenticacao de verdade nem protecao de
 * caminho alem do minimo.
 */

import { createServer, type Server, type Socket } from 'node:net';
import { createWriteStream } from 'node:fs';
import { mkdir, stat } from 'node:fs/promises';
import { join, dirname, normalize } from 'node:path';

export interface FakeFtp {
  port: number;
  close: () => Promise<void>;
}

export async function startFakeFtp(rootDir: string): Promise<FakeFtp> {
  const dataServers = new Set<Server>();

  /** "/Hdd1/Content/x" vira "<rootDir>/Hdd1/Content/x". */
  const toLocal = (ftpPath: string): string => {
    const clean = normalize(ftpPath.replace(/\\/g, '/')).replace(/^([/\\])+/, '');
    return join(rootDir, clean);
  };

  const server = createServer((socket: Socket) => {
    let buffer = '';
    /** Socket de dados prometido pelo ultimo PASV, aguardando conexao do cliente. */
    let pendingData: Promise<Socket> | null = null;

    const reply = (line: string): void => {
      socket.write(`${line}\r\n`);
    };

    const openPassive = async (): Promise<number> => {
      const dataServer = createServer();
      dataServers.add(dataServer);

      pendingData = new Promise<Socket>((resolve) => {
        dataServer.once('connection', (dataSocket) => resolve(dataSocket));
      });

      await new Promise<void>((resolve) => dataServer.listen(0, '127.0.0.1', resolve));
      const address = dataServer.address();
      if (typeof address === 'string' || address === null) throw new Error('PASV sem porta');
      return address.port;
    };

    const handle = async (raw: string): Promise<void> => {
      const [verb = '', ...rest] = raw.trim().split(' ');
      const arg = rest.join(' ');

      switch (verb.toUpperCase()) {
        case 'USER':
          return reply('331 senha, por favor');
        case 'PASS':
          return reply('230 autenticado');
        case 'TYPE':
          return reply('200 modo binario');
        case 'SYST':
          return reply('215 XBOX');

        case 'PASV': {
          const port = await openPassive();
          const p1 = Math.floor(port / 256);
          const p2 = port % 256;
          return reply(`227 Entering Passive Mode (127,0,0,1,${p1},${p2})`);
        }

        case 'MKD': {
          const local = toLocal(arg);
          try {
            await stat(local);
            return reply('550 ja existe');
          } catch {
            await mkdir(local, { recursive: true });
            return reply(`257 "${arg}" criado`);
          }
        }

        case 'SIZE': {
          try {
            const s = await stat(toLocal(arg));
            return reply(`213 ${s.size}`);
          } catch {
            return reply('550 nao encontrado');
          }
        }

        case 'STOR': {
          if (!pendingData) return reply('425 sem conexao de dados');
          const local = toLocal(arg);
          await mkdir(dirname(local), { recursive: true });

          reply('150 pode mandar');
          const dataSocket = await pendingData;
          pendingData = null;

          await new Promise<void>((resolve, reject) => {
            const out = createWriteStream(local);
            dataSocket.pipe(out);
            out.on('finish', () => resolve());
            out.on('error', reject);
            dataSocket.on('error', reject);
          });
          return reply('226 recebido');
        }

        case 'QUIT':
          reply('221 tchau');
          return void socket.end();

        default:
          return reply('502 nao implementado');
      }
    };

    socket.on('data', (chunk) => {
      buffer += chunk.toString('latin1');
      for (;;) {
        const idx = buffer.indexOf('\n');
        if (idx === -1) break;
        const line = buffer.slice(0, idx);
        buffer = buffer.slice(idx + 1);
        // Comandos sao sequenciais: so tratamos o proximo depois de responder este.
        void handle(line).catch(() => reply('451 erro interno'));
      }
    });

    socket.on('error', () => { /* cliente sumiu; nada a fazer */ });
    reply('220 FTP de teste do EPTX');
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  if (typeof address === 'string' || address === null) throw new Error('servidor sem porta');

  return {
    port: address.port,
    close: async () => {
      for (const ds of dataServers) ds.close();
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
  };
}
