/**
 * Empacota o eptx.xex como container XBLA: npm run client:xbla
 *
 * A NXE original nao abre .xex solto. Um container 0xD0000 com default.xex na raiz
 * aparece em "Meus Jogos" como qualquer jogo de Arcade, e no RGH o freeboot nao
 * confere a assinatura que fica zerada.
 *
 * O arquivo sai com o content ID como nome, que e como os pacotes ficam no HD.
 */

import { execFileSync } from 'node:child_process';
import { copyFileSync, cpSync, existsSync, mkdirSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const out = join(here, '..', 'build', 'xbox');
const xex = join(out, 'eptx.xex');
const stage = join(out, 'xbla');
const TITLE_ID = '45505458';

if (!existsSync(xex)) {
  console.error(`Nao achei ${xex}. Compile antes, no WSL:`);
  console.error('  wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX Live/client/xbox/build.sh"');
  process.exit(1);
}

rmSync(stage, { recursive: true, force: true });
mkdirSync(stage, { recursive: true });
copyFileSync(xex, join(stage, 'default.xex'));
// MIT e OFL pedem o aviso junto de quem distribui; o build.sh junta os textos aqui.
if (existsSync(join(out, 'licenses'))) cpSync(join(out, 'licenses'), join(stage, 'licenses'), { recursive: true });

// Um pacote de um grupo so de blocos (0xAA blocos de 4 KB, ~700 KB) usa um layout
// que o README marca como deduzido, nunca conferido contra um original. Completar
// ate passar disso poe o pacote no layout que o round-trip validou.
const ONE_GROUP = 0xaa * 0x1000;
const size = statSync(xex).size;
if (size <= ONE_GROUP + 64 * 1024) {
  writeFileSync(join(stage, 'padding.bin'), Buffer.alloc(ONE_GROUP + 128 * 1024 - size));
}

const temp = join(out, 'EPTX-Live.tmp');
const log = execFileSync(
  process.execPath,
  [join(root, 'src', 'tools', 'stfs-pack.ts'), stage, temp,
    '--title-id', TITLE_ID, '--content-type', '0xD0000', '--name', 'EPTX Live'],
  { encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] },
);
process.stdout.write(log);

const contentId = /content ID: ([0-9A-Fa-f]+)/.exec(log)?.[1];
if (!contentId) {
  console.error('O empacotador nao informou o content ID.');
  process.exit(1);
}
const final = join(out, contentId.toUpperCase());
rmSync(final, { force: true });
renameSync(temp, final);

console.log(`\nPacote: ${final}`);
console.log(`Envie para Hdd1:\\Content\\0000000000000000\\${TITLE_ID}\\000D0000\\ e reinicie o console.`);
