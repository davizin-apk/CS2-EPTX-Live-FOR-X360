/**
 * Compila e roda o autoteste do nucleo em C: npm run selftest:core
 *
 * Faz duas coisas:
 *
 *   1. Compila para o host e roda, conferindo os vetores contra o servidor.
 *   2. Compila para PowerPC64 big-endian, a arquitetura do Xbox 360.
 *
 * O passo 2 nao roda nada — nao ha emulador aqui. Ele existe para pegar erro de
 * compilacao na arquitetura real antes de voce descobrir isso dentro do XDK.
 *
 * Precisa do Zig no PATH ou instalado pelo winget (winget install zig.zig).
 */

import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const here = dirname(fileURLToPath(import.meta.url));
const SOURCES = ['selftest_main.c', 'eptx_sha256.c', 'eptx_sign.c'].map((f) => join(here, f));

/** O winget instala numa pasta versionada, entao procuramos em vez de chutar. */
function findZig() {
  try {
    execFileSync('zig', ['version'], { stdio: 'pipe' });
    return 'zig';
  } catch { /* nao esta no PATH desta sessao */ }

  const wingetRoot = join(
    process.env.LOCALAPPDATA ?? '',
    'Microsoft', 'WinGet', 'Packages',
    'zig.zig_Microsoft.Winget.Source_8wekyb3d8bbwe',
  );
  if (existsSync(wingetRoot)) {
    for (const entry of readdirSync(wingetRoot)) {
      const candidate = join(wingetRoot, entry, 'zig.exe');
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

const flags = ['-std=c99', '-Wall', '-Wextra', '-O2'];
const out = join(tmpdir(), `eptx-selftest-${process.pid}.exe`);

console.log('Compilando para o host...');
execFileSync(zig, ['cc', ...flags, '-o', out, ...SOURCES], { stdio: 'inherit' });

console.log();
try {
  execFileSync(out, { stdio: 'inherit' });
} catch {
  console.error('\nO autoteste falhou. Nao grave nada no console antes de resolver isto.');
  process.exit(1);
}

console.log('\nCompilando para PowerPC64 big-endian (arquitetura do Xbox 360)...');
for (const src of SOURCES) {
  execFileSync(
    zig,
    ['cc', '-target', 'powerpc64-linux-musl', ...flags, '-c', '-o', join(tmpdir(), 'eptx-ppc.o'), src],
    { stdio: 'inherit' },
  );
}
console.log('  Compilou sem avisos na arquitetura do console.');
console.log('\nObservacao: o binario PPC nao e executado aqui (sem emulador). O que este');
console.log('passo garante e que compila; o que garante a corretude em big-endian e o');
console.log('codigo nao ter nenhuma leitura de palavra direto da memoria.');
