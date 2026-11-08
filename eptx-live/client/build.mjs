/**
 * Compila o cliente de host: npm run client:build
 *
 * Usa o Zig como compilador C. O binario sai em client/build/eptx.exe e fala com
 * o servidor pelo mesmo codigo que vai rodar no console.
 *
 * Passe --ppc para so verificar que o nucleo compila para PowerPC64 big-endian,
 * a arquitetura do Xbox 360.
 */

import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const core = join(here, 'core');
const buildDir = join(here, 'build');

const CORE_SOURCES = [
  'eptx_sha256.c',
  'eptx_sign.c',
  'eptx_json.c',
  'eptx_http.c',
  'eptx_client.c',
].map((f) => join(core, f));

function findZig() {
  try {
    execFileSync('zig', ['version'], { stdio: 'pipe' });
    return 'zig';
  } catch { /* nao esta no PATH desta sessao */ }

  const root = join(
    process.env.LOCALAPPDATA ?? '',
    'Microsoft', 'WinGet', 'Packages',
    'zig.zig_Microsoft.Winget.Source_8wekyb3d8bbwe',
  );
  if (existsSync(root)) {
    for (const entry of readdirSync(root)) {
      const candidate = join(root, entry, 'zig.exe');
      if (existsSync(candidate)) return candidate;
    }
  }
  return null;
}

const zig = findZig();
if (!zig) {
  console.error('Zig nao encontrado. Instale com:  winget install zig.zig');
  process.exit(1);
}

const FLAGS = ['-std=c99', '-Wall', '-Wextra', '-O2', `-I${core}`];

mkdirSync(buildDir, { recursive: true });

if (process.argv.includes('--ppc')) {
  console.log('Compilando o nucleo para PowerPC64 big-endian (arquitetura do Xbox 360)...');

  // _DEFAULT_SOURCE expoe gethostbyname na musl, que por padrao a esconde sob C99
  // estrito. No console esse caminho nem existe: la a resolucao de nomes e recusada
  // de proposito e o servidor e sempre um IP.
  const ppcFlags = [...FLAGS, '-D_DEFAULT_SOURCE'];
  for (const src of CORE_SOURCES) {
    execFileSync(
      zig,
      ['cc', '-target', 'powerpc64-linux-musl', ...ppcFlags, '-c', '-o', join(buildDir, 'ppc.o'), src],
      { stdio: 'inherit' },
    );
  }
  console.log('Compilou sem avisos.');
  process.exit(0);
}

const out = join(buildDir, process.platform === 'win32' ? 'eptx.exe' : 'eptx');

// ws2_32 e a WinSock do Windows; no console o equivalente vem do XNet, ligado pelo XDK.
const libs = process.platform === 'win32' ? ['-lws2_32'] : [];

execFileSync(zig, ['cc', ...FLAGS, '-o', out, join(here, 'host', 'eptx_cli.c'), ...CORE_SOURCES, ...libs], {
  stdio: 'inherit',
});

console.log(`\nCompilado: ${out}\n`);
console.log('Exemplo:');
console.log('  client/build/eptx --host 127.0.0.1 --user Rodrigo --pass senha --list');
console.log('  client/build/eptx --host 127.0.0.1 --user Rodrigo --pass senha --get 2 --out ./baixados');
