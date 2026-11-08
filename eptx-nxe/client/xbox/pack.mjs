/**
 * Empacota o eptx_nxe.xex como container XBLA, com a skin do FSD dentro:
 *
 *   node client/xbox/pack.mjs
 *
 * A NXE original nao abre .xex solto. Um container 0xD0000 com default.xex na raiz
 * aparece em "Meus Jogos" como qualquer jogo de Arcade, e no RGH o freeboot nao
 * confere a assinatura que fica zerada. A skin vai em Skin/ (game:\Skin para a dash):
 * main.xui, skin.xui e as imagens PNG da skin Default do Freestyle Dash.
 *
 * O empacotador e o do EPTX Live (src/tools/stfs-pack.ts), so executado: ele le a pasta
 * montada aqui e grava o pacote em client/build/xbox desta edicao.
 *
 * Os sons (Audio/*.xma, XMA2) viram PCM com o ffmpeg em Skin/Audio/*.pcm.
 *
 * Gera tambem client/build/xbox/solto/, para usar como dash do DashLaunch: o .xex com
 * a pasta Skin ao lado.
 */

import { execFileSync } from 'node:child_process';
import { copyFileSync, cpSync, existsSync, mkdirSync, readdirSync, renameSync, rmSync, statSync } from 'node:fs';
import { join, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const edition = join(here, '..', '..');
const live = process.env.EPTX_LIVE ?? 'D:/EPTX Live';
const out = join(here, '..', 'build', 'xbox');
const xex = join(out, 'eptx_nxe.xex');
const skin = join(edition, 'freestyledash', 'Skins', 'Default');
const stage = join(out, 'xbla');
const loose = join(out, 'solto');
// "EPXN": outro Title ID que o do EPTX Live, para os pacotes nao se sobrescreverem no console.
const TITLE_ID = '4550584E';

if (!existsSync(xex)) {
  console.error(`Nao achei ${xex}. Compile antes, no WSL:`);
  console.error('  wsl -d Ubuntu-24.04 -e bash "/mnt/d/EPTX-NXE-Edition/client/xbox/build.sh"');
  process.exit(1);
}
const packer = join(live, 'src', 'tools', 'stfs-pack.ts');
if (!existsSync(packer)) {
  console.error(`Nao achei o empacotador do EPTX Live em ${packer} (EPTX_LIVE=...).`);
  process.exit(1);
}

// A skin: as duas cenas e as imagens PNG (DDS e audio a dash nao usa).
function copySkin(dest) {
  mkdirSync(dest, { recursive: true });
  for (const file of ['main.xui', 'skin.xui']) copyFileSync(join(skin, file), join(dest, file));
  let count = 0;
  const walk = (dir) => {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      const abs = join(dir, entry.name);
      if (entry.isDirectory()) walk(abs);
      else if (entry.name.toLowerCase().endsWith('.png')) {
        const target = join(dest, relative(skin, abs));
        mkdirSync(dirname(target), { recursive: true });
        copyFileSync(abs, target);
        count++;
      }
    }
  };
  walk(join(skin, 'Images'));
  return count;
}

// Os efeitos sonoros: XMA2, que o xsdk nao decodifica, viram PCM de 48 kHz estereo (sound.c).
// O ffmpeg vem do PATH ou do FFMPEG; a musica dos creditos fica de fora.
function findFfmpeg() {
  if (process.env.FFMPEG) return process.env.FFMPEG;
  try {
    execFileSync('ffmpeg', ['-version'], { stdio: 'ignore' });
    return 'ffmpeg';
  } catch {}
  const winget = join(process.env.LOCALAPPDATA ?? '', 'Microsoft', 'WinGet', 'Packages');
  if (existsSync(winget))
    for (const pkg of readdirSync(winget).filter((d) => d.startsWith('Gyan.FFmpeg')))
      for (const build of readdirSync(join(winget, pkg))) {
        const exe = join(winget, pkg, build, 'bin', 'ffmpeg.exe');
        if (existsSync(exe)) return exe;
      }
  return null;
}

function convertSounds(dest) {
  const ffmpeg = findFfmpeg();
  if (!ffmpeg) {
    console.warn('ffmpeg nao encontrado: o pacote vai sem os sons do FSD.');
    return 0;
  }
  mkdirSync(dest, { recursive: true });
  let count = 0;
  for (const file of readdirSync(join(skin, 'Audio'))) {
    if (!file.toLowerCase().endsWith('.xma') || file.toLowerCase() === 'credits.xma') continue;
    const out = join(dest, file.replace(/\.xma$/i, '.pcm'));
    execFileSync(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-y', '-i', join(skin, 'Audio', file),
      '-ar', '48000', '-ac', '2', '-f', 's16le', out]);
    count++;
  }
  return count;
}

for (const dir of [stage, loose]) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  if (existsSync(join(out, 'licenses'))) cpSync(join(out, 'licenses'), join(dir, 'licenses'), { recursive: true });
}
copyFileSync(xex, join(stage, 'default.xex'));
copyFileSync(xex, join(loose, 'eptx_nxe.xex'));
const images = copySkin(join(stage, 'Skin'));
copySkin(join(loose, 'Skin'));
const sounds = convertSounds(join(stage, 'Skin', 'Audio'));
if (sounds) cpSync(join(stage, 'Skin', 'Audio'), join(loose, 'Skin', 'Audio'), { recursive: true });
console.log(`Skin: main.xui, skin.xui, ${images} imagens PNG e ${sounds} sons.`);

const temp = join(out, 'EPTX-NXE.tmp');
const log = execFileSync(
  process.execPath,
  [packer, stage, temp, '--title-id', TITLE_ID, '--content-type', '0xD0000', '--name', 'EPTX NXE Edition'],
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

console.log(`\nPacote: ${final} (${(statSync(final).size / 1048576).toFixed(1)} MB)`);
console.log(`Envie para Hdd1:\\Content\\0000000000000000\\${TITLE_ID}\\000D0000\\ e reinicie o console.`);
console.log(`Solto (DashLaunch): ${loose} -> Hdd1:\\EPTX-NXE\\`);
