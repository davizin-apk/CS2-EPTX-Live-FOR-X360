/**
 * Teste de ponta a ponta, sem console: npm run smoke
 *
 * Monta uma biblioteca falsa com um pacote GOD sintetico (cabecalho STFS de
 * verdade, com capa PNG e duas partes de dados), sobe o servidor num banco
 * descartavel e exercita cadastro, login, catalogo, download com Range,
 * amizade, chat com long-poll e envio para o console por FTP (contra um servidor
 * FTP de mentira, que grava num HD falso).
 */

import { mkdir, writeFile, rm, stat, readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { crc32, deflateSync } from 'node:zlib';
import { pngSize } from '../lib/png.ts';
import { DEFAULT_LOOK } from '../lib/avatarLook.ts';

const ROOT = join(tmpdir(), `eptx-smoke-${process.pid}`);
const LIBRARY = join(ROOT, 'library');
/** HD falso do console: o FTP de teste grava aqui. */
const CONSOLE_HDD = join(ROOT, 'console-hdd');
const PORT = 8399;

/** Nome do arquivo de cabecalho do GOD sintetico, usado tambem nas verificacoes. */
const GOD_NAME = '4D5307E600000000000000000000000000';
const GOD_PART_A = 64 * 1024;
const GOD_PART_B = 32 * 1024;

process.env.EPTX_LIBRARY = LIBRARY;
process.env.EPTX_DB = join(ROOT, 'smoke.db');
process.env.EPTX_PORT = String(PORT);
process.env.EPTX_HOST = '127.0.0.1';
process.env.EPTX_SECRET = 'a'.repeat(64);
process.env.EPTX_LOG_LEVEL = 'warn';

const PNG_1PX = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==',
  'base64',
);

/** Um GIF animado minimo: frames quadros de 1x1, com o atraso de cada um. */
function makeGif(frames: number): Buffer {
  const parts = [Buffer.from('GIF89a', 'latin1'), Buffer.from([1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 255, 255, 255])];
  for (let i = 0; i < frames; i++) {
    parts.push(Buffer.from([0x21, 0xf9, 4, 0, 10, 0, 0, 0]));
    parts.push(Buffer.from([0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 0x01, 0]));
  }
  parts.push(Buffer.from([0x3b]));
  return Buffer.concat(parts);
}

