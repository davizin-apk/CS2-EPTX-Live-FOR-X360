import { DatabaseSync } from 'node:sqlite';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import { config } from './config.ts';

mkdirSync(dirname(config.dbPath), { recursive: true });

export const db = new DatabaseSync(config.dbPath);

db.exec('PRAGMA journal_mode = WAL');
db.exec('PRAGMA foreign_keys = ON');
db.exec('PRAGMA synchronous = NORMAL');

db.exec(`
CREATE TABLE IF NOT EXISTS users (
  id            INTEGER PRIMARY KEY,
  gamertag      TEXT NOT NULL,
  gamertag_ci   TEXT NOT NULL UNIQUE,      -- lowercase, para busca case-insensitive
  password_hash TEXT NOT NULL,             -- scrypt: salt:hash em hex
  motto         TEXT NOT NULL DEFAULT '',
  avatar        TEXT NOT NULL DEFAULT '',  -- id de avatar interno
  is_admin      INTEGER NOT NULL DEFAULT 0,
  created_at    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS consoles (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  hw_id       TEXT NOT NULL,               -- identificador estavel enviado pelo console
  label       TEXT NOT NULL DEFAULT '',
  created_at  INTEGER NOT NULL,
  UNIQUE(user_id, hw_id)
);

CREATE TABLE IF NOT EXISTS sessions (
  token       TEXT PRIMARY KEY,            -- enviado em X-EPTX-Session
  secret      TEXT NOT NULL,               -- chave HMAC, nunca reenviada apos o login
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  console_id  INTEGER REFERENCES consoles(id) ON DELETE SET NULL,
  created_at  INTEGER NOT NULL,
  last_seen   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);

-- Nonces ja usados, para impedir replay de request assinado.
CREATE TABLE IF NOT EXISTS used_nonces (
  nonce      TEXT PRIMARY KEY,
  expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_nonces_exp ON used_nonces(expires_at);

-- Pareamento: console mostra user_code na TV, usuario aprova no celular.
CREATE TABLE IF NOT EXISTS device_codes (
  device_code TEXT PRIMARY KEY,
  user_code   TEXT NOT NULL UNIQUE,
  hw_id       TEXT NOT NULL,
  user_id     INTEGER REFERENCES users(id) ON DELETE CASCADE,
  approved    INTEGER NOT NULL DEFAULT 0,
  claimed     INTEGER NOT NULL DEFAULT 0,
  expires_at  INTEGER NOT NULL,
  created_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS titles (
  id            INTEGER PRIMARY KEY,
  title_id      TEXT NOT NULL,             -- TitleID do 360, 8 hex (ex: 4D5307E6)
  name          TEXT NOT NULL,
  kind          TEXT NOT NULL,             -- god | xbla | iso | xex
  content_type  INTEGER NOT NULL DEFAULT 0,
  root_path     TEXT NOT NULL,             -- pasta raiz do titulo no servidor
  key_path      TEXT NOT NULL UNIQUE,      -- identidade estavel: raiz + arquivo principal
  total_size    INTEGER NOT NULL,
  art_path      TEXT,                      -- PNG extraido do header STFS
  description   TEXT NOT NULL DEFAULT '',
  publisher     TEXT NOT NULL DEFAULT '',
  added_at      INTEGER NOT NULL,
  scanned_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_titles_titleid ON titles(title_id);

-- Cada arquivo que o console precisa baixar, com o caminho de destino no HD dele.
CREATE TABLE IF NOT EXISTS title_files (
  id           INTEGER PRIMARY KEY,
  title_pk     INTEGER NOT NULL REFERENCES titles(id) ON DELETE CASCADE,
  rel_path     TEXT NOT NULL,              -- relativo a root_path, no servidor
  install_path TEXT NOT NULL,              -- destino no console, ex: Hdd1:\Content\...
  size         INTEGER NOT NULL,
  ordinal      INTEGER NOT NULL,
  UNIQUE(title_pk, rel_path)
);

CREATE TABLE IF NOT EXISTS friendships (
  id          INTEGER PRIMARY KEY,
  requester   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  addressee   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  status      TEXT NOT NULL,               -- pending | accepted | blocked
  created_at  INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL,
  UNIQUE(requester, addressee)
);

CREATE TABLE IF NOT EXISTS presence (
  user_id     INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  state       TEXT NOT NULL,               -- online | away | offline
  title_id    TEXT NOT NULL DEFAULT '',
  title_name  TEXT NOT NULL DEFAULT '',
  detail      TEXT NOT NULL DEFAULT '',
  updated_at  INTEGER NOT NULL
);

-- O avatar desenhado da pessoa: look e a lista de escolhas do editor da dash
-- (JSON), e png o corpo inteiro ja desenhado, para /avatar/<gamertag>/avatar-body.png.
-- Conta sem linha aqui usa o avatar padrao.
CREATE TABLE IF NOT EXISTS avatars (
  user_id     INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  png         BLOB NOT NULL,
  width       INTEGER NOT NULL,
  height      INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL,
  look        TEXT,
  mime        TEXT NOT NULL DEFAULT 'image/png'  -- de antes da avatar_photos; hoje sempre image/png
);

-- A foto do avatar mandada de fora: o GIF salvo no 360sona ou um PNG. Fica junto
-- com o desenhado, e in_use diz qual dos dois a conta mostra.
CREATE TABLE IF NOT EXISTS avatar_photos (
  user_id     INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  data        BLOB NOT NULL,
  mime        TEXT NOT NULL,               -- image/gif | image/png
  width       INTEGER NOT NULL,
  height      INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL,
  in_use      INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS conversations (
  id          INTEGER PRIMARY KEY,
  kind        TEXT NOT NULL,               -- dm | group
  name        TEXT NOT NULL DEFAULT '',
  created_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS conversation_members (
  conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
  user_id         INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  last_read_id    INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (conversation_id, user_id)
);

CREATE TABLE IF NOT EXISTS messages (
  id              INTEGER PRIMARY KEY,
  conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
  sender_id       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  body            TEXT NOT NULL,
  created_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_messages_conv ON messages(conversation_id, id);

-- Progresso de download, para o console retomar de onde parou.
CREATE TABLE IF NOT EXISTS downloads (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  console_id  INTEGER REFERENCES consoles(id) ON DELETE CASCADE,
  title_pk    INTEGER NOT NULL REFERENCES titles(id) ON DELETE CASCADE,
  file_id     INTEGER NOT NULL REFERENCES title_files(id) ON DELETE CASCADE,
  bytes_done  INTEGER NOT NULL DEFAULT 0,
  state       TEXT NOT NULL DEFAULT 'queued', -- queued | active | done | failed
  updated_at  INTEGER NOT NULL,
  UNIQUE(user_id, file_id)
);

-- Consoles alcancaveis por FTP. E assim que o conteudo chega no HD sem que o
-- usuario saia da dash original: o DashLaunch mantem o FTP vivo por baixo dela.
CREATE TABLE IF NOT EXISTS push_targets (
  id         INTEGER PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  host       TEXT NOT NULL,
  port       INTEGER NOT NULL DEFAULT 21,
  username   TEXT NOT NULL DEFAULT 'xbox',
  password   TEXT NOT NULL DEFAULT 'xbox',
  last_ok    INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS push_jobs (
  id          INTEGER PRIMARY KEY,
  target_id   INTEGER NOT NULL REFERENCES push_targets(id) ON DELETE CASCADE,
  title_pk    INTEGER NOT NULL REFERENCES titles(id) ON DELETE CASCADE,
  user_id     INTEGER REFERENCES users(id) ON DELETE SET NULL,
  state       TEXT NOT NULL DEFAULT 'queued',  -- queued | active | done | failed
  bytes_total INTEGER NOT NULL DEFAULT 0,
  bytes_done  INTEGER NOT NULL DEFAULT 0,
  files_total INTEGER NOT NULL DEFAULT 0,
  files_done  INTEGER NOT NULL DEFAULT 0,
  current     TEXT NOT NULL DEFAULT '',
  error       TEXT NOT NULL DEFAULT '',
  started_at  INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_push_jobs_state ON push_jobs(state, updated_at);
`);

// Bancos de antes do editor de avatar e do GIF nao tem as colunas look e mime.
const avatarColumns = (db.prepare('PRAGMA table_info(avatars)').all() as { name: string }[]).map((c) => c.name);
if (!avatarColumns.includes('look')) db.exec('ALTER TABLE avatars ADD COLUMN look TEXT');
if (!avatarColumns.includes('mime')) db.exec("ALTER TABLE avatars ADD COLUMN mime TEXT NOT NULL DEFAULT 'image/png'");
// Antes da avatar_photos, a foto ocupava a linha do desenhado (sem look): passa para la.
db.exec(`
  INSERT OR IGNORE INTO avatar_photos (user_id, data, mime, width, height, updated_at, in_use)
    SELECT user_id, png, mime, width, height, updated_at, 1 FROM avatars WHERE look IS NULL;
  DELETE FROM avatars WHERE look IS NULL;`);

export function now(): number {
  return Math.floor(Date.now() / 1000);
}
