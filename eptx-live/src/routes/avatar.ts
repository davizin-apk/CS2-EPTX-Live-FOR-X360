/**
 * Avatar de cada conta.
 *
 * O avatar do 360 e desenhado em 3D pela biblioteca de avatares do XDK, que o
 * console nao tem fora da dash oficial. Uma conta EPTX guarda dois, e mostra um:
 *
 *   - o desenhado ("look"): o boneco proprio do EPTX, que a dash desenha a partir
 *     de uma lista de escolhas (lib/avatarLook.ts), editada no editor de avatar da
 *     dash. Toda conta comeca com o look padrao.
 *   - a foto: um GIF animado salvo no 360sona (pela pagina /avatar deste servidor)
 *     ou um PNG mandado por `npm run avatar -- --file`. A dash mostra o primeiro
 *     quadro do GIF e roda a animacao quando o avatar esta selecionado.
 *
 * Mandar uma foto passa a mostrar a foto; salvar no editor passa a mostrar o
 * desenhado; e o perfil da dash troca entre os dois (POST /api/v1/me/avatar/use).
 *
 * Os enderecos publicos imitam o do servico de avatares do Xbox LIVE,
 * /avatar/<gamertag>/avatar-body.png (e .gif), mas respondem pelas contas do
 * EPTX: nada aqui fala com a Microsoft.
 */

import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import { existsSync, readFileSync } from 'node:fs';
import { db, now } from '../lib/db.ts';
import { requireSession, sessionOf } from '../lib/session.ts';
import { checkAvatarImage, GIF_MAX_BYTES, type AvatarMime } from '../lib/png.ts';
import { DEFAULT_LOOK, parseLook } from '../lib/avatarLook.ts';

const upsertDrawn = db.prepare(`
  INSERT INTO avatars (user_id, png, width, height, updated_at, look, mime) VALUES (?, ?, ?, ?, ?, ?, 'image/png')
  ON CONFLICT(user_id) DO UPDATE SET
    png = excluded.png, width = excluded.width, height = excluded.height, updated_at = excluded.updated_at,
    look = excluded.look`);
const upsertPhoto = db.prepare(`
  INSERT INTO avatar_photos (user_id, data, mime, width, height, updated_at, in_use) VALUES (?, ?, ?, ?, ?, ?, 1)
  ON CONFLICT(user_id) DO UPDATE SET
    data = excluded.data, mime = excluded.mime, width = excluded.width, height = excluded.height,
    updated_at = excluded.updated_at, in_use = 1`);
const usePhoto = db.prepare('UPDATE avatar_photos SET in_use = ?, updated_at = ? WHERE user_id = ?');
const deleteDrawn = db.prepare('DELETE FROM avatars WHERE user_id = ?');
const deletePhoto = db.prepare('DELETE FROM avatar_photos WHERE user_id = ?');
const selectUserByTag = db.prepare('SELECT id FROM users WHERE gamertag_ci = ?');
const selectDrawn = db.prepare('SELECT png, look, updated_at FROM avatars WHERE user_id = ?');
const selectPhoto = db.prepare('SELECT mime, updated_at, in_use FROM avatar_photos WHERE user_id = ?');
const selectPhotoData = db.prepare('SELECT data, mime, updated_at, in_use FROM avatar_photos WHERE user_id = ?');

/**
 * O corpo do avatar padrao, desenhado pela propria dash (o preview grava em
 * src/assets): e a imagem de quem ainda nao mexeu no avatar.
 */
const DEFAULT_PNG_PATH = new URL('../assets/avatar-default.png', import.meta.url);
const defaultPng = existsSync(DEFAULT_PNG_PATH) ? readFileSync(DEFAULT_PNG_PATH) : null;
const PAGE_PATH = new URL('../web/avatar.html', import.meta.url);

export type AvatarType = 'look' | 'png' | 'gif';

export interface AvatarInfo {
  /** Quando o avatar mostrado mudou pela ultima vez, ou 0 com o padrao: a dash usa para saber se baixa de novo. */
  avatarUpdated: number;
  /** O que a conta mostra agora: o desenhado, ou a foto (PNG ou GIF). */
  avatarType: AvatarType;
  /** As escolhas do desenhado; o padrao se a conta nunca abriu o editor. */
  avatarLook: readonly number[];
  /** A foto guardada, mostrada ou nao; null sem foto. */
  avatarPhoto: 'png' | 'gif' | null;
}

interface PhotoRow { mime: string; updated_at: number; in_use: number }
interface DrawnRow { look: string | null; updated_at: number }

export function avatarInfo(userId: number): AvatarInfo {
  const drawn = selectDrawn.get(userId) as DrawnRow | undefined;
  const photo = selectPhoto.get(userId) as PhotoRow | undefined;
  const look = (drawn?.look ? parseLook(JSON.parse(drawn.look)) : null) ?? DEFAULT_LOOK;
  const photoType = photo ? (photo.mime === 'image/gif' ? 'gif' : 'png') : null;
  if (photo && photo.in_use && photoType) {
    return { avatarUpdated: photo.updated_at, avatarType: photoType, avatarLook: look, avatarPhoto: photoType };
  }
  return { avatarUpdated: drawn?.updated_at ?? 0, avatarType: 'look', avatarLook: look, avatarPhoto: photoType };
}

