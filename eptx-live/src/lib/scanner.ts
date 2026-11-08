/**
 * Varredura da biblioteca. Descobre o que existe no HD do servidor e monta, para
 * cada titulo, a lista exata de arquivos que o console precisa baixar e o caminho
 * de destino de cada um.
 *
 * Formatos reconhecidos:
 *
 *   GOD / Games on Demand   arquivo STFS + pasta irma "<nome>.data" com Data0000...
 *   XBLA / DLC / update     arquivo STFS avulso
 *   Pasta XEX               diretorio contendo default.xex
 *   ISO                     catalogado, mas marcado como nao-instalavel (ver abaixo)
 *
 * Sobre ISO: FATX nao aceita arquivo maior que 4 GB, entao um ISO de jogo nao pode
 * simplesmente ser copiado para o HD do console. Ele precisa virar GOD antes. O
 * scanner lista o ISO para voce saber que ele esta la, com installable=false.
 */

import { readdir, stat, mkdir, writeFile } from 'node:fs/promises';
import { join, relative, basename, sep } from 'node:path';
import { createHash } from 'node:crypto';
import { config } from './config.ts';
import { db, now } from './db.ts';
import { isStfsFile, readStfsHeader, type StfsHeader } from './stfs.ts';

/** Raiz do armazenamento de conteudo no console. */
const CONTENT_ROOT = 'Hdd1:\\Content\\0000000000000000';
const ART_DIR = join(config.dbPath, '..', 'art');

export type TitleKind = 'god' | 'stfs' | 'xex' | 'iso';

export interface ScannedFile {
  /** Caminho relativo a raiz do titulo, no servidor. */
  relPath: string;
  /** Caminho absoluto de destino no console. */
  installPath: string;
  size: number;
}

export interface ScannedTitle {
  titleId: string;
  name: string;
  kind: TitleKind;
  contentType: number;
  rootPath: string;
  totalSize: number;
  publisher: string;
  description: string;
  artPath: string | null;
  files: ScannedFile[];
  installable: boolean;
}

interface ScanReport {
  found: number;
  added: number;
  updated: number;
  removed: number;
  skipped: string[];
  durationMs: number;
}

async function fileSize(p: string): Promise<number> {
  return Number((await stat(p)).size);
}

/** Grava o PNG do cabecalho em disco e devolve o caminho, ou null se nao havia arte. */
async function saveArt(header: StfsHeader, rootPath: string): Promise<string | null> {
  if (!header.thumbnail) return null;
  await mkdir(ART_DIR, { recursive: true });
  const key = createHash('sha1').update(rootPath).digest('hex').slice(0, 12);
  const out = join(ART_DIR, `${header.titleId}-${key}.png`);
  await writeFile(out, header.thumbnail);
  return out;
}

/**
 * Monta um titulo a partir de um arquivo STFS. Se existir a pasta "<arquivo>.data"
 * ao lado, e um pacote GOD multi-parte; senao e um container avulso (XBLA, DLC).
 */
async function titleFromStfs(filePath: string, dirEntries: Set<string>): Promise<ScannedTitle | null> {
  const header = await readStfsHeader(filePath);
  if (!header) return null;

  const fileName = basename(filePath);
  const dataDirName = `${fileName}.data`;
  // dirEntries vem em minusculas; a pasta de dados pode ter caixa diferente
  // do arquivo de cabecalho.
  const isGod = dirEntries.has(dataDirName.toLowerCase());

  const installDir = `${CONTENT_ROOT}\\${header.titleId}\\${header.contentTypeDir}`;
  const files: ScannedFile[] = [];

  const headerSize = await fileSize(filePath);
  files.push({ relPath: fileName, installPath: `${installDir}\\${fileName}`, size: headerSize });

  let totalSize = headerSize;

  if (isGod) {
    const dataDirPath = join(filePath, '..', dataDirName);
    const parts = (await readdir(dataDirPath)).sort();
    for (const part of parts) {
      const partPath = join(dataDirPath, part);
      const s = await stat(partPath);
      if (!s.isFile()) continue;
      const size = Number(s.size);
      files.push({
        relPath: `${dataDirName}${sep}${part}`,
        installPath: `${installDir}\\${dataDirName}\\${part}`,
        size,
      });
      totalSize += size;
    }
  }

  const name = header.displayName || header.titleName || fileName;

  return {
    titleId: header.titleId,
    name,
    kind: isGod ? 'god' : 'stfs',
    contentType: header.contentType,
    rootPath: join(filePath, '..'),
    totalSize,
    publisher: header.publisher,
    description: header.description,
    artPath: await saveArt(header, filePath),
    files,
    installable: true,
  };
}

/** Pasta com default.xex: homebrew ou jogo extraido. Vai inteira para Hdd1:\Games. */
async function titleFromXexFolder(dirPath: string): Promise<ScannedTitle | null> {
  const folderName = basename(dirPath);
  const installDir = `Hdd1:\\Games\\${folderName}`;
  const files: ScannedFile[] = [];
  let totalSize = 0;

  async function walk(current: string): Promise<void> {
    for (const entry of await readdir(current, { withFileTypes: true })) {
      const abs = join(current, entry.name);
      if (entry.isDirectory()) {
        await walk(abs);
      } else if (entry.isFile()) {
        const size = await fileSize(abs);
        const rel = relative(dirPath, abs);
        files.push({
          relPath: rel,
          installPath: `${installDir}\\${rel.split(sep).join('\\')}`,
          size,
        });
        totalSize += size;
      }
    }
  }
  await walk(dirPath);
  if (files.length === 0) return null;

  return {
    // Sem cabecalho STFS nao ha TitleID confiavel; usamos um placeholder estavel.
    titleId: '00000000',
    name: folderName,
    kind: 'xex',
    contentType: 0,
    rootPath: dirPath,
    totalSize,
    publisher: '',
    description: '',
    artPath: null,
    files,
    installable: true,
  };
}