/** Um PNG RGBA de verdade, com CRCs certos, para o teste de avatar. */
function makePng(width: number, height: number): Buffer {
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([Buffer.from(type, 'latin1'), data])) >>> 0);
    return Buffer.concat([len, Buffer.from(type, 'latin1'), data, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bits por canal
  ihdr[9] = 6; // RGBA
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y++) raw.fill(0x7f, y * (width * 4 + 1) + 1, (y + 1) * (width * 4 + 1));
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/** Escreve uma string como UTF-16BE num campo de tamanho fixo, preenchido com NUL. */
function writeUtf16be(buf: Buffer, offset: number, text: string, fieldLen: number): void {
  const le = Buffer.from(text, 'utf16le');
  const be = Buffer.from(le);
  be.swap16();
  be.copy(buf, offset, 0, Math.min(be.length, fieldLen));
}

/** Cabecalho STFS valido o bastante para o parser do catalogo. */
function buildStfsHeader(titleId: number, name: string, publisher: string, contentType: number, contentSize: bigint): Buffer {
  const buf = Buffer.alloc(0x971a);
  buf.write('CON ', 0, 'ascii');
  buf.writeUInt32BE(contentType, 0x344);
  buf.writeUInt32BE(0, 0x348);
  buf.writeBigUInt64BE(contentSize, 0x34c);
  buf.writeUInt32BE(titleId, 0x360);
  writeUtf16be(buf, 0x411, name, 0x80);
  writeUtf16be(buf, 0xd11, 'Pacote sintetico do smoke test', 0x80);
  writeUtf16be(buf, 0x1611, publisher, 0x80);
  writeUtf16be(buf, 0x1691, name, 0x80);
  buf.writeUInt32BE(PNG_1PX.length, 0x1712);
  PNG_1PX.copy(buf, 0x171a);
  buf.writeUInt32BE(PNG_1PX.length, 0x1716);
  PNG_1PX.copy(buf, 0x571a);
  return buf;
}

async function buildFixture(): Promise<void> {
  await rm(ROOT, { recursive: true, force: true });
  await mkdir(LIBRARY, { recursive: true });

  // Um GOD: arquivo de cabecalho + pasta .data com duas partes.
  const godDir = join(LIBRARY, 'Jogo Um');
  await mkdir(godDir, { recursive: true });
  const partA = Buffer.alloc(GOD_PART_A, 0xaa);
  const partB = Buffer.alloc(GOD_PART_B, 0xbb);
  await writeFile(
    join(godDir, GOD_NAME),
    buildStfsHeader(0x4d5307e6, 'Jogo de Teste EPTX', 'EPTX Homebrew', 0x7000, BigInt(partA.length + partB.length)),
  );
  await mkdir(join(godDir, `${GOD_NAME}.data`), { recursive: true });
  await writeFile(join(godDir, `${GOD_NAME}.data`, 'Data0000'), partA);
  await writeFile(join(godDir, `${GOD_NAME}.data`, 'Data0001'), partB);

  // Um XBLA avulso, sem pasta .data.
  const xblaDir = join(LIBRARY, 'Arcade');
  await mkdir(xblaDir, { recursive: true });
  await writeFile(
    join(xblaDir, '584111F7'),
    buildStfsHeader(0x584111f7, 'Arcade de Teste', 'EPTX Homebrew', 0xd0000, 1024n),
  );

  // Uma pasta homebrew com default.xex.
  const xexDir = join(LIBRARY, 'MeuHomebrew');
  await mkdir(xexDir, { recursive: true });
  await writeFile(join(xexDir, 'default.xex'), Buffer.alloc(4096, 0x11));
  await writeFile(join(xexDir, 'leiame.txt'), 'homebrew de teste');

  // Um ISO, que deve ser catalogado mas recusado no envio (limite de 4 GB do FATX).
  await writeFile(join(LIBRARY, 'Jogo Em Iso.iso'), Buffer.alloc(2048, 0xcc));
}

let failures = 0;
function check(label: string, condition: boolean, extra = ''): void {
  if (condition) {
    console.log(`  ok    ${label}`);
  } else {
    failures++;
    console.log(`  FALHA ${label} ${extra}`);
  }
}

async function waitForHealth(url: string, tries = 50): Promise<void> {
  for (let i = 0; i < tries; i++) {
    try {
      const res = await fetch(`${url}/health`);
      if (res.ok) return;
    } catch { /* ainda subindo */ }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('servidor nao subiu');
}

/**
 * No Windows o SQLite mantem -wal e -shm abertos, entao fechamos o banco antes
 * de apagar a pasta; sem isso o unlink falha com EBUSY.
 */
async function cleanup(): Promise<void> {
  try {
    const { db } = await import('../lib/db.ts');
    db.close();
  } catch { /* banco pode nem ter sido aberto */ }
  try {
    await rm(ROOT, { recursive: true, force: true });
  } catch { /* sobra de arquivo temporario nao invalida o teste */ }
}

async function main(): Promise<void> {
  await buildFixture();

  // Importado so agora, para que config.ts leia as variaveis definidas acima.
  await import('../index.ts');

  const base = `http://127.0.0.1:${PORT}`;
  await waitForHealth(base);

  const { EptxClient } = await import('./client.ts');

  console.log('\nVarredura da biblioteca');
  // O servidor varre em segundo plano depois de subir; da tempo a ela antes de contar.
  let health = { titles: 0 };
  for (let i = 0; i < 100 && health.titles < 4; i++) {
    health = await (await fetch(`${base}/health`)).json() as { titles: number };
    if (health.titles < 4) await new Promise((r) => setTimeout(r, 100));
  }
  check('encontrou os 4 titulos da fixture', health.titles === 4, `(veio ${health.titles})`);

  console.log('\nContas e sessao');
  const a = new EptxClient(base);
  await a.syncClock();
  await a.register('RodrigoEPTX', 'senha-de-teste');
  const sessionA = await a.login('RodrigoEPTX', 'senha-de-teste', 'CONSOLE-AAAA');
  check('primeiro usuario vira admin', sessionA.isAdmin === true);
  check('recebeu segredo de 32 bytes', sessionA.secret.length === 64);

  const b = new EptxClient(base);
  await b.syncClock();
  await b.register('AmigoDois', 'senha-de-teste');
  await b.login('AmigoDois', 'senha-de-teste', 'CONSOLE-BBBB');

  const me = await a.json<{ gamertag: string }>('GET', '/api/v1/me');
  check('requisicao assinada passa', me.gamertag === 'RodrigoEPTX');

  const badRes = await fetch(`${base}/api/v1/me`, {
    headers: {
      'X-EPTX-Session': sessionA.token,
      'X-EPTX-Timestamp': String(Math.floor(Date.now() / 1000)),
      'X-EPTX-Nonce': 'nonce-forjado',
      'X-EPTX-Signature': '00'.repeat(32),
    },
  });
  check('assinatura errada e rejeitada', badRes.status === 401, `(veio ${badRes.status})`);

  console.log('\nCatalogo');
  const catalog = await a.json<{ total: number; items: { id: number; name: string; kind: string; installable: boolean; hasArt: boolean }[] }>(
    'GET', '/api/v1/catalog',
  );
  check('catalogo lista 4 titulos', catalog.total === 4, `(veio ${catalog.total})`);

  const god = catalog.items.find((t) => t.kind === 'god');
  check('GOD detectado', !!god);
  check('nome lido do cabecalho STFS', god?.name === 'Jogo de Teste EPTX', `(veio "${god?.name}")`);
  check('capa extraida do cabecalho', god?.hasArt === true);
  check('pasta com default.xex virou titulo', catalog.items.some((t) => t.kind === 'xex'));

  const detail = await a.json<{ files: { id: number; size: number; installPath: string }[] }>(
    'GET', `/api/v1/catalog/${god!.id}`,
  );
  check('GOD tem cabecalho + 2 partes', detail.files.length === 3, `(veio ${detail.files.length})`);
  const dataFile = detail.files.find((f) => f.installPath.includes('.data'))!;
  check(
    'caminho de instalacao aponta para Content\\0000000000000000',
    dataFile.installPath.startsWith('Hdd1:\\Content\\0000000000000000\\4D5307E6\\00007000\\'),
    `(veio "${dataFile.installPath}")`,
  );

  const art = await a.signed('GET', `/api/v1/catalog/${god!.id}/art`);
  check('capa servida como PNG', art.headers.get('content-type')?.includes('image/png') === true);

  console.log('\nDownload com Range');
  const target = detail.files.find((f) => f.size === 64 * 1024)!;
  const first = await a.downloadRange(target.id, 0, 1023);
  check('primeiro pedaco tem 1024 bytes', first.length === 1024, `(veio ${first.length})`);
  check('conteudo do primeiro pedaco confere', first.every((byte) => byte === 0xaa));

  const resumed = await a.downloadRange(target.id, 65_000, target.size - 1);
  check('retomada do meio do arquivo funciona', resumed.length === target.size - 65_000, `(veio ${resumed.length})`);

  const bad = await a.signed('GET', `/api/v1/download/${target.id}`, undefined, { Range: 'bytes=999999999-' });
  check('Range fora do arquivo devolve 416', bad.status === 416, `(veio ${bad.status})`);

  await a.json('POST', '/api/v1/downloads/progress', { fileId: target.id, bytesDone: 1024, state: 'active' });
  const inProgress = await a.json<{ items: unknown[] }>('GET', '/api/v1/downloads');
  check('progresso fica registrado', inProgress.items.length === 1);

  console.log('\nAmigos e presenca');
  await a.json('POST', '/api/v1/friends/requests', { gamertag: 'AmigoDois' });
  const requests = await b.json<{ incoming: { id: number }[] }>('GET', '/api/v1/friends/requests');
  check('pedido chega para o outro lado', requests.incoming.length === 1);

  await b.json('POST', `/api/v1/friends/requests/${requests.incoming[0]!.id}/accept`);
  await b.json('POST', '/api/v1/presence', { state: 'online', titleId: '4D5307E6', titleName: 'Jogo de Teste EPTX' });

  const friends = await a.json<{ items: { gamertag: string; presence: { state: string; titleName: string } }[] }>(
    'GET', '/api/v1/friends',
  );
  check('amizade aceita aparece na lista', friends.items.length === 1);
  check('presenca do amigo esta online', friends.items[0]?.presence.state === 'online');
  check('mostra em que jogo o amigo esta', friends.items[0]?.presence.titleName === 'Jogo de Teste EPTX');

  console.log('\nAvatar');
  const avatarUrl = (tag: string, ext = 'png') => `${base}/avatar/${encodeURIComponent(tag)}/avatar-body.${ext}`;
  const fresh = await fetch(avatarUrl('AmigoDois'));
  check('conta nova recebe o PNG do avatar padrao', fresh.status === 200 &&
    pngSize(Buffer.from(await fresh.arrayBuffer()))?.height === 300, `(veio ${fresh.status})`);
  const meFresh = await b.json<{ avatarType: string; avatarLook: number[] | null }>('GET', '/api/v1/me');
  check('conta nova tem o look padrao', meFresh.avatarType === 'look' &&
    JSON.stringify(meFresh.avatarLook) === JSON.stringify(DEFAULT_LOOK));
  check('gamertag que nao existe da 404', (await fetch(avatarUrl('NinguemAqui'))).status === 404);

  const body = makePng(150, 300);
  const put = await b.signed('PUT', '/api/v1/me/avatar', body, { 'content-type': 'image/png' });
  check('sobe o avatar assinado', put.status === 200, `(veio ${put.status} ${put.ok ? '' : await put.text()})`);
  const got = await fetch(avatarUrl('amigodois'));
  const gotBytes = Buffer.from(await got.arrayBuffer());
  check('serve no mesmo endereco do Xbox LIVE, sem assinatura', got.status === 200 &&
    got.headers.get('content-type')?.includes('image/png') === true);
  check('PNG volta byte a byte', gotBytes.equals(body));
  // A dash pede com o mesmo cliente das rotas assinadas, entao a rota publica tem que aceitar os cabecalhos.
  const signedGet = await a.signed('GET', '/avatar/AmigoDois/avatar-body.png');
  check('rota publica aceita pedido assinado, como o da dash', signedGet.status === 200, `(veio ${signedGet.status})`);
  const friendsWithAvatar = await a.json<{ items: { avatarUpdated: number }[] }>('GET', '/api/v1/friends');
  check('lista de amigos diz quando o avatar mudou', (friendsWithAvatar.items[0]?.avatarUpdated ?? 0) > 0);

  const notPng = await b.signed('PUT', '/api/v1/me/avatar', Buffer.from('nao sou png'), { 'content-type': 'image/png' });
  check('recusa o que nao e PNG', notPng.status === 400, `(veio ${notPng.status})`);
  const huge = await b.signed('PUT', '/api/v1/me/avatar', makePng(1024, 64), { 'content-type': 'image/png' });
  check('recusa avatar maior que a dash decodifica', huge.status === 400, `(veio ${huge.status})`);
  const unsigned = await fetch(`${base}/api/v1/me/avatar`, {
    method: 'PUT', headers: { 'content-type': 'image/png' }, body,
  });
  check('upload sem assinatura e recusado', unsigned.status === 401, `(veio ${unsigned.status})`);

  // O GIF animado do 360sona: dois quadros de 1x1.
  const gif = makeGif(2);
  const putGif = await b.signed('PUT', '/api/v1/me/avatar', gif, { 'content-type': 'image/gif' });
  check('sobe o GIF animado', putGif.status === 200, `(veio ${putGif.status} ${putGif.ok ? '' : await putGif.text()})`);
  const gotGif = await fetch(avatarUrl('AmigoDois', 'gif'));
  check('serve o GIF byte a byte', gotGif.status === 200 && gotGif.headers.get('content-type') === 'image/gif' &&
    Buffer.from(await gotGif.arrayBuffer()).equals(gif));
  const pngWithGif = await fetch(avatarUrl('AmigoDois'));
  check('com GIF, o .png cai no desenhado', pngWithGif.status === 200 &&
    pngSize(Buffer.from(await pngWithGif.arrayBuffer()))?.height === 300);
  const friendsGif = await a.json<{ items: { avatarType: string; avatarPhoto: string | null }[] }>('GET', '/api/v1/friends');
  check('amigos dizem que o avatar e GIF', friendsGif.items[0]?.avatarType === 'gif' && friendsGif.items[0]?.avatarPhoto === 'gif');
  const brokenGif = await b.signed('PUT', '/api/v1/me/avatar', gif.subarray(0, gif.length - 6), { 'content-type': 'image/gif' });
  check('recusa GIF cortado', brokenGif.status === 400, `(veio ${brokenGif.status})`);

  // O editor da dash: escolhas mais o corpo em PNG.
  const look = [...DEFAULT_LOOK];
  look[0] = 4;
  const saved = await b.signed('POST', '/api/v1/me/avatar/look', { look, png: makePng(150, 300).toString('base64') });
  check('salva o look do editor', saved.status === 200, `(veio ${saved.status})`);
  const meLook = await b.json<{ avatarType: string; avatarLook: number[]; avatarPhoto: string | null }>('GET', '/api/v1/me');
  check('/me devolve o look salvo', meLook.avatarType === 'look' && meLook.avatarLook[0] === 4);
  check('salvar o desenhado guarda a foto', meLook.avatarPhoto === 'gif');
  check('o GIF continua no ar', (await fetch(avatarUrl('AmigoDois', 'gif'))).status === 200);

  // O perfil da dash troca entre os dois.
  const toPhoto = await b.json<{ avatarType: string }>('POST', '/api/v1/me/avatar/use', { use: 'photo' });
  check('troca para a foto', toPhoto.avatarType === 'gif');
  const toLook = await b.json<{ avatarType: string; avatarLook: number[] }>('POST', '/api/v1/me/avatar/use', { use: 'look' });
  check('troca de volta para o desenhado', toLook.avatarType === 'look' && toLook.avatarLook[0] === 4);
  const badUse = await b.signed('POST', '/api/v1/me/avatar/use', { use: 'outro' });
  check('recusa troca torta', badUse.status === 400, `(veio ${badUse.status})`);
  const badLook = await b.signed('POST', '/api/v1/me/avatar/look', { look: [1, 2], png: '' });
  check('recusa look torto', badLook.status === 400, `(veio ${badLook.status})`);

  await b.json('DELETE', '/api/v1/me/avatar');
  const meReset = await b.json<{ avatarUpdated: number; avatarType: string }>('GET', '/api/v1/me');
  check('remover volta ao avatar padrao', meReset.avatarUpdated === 0 && meReset.avatarType === 'look');
  const noPhoto = await b.signed('POST', '/api/v1/me/avatar/use', { use: 'photo' });
  check('sem foto, nao troca para a foto', noPhoto.status === 409, `(veio ${noPhoto.status})`);

  console.log('\nChat com long-poll');
  const dm = await a.json<{ conversationId: number }>('POST', '/api/v1/chat/dm', { gamertag: 'AmigoDois' });

  // B fica pendurado esperando; A manda logo depois. O poll tem que voltar na hora.
  const started = Date.now();
  const polling = b.json<{ items: { body: string; sender: string }[] }>(
    'GET', `/api/v1/chat/${dm.conversationId}/messages?since=0&wait=10`,
  );
  await new Promise((r) => setTimeout(r, 200));
  await a.json('POST', `/api/v1/chat/${dm.conversationId}/messages`, { body: 'e ai, bora jogar?' });

  const polled = await polling;
  const elapsed = Date.now() - started;
  check('long-poll acorda com mensagem nova', polled.items.length === 1);
  check('mensagem chega com o remetente certo', polled.items[0]?.sender === 'RodrigoEPTX');
  check('acordou rapido, sem esperar o timeout', elapsed < 3000, `(levou ${elapsed}ms)`);

  const convos = await b.json<{ items: { unread: number }[] }>('GET', '/api/v1/chat/conversations');
  check('conversa aparece na lista do destinatario', convos.items.length === 1);

  console.log('\nEnvio para o console por FTP');
  const { startFakeFtp } = await import('./fakeFtp.ts');
  await mkdir(CONSOLE_HDD, { recursive: true });
  const ftp = await startFakeFtp(CONSOLE_HDD);

  const consoleTarget = await a.json<{ id: number }>('POST', '/api/v1/targets', {
    name: 'Console da Sala', host: '127.0.0.1', port: ftp.port, username: 'xbox', password: 'xbox',
  });
  const conn = await a.json<{ ok: boolean; error?: string }>('POST', `/api/v1/targets/${consoleTarget.id}/test`);
  check('conecta e autentica no FTP do console', conn.ok === true, conn.error ?? '');

  const targets = await a.json<{ items: Record<string, unknown>[] }>('GET', '/api/v1/targets');
  check('senha do FTP nao vaza na listagem', !('password' in (targets.items[0] ?? {})));

  interface Job { state: string; bytes_done: number; bytes_total: number; files_done: number; error: string }
  const waitForJob = async (jobId: number): Promise<Job> => {
    for (let i = 0; i < 200; i++) {
      const job = await a.json<Job>('GET', `/api/v1/push/${jobId}`);
      if (job.state === 'done' || job.state === 'failed') return job;
      await new Promise((r) => setTimeout(r, 50));
    }
    throw new Error('job nao terminou');
  };

  const push = await a.json<{ jobId: number }>('POST', '/api/v1/push', { titleId: god!.id, targetId: consoleTarget.id });
  const job = await waitForJob(push.jobId);
  check('envio concluiu', job.state === 'done', job.error);
  check('enviou os 3 arquivos do GOD', job.files_done === 3, `(veio ${job.files_done})`);
  check('contagem de bytes fecha', job.bytes_done === job.bytes_total, `(${job.bytes_done}/${job.bytes_total})`);

  // O que importa de verdade: os arquivos cairam onde a dash original procura.
  const installedDir = join(CONSOLE_HDD, 'Hdd1', 'Content', '0000000000000000', '4D5307E6', '00007000');
  const headerStat = await stat(join(installedDir, GOD_NAME));
  check('cabecalho do GOD chegou no lugar certo', headerStat.size === 0x971a, `(veio ${headerStat.size})`);

  const dataPart = await readFile(join(installedDir, `${GOD_NAME}.data`, 'Data0000'));
  check('parte de dados chegou inteira', dataPart.length === GOD_PART_A, `(veio ${dataPart.length})`);
  check('conteudo da parte confere byte a byte', dataPart.every((byte) => byte === 0xaa));

  const partB = await stat(join(installedDir, `${GOD_NAME}.data`, 'Data0001'));
  check('segunda parte chegou', partB.size === GOD_PART_B, `(veio ${partB.size})`);

  // Reenvio: arquivo com o mesmo tamanho deve ser pulado, nao retransmitido.
  // Marcamos o remoto com bytes diferentes e conferimos que ele nao foi sobrescrito.
  await writeFile(join(installedDir, `${GOD_NAME}.data`, 'Data0000'), Buffer.alloc(GOD_PART_A, 0x99));
  const push2 = await a.json<{ jobId: number }>('POST', '/api/v1/push', { titleId: god!.id, targetId: consoleTarget.id });
  const job2 = await waitForJob(push2.jobId);
  check('reenvio conclui', job2.state === 'done', job2.error);

  const afterResend = await readFile(join(installedDir, `${GOD_NAME}.data`, 'Data0000'));
  check('arquivo de mesmo tamanho e pulado no reenvio', afterResend.every((byte) => byte === 0x99));

  const iso = catalog.items.find((t) => t.kind === 'iso')!;
  const isoRes = await a.signed('POST', '/api/v1/push', { titleId: iso.id, targetId: consoleTarget.id });
  check('ISO e recusado com explicacao', isoRes.status === 422, `(veio ${isoRes.status})`);

  await ftp.close();

  console.log(`\n${failures === 0 ? 'TUDO PASSOU' : `${failures} FALHA(S)`}`);
  await cleanup();
  process.exit(failures === 0 ? 0 : 1);
}

main().catch(async (err) => {
  console.error('\nErro no smoke test:', err);
  await cleanup();
  process.exit(1);
});