export function registerAvatarRoutes(app: FastifyInstance): void {
  const guard = { preHandler: requireSession };

  // A foto, de fora da dash: o GIF do 360sona ou um PNG parado. Passa a ser a mostrada.
  app.put('/api/v1/me/avatar', { ...guard, bodyLimit: GIF_MAX_BYTES + 1024 }, async (req, reply) => {
    const type = req.headers['content-type'] ?? '';
    const mime: AvatarMime | null = type.startsWith('image/png')
      ? 'image/png'
      : type.startsWith('image/gif')
        ? 'image/gif'
        : null;
    if (!mime) return reply.code(415).send({ error: 'envie_png_ou_gif' });
    const data = req.rawBody;
    const checked = checkAvatarImage(data, mime);
    if ('error' in checked) return reply.code(checked.code).send(checked.error);
    const updatedAt = now();
    upsertPhoto.run(sessionOf(req).userId, data, mime, checked.width, checked.height, updatedAt);
    return { ok: true, ...checked, type: mime === 'image/gif' ? 'gif' : 'png', updatedAt };
  });

  // O que o editor de avatar da dash salva: as escolhas e o corpo desenhado. Passa a ser o mostrado.
  app.post('/api/v1/me/avatar/look', guard, async (req, reply) => {
    const body = req.body as { look?: unknown; png?: unknown } | undefined;
    const look = parseLook(body?.look);
    if (!look) return reply.code(400).send({ error: 'look_invalido' });
    if (typeof body?.png !== 'string') return reply.code(400).send({ error: 'falta_png' });
    const png = Buffer.from(body.png, 'base64');
    const checked = checkAvatarImage(png, 'image/png');
    if ('error' in checked) return reply.code(checked.code).send(checked.error);
    const userId = sessionOf(req).userId;
    const updatedAt = now();
    upsertDrawn.run(userId, png, checked.width, checked.height, updatedAt, JSON.stringify(look));
    usePhoto.run(0, updatedAt, userId);
    return { ok: true, updatedAt };
  });

  // Troca entre o desenhado e a foto, sem mandar nada de novo.
  app.post('/api/v1/me/avatar/use', guard, async (req, reply) => {
    const { use } = (req.body ?? {}) as { use?: unknown };
    if (use !== 'look' && use !== 'photo') return reply.code(400).send({ error: 'use_look_ou_photo' });
    const userId = sessionOf(req).userId;
    if (use === 'photo' && !selectPhoto.get(userId)) return reply.code(409).send({ error: 'sem_foto' });
    usePhoto.run(use === 'photo' ? 1 : 0, now(), userId);
    return { ok: true, ...avatarInfo(userId) };
  });

  // Volta ao avatar padrao: apaga o desenhado e a foto.
  app.delete('/api/v1/me/avatar', guard, async (req) => {
    const userId = sessionOf(req).userId;
    deleteDrawn.run(userId);
    deletePhoto.run(userId);
    return { ok: true };
  });

  // Publicos e sem assinatura, como o original: e so a imagem que os amigos ja veem na dash.
  // Gamertag que nao existe da 404.
  const findUser = (req: FastifyRequest): number | null => {
    const { gamertag } = req.params as { gamertag: string };
    const user = selectUserByTag.get(gamertag.toLowerCase()) as { id: number } | undefined;
    return user ? user.id : null;
  };
  const send = (reply: FastifyReply, data: Buffer, mime: string, updatedAt: number | null) => {
    reply.header('Content-Type', mime);
    if (updatedAt) reply.header('Last-Modified', new Date(updatedAt * 1000).toUTCString());
    reply.header('Cache-Control', 'no-cache');
    return reply.send(data);
  };

  // O PNG: a foto, se a mostrada for um PNG; senao o corpo desenhado (ou o padrao).
  app.get('/avatar/:gamertag/avatar-body.png', async (req, reply) => {
    const userId = findUser(req);
    if (userId === null) return reply.code(404).send({ error: 'sem_avatar' });
    const photo = selectPhotoData.get(userId) as (PhotoRow & { data: Uint8Array }) | undefined;
    if (photo && photo.in_use && photo.mime === 'image/png') return send(reply, Buffer.from(photo.data), 'image/png', photo.updated_at);
    const drawn = selectDrawn.get(userId) as (DrawnRow & { png: Uint8Array }) | undefined;
    if (drawn) return send(reply, Buffer.from(drawn.png), 'image/png', drawn.updated_at);
    if (!defaultPng) return reply.code(404).send({ error: 'sem_avatar' });
    return send(reply, defaultPng, 'image/png', null);
  });

  // O GIF do 360sona, mostrado ou nao.
  app.get('/avatar/:gamertag/avatar-body.gif', async (req, reply) => {
    const userId = findUser(req);
    if (userId === null) return reply.code(404).send({ error: 'sem_avatar' });
    const photo = selectPhotoData.get(userId) as (PhotoRow & { data: Uint8Array }) | undefined;
    if (!photo || photo.mime !== 'image/gif') return reply.code(404).send({ error: 'sem_gif' });
    return send(reply, Buffer.from(photo.data), 'image/gif', photo.updated_at);
  });

  // A pagina para montar o avatar no 360sona e mandar o GIF para a conta. Lida a cada
  // pedido, para quem mexe nela ver a mudanca sem reiniciar o servidor.
  app.get('/avatar', async (_req, reply) => {
    reply.header('Content-Type', 'text/html; charset=utf-8');
    reply.header('Cache-Control', 'no-cache');
    return reply.send(readFileSync(PAGE_PATH));
  });
}