async function titleFromIso(filePath: string): Promise<ScannedTitle> {
  const size = await fileSize(filePath);
  return {
    titleId: '00000000',
    name: basename(filePath).replace(/\.iso$/i, ''),
    kind: 'iso',
    contentType: 0,
    rootPath: join(filePath, '..'),
    totalSize: size,
    publisher: '',
    description: 'ISO precisa ser convertido para GOD antes de instalar no console (limite de 4 GB do FATX).',
    artPath: null,
    files: [{ relPath: basename(filePath), installPath: '', size }],
    installable: false,
  };
}

export async function scanLibrary(): Promise<ScanReport> {
  const started = Date.now();
  const skipped: string[] = [];
  const titles: ScannedTitle[] = [];

  async function walk(dir: string, depth: number): Promise<void> {
    if (depth > 8) return;
    let entries;
    try {
      entries = await readdir(dir, { withFileTypes: true });
    } catch (err) {
      skipped.push(`${dir}: ${(err as Error).message}`);
      return;
    }

    // Windows nao liga para caixa e os rips vem com grafias variadas
    // ('Default.xex', 'DEFAULT.XEX'), entao comparamos sempre em minusculas.
    const names = new Set(entries.map((e) => e.name.toLowerCase()));

    // Uma pasta com default.xex e um titulo inteiro; nao descemos mais nela.
    if (names.has('default.xex')) {
      const t = await titleFromXexFolder(dir);
      if (t) titles.push(t);
      return;
    }

    for (const entry of entries) {
      const abs = join(dir, entry.name);

      if (entry.isDirectory()) {
        // Pastas .data pertencem ao pacote GOD irmao, ja tratadas la.
        if (/\.data$/i.test(entry.name) && names.has(entry.name.replace(/\.data$/i, '').toLowerCase())) continue;
        await walk(abs, depth + 1);
        continue;
      }

      if (!entry.isFile()) continue;

      if (/\.iso$/i.test(entry.name)) {
        titles.push(await titleFromIso(abs));
        continue;
      }

      if (await isStfsFile(abs)) {
        const t = await titleFromStfs(abs, names);
        if (t) titles.push(t);
        else skipped.push(`${abs}: cabecalho STFS ilegivel`);
      }
    }
  }

  await walk(config.library, 0);

  // Reconciliacao: um titulo e identificado pelo root_path + primeiro arquivo, o que
  // mantem estavel a chave mesmo se o nome interno mudar entre versoes do pacote.
  const ts = now();
  const seenKeys = new Set<string>();
  let added = 0, updated = 0;

  const selectByKey = db.prepare('SELECT id FROM titles WHERE key_path = ?');
  const insertTitle = db.prepare(`
    INSERT INTO titles (title_id, name, kind, content_type, root_path, key_path, total_size, art_path,
                        description, publisher, added_at, scanned_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`);
  const updateTitle = db.prepare(`
    UPDATE titles SET title_id = ?, name = ?, kind = ?, content_type = ?, root_path = ?, total_size = ?,
                      art_path = ?, description = ?, publisher = ?, scanned_at = ?
    WHERE id = ?`);
  const clearFiles = db.prepare('DELETE FROM title_files WHERE title_pk = ?');
  const insertFile = db.prepare(`
    INSERT INTO title_files (title_pk, rel_path, install_path, size, ordinal)
    VALUES (?, ?, ?, ?, ?)`);

  for (const t of titles) {
    const key = join(t.rootPath, t.files[0]!.relPath);
    if (seenKeys.has(key)) continue;
    seenKeys.add(key);

    const existing = selectByKey.get(key) as { id: number } | undefined;
    let pk: number;
    if (existing) {
      updateTitle.run(t.titleId, t.name, t.kind, t.contentType, t.rootPath, t.totalSize, t.artPath,
        t.description, t.publisher, ts, existing.id);
      pk = existing.id;
      updated++;
    } else {
      const res = insertTitle.run(t.titleId, t.name, t.kind, t.contentType, t.rootPath, key, t.totalSize,
        t.artPath, t.description, t.publisher, ts, ts);
      pk = Number(res.lastInsertRowid);
      added++;
    }

    clearFiles.run(pk);
    t.files.forEach((f, i) => insertFile.run(pk, f.relPath, f.installPath, f.size, i));
  }

  // Some da lista o que nao foi visto nesta passada (arquivo apagado ou movido).
  const stale = db.prepare('SELECT id FROM titles WHERE scanned_at < ?').all(ts) as { id: number }[];
  const del = db.prepare('DELETE FROM titles WHERE id = ?');
  for (const row of stale) del.run(row.id);

  return {
    found: titles.length,
    added,
    updated,
    removed: stale.length,
    skipped,
    durationMs: Date.now() - started,
  };
}
