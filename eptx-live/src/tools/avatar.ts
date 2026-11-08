/**
 * Poe o avatar na conta EPTX, para a dash mostrar:
 *
 *   npm run avatar -- --user Rodrigo --pass senha --file meu-avatar.png
 *   npm run avatar -- --user Rodrigo --pass senha --remove
 *
 * O arquivo e um PNG (o corpo com fundo transparente, ate 512x512; o do 360 tem
 * 150x300) ou o GIF animado salvo no 360sona (ate 1024x1024 e 8 MB). Depois fica
 * em http://<servidor>/avatar/<gamertag>/avatar-body.png (ou .gif). Pelo navegador,
 * a pagina http://<servidor>/avatar faz o mesmo.
 *
 * --host e --port apontam para o servidor (padrao 127.0.0.1:8360).
 */

import { readFile } from 'node:fs/promises';
import { EptxClient } from './client.ts';
import { checkAvatarImage, type AvatarMime } from '../lib/png.ts';

const args = process.argv.slice(2);
function flag(name: string): string | null {
  const i = args.indexOf(`--${name}`);
  return i === -1 ? null : (args[i + 1] ?? '');
}

/** Para com uma mensagem. Sem process.exit: no Windows ele derruba o Node com conexoes do fetch ainda fechando. */
class Stop extends Error {}

async function main(): Promise<void> {
  const user = flag('user');
  const pass = flag('pass');
  const file = flag('file');
  const remove = args.includes('--remove');
  if (!user || !pass || (!file && !remove)) {
    throw new Stop('Uso: npm run avatar -- --user <conta EPTX> --pass <senha> (--file <png> | --remove)');
  }

  // Confere o arquivo antes de falar com o servidor.
  let png: Buffer | null = null;
  let mime: AvatarMime = 'image/png';
  if (file) {
    png = await readFile(file).catch(() => {
      throw new Stop(`Nao consegui ler ${file}.`);
    });
    mime = png.toString('latin1', 0, 3) === 'GIF' ? 'image/gif' : 'image/png';
    const checked = checkAvatarImage(png, mime);
    if ('error' in checked) throw new Stop(`${file} nao serve: ${JSON.stringify(checked.error)}`);
  }

  const host = flag('host') ?? '127.0.0.1';
  const port = flag('port') ?? '8360';
  const client = new EptxClient(`http://${host}:${port}`);
  try {
    await client.syncClock();
    await client.login(user, pass, 'PC-AVATAR');
  } catch (err) {
    throw new Stop(`Nao entrei no servidor ${host}:${port}: ${(err as Error).message}`);
  }

  if (remove) {
    await client.json('DELETE', '/api/v1/me/avatar');
    console.log('Avatar removido. A dash volta a mostrar o boneco padrao.');
    return;
  }

  const res = await client.signed('PUT', '/api/v1/me/avatar', png!, { 'content-type': mime });
  const text = await res.text();
  if (!res.ok) throw new Stop(`O servidor recusou: ${res.status} ${text}`);
  const { width, height } = JSON.parse(text) as { width: number; height: number };
  const gamertag = client.currentSession?.gamertag ?? user;
  console.log(`Avatar salvo na conta ${gamertag} (${width}x${height}).`);
  console.log(`Endereco: http://${host}:${port}/avatar/${encodeURIComponent(gamertag)}/avatar-body.${mime === 'image/gif' ? 'gif' : 'png'}`);
  console.log('A dash do console baixa na proxima vez que entrar, ou com Y (Atualizar).');
}

main().catch((err) => {
  console.error(err instanceof Stop ? err.message : err);
  process.exitCode = 1;
});
