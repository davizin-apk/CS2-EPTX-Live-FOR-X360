/**
 * Salas de jogo e o relay UDP.
 *
 * Uma sala junta amigos para jogar juntos (o primeiro uso e o Bot Strike: ate 4 humanos contra bots, no 360 e no
 * PC). Quem cria a sala e o host: a partida roda na maquina dele, e os outros mandam comandos e recebem o estado.
 *
 * A troca de pacotes nao e direta entre os jogadores: passa por este servidor (UDP, porta EPTX_PORT + 1). Assim
 * ninguem precisa abrir porta no roteador, e o 360 so fala com o servidor do EPTX, nunca com a Microsoft.
 *
 * Pacote (big-endian, os dois sentidos):
 *   'BSN1' | sala u32 | slot u8 | chave 8 bytes | tipo u8 | alvo u8 | dados...
 * A chave de cada jogador sai da API assinada (GET /api/v1/rooms/current) e so serve para esta sala. O servidor
 * zera a chave antes de repassar. Tipos: 0 manter (o servidor aprende o endereco), 1 dados (cliente -> host, ou host
 * -> o slot do alvo, 255 para todos), 9 ping (o servidor devolve o pacote como veio: o teste de UDP do console).
 *
 * As salas ficam so na memoria: reiniciar o servidor desfaz todas, e tudo bem.
 */

import dgram from 'node:dgram';
import { randomBytes } from 'node:crypto';
import type { FastifyInstance } from 'fastify';
import { db, now } from '../lib/db.ts';
import { config } from '../lib/config.ts';
import { requireSession, sessionOf } from '../lib/session.ts';

const MAGIC = Buffer.from('BSN1');
const HEADER = 4 + 4 + 1 + 8 + 1 + 1;
const MAX_MEMBERS = 4;
const STALE_SECONDS = 60;
export const relayPort = config.port + 1;

interface Member {
  userId: number;
  gamertag: string;
  slot: number;
  key: Buffer;
  packHash: string;
  address?: string;
  port?: number;
  lastSeen: number;
  lastUdp: number;
}

interface Room {
  id: number;
  game: string;
  hostId: number;
  mode: string;
  settings: Record<string, unknown>;
  state: 'lobby' | 'playing';
  matchId: number;
  members: Member[];
  invites: Map<number, number>; // userId -> quando
  createdAt: number;
}

const rooms = new Map<number, Room>();
let nextRoomId = Math.floor(Math.random() * 100000) + 1;

function roomOf(userId: number): Room | undefined {
  for (const r of rooms.values()) if (r.members.some((m) => m.userId === userId)) return r;
  return undefined;
}

function leave(userId: number): void {
  const r = roomOf(userId);
  if (!r) return;
  // o host saiu: a partida era na maquina dele, a sala acaba
  if (r.hostId === userId) {
    rooms.delete(r.id);
    return;
  }
  r.members = r.members.filter((m) => m.userId !== userId);
  if (r.members.length === 0) rooms.delete(r.id);
}

function freeSlot(r: Room): number {
  for (let s = 0; s < MAX_MEMBERS; s++) if (!r.members.some((m) => m.slot === s)) return s;
  return -1;
}

function view(r: Room, me: number) {
  const mine = r.members.find((m) => m.userId === me);
  const host = r.members.find((m) => m.userId === r.hostId);
  return {
    id: r.id,
    game: r.game,
    hostId: r.hostId,
    hostGamertag: host?.gamertag ?? '',
    mode: r.mode,
    settings: r.settings,
    state: r.state,
    matchId: r.matchId,
    relayPort,
    members: r.members.map((m) => ({
      userId: m.userId,
      gamertag: m.gamertag,
      slot: m.slot,
      packHash: m.packHash,
      udp: now() - m.lastUdp < 10,
    })),
    you: mine ? { slot: mine.slot, key: mine.key.toString('hex') } : null,
  };
}

const findFriendship = db.prepare(`
  SELECT id FROM friendships
  WHERE ((requester = ? AND addressee = ?) OR (requester = ? AND addressee = ?)) AND status = 'accepted'`);
const findTag = db.prepare('SELECT gamertag FROM users WHERE id = ?');

function sweep(): void {
  const ts = now();
  for (const r of [...rooms.values()]) {
    for (const m of [...r.members]) {
      if (ts - Math.max(m.lastSeen, m.lastUdp) > STALE_SECONDS) leave(m.userId);
    }
    for (const [u, at] of r.invites) if (ts - at > 300) r.invites.delete(u);
  }
}

export function registerRoomRoutes(app: FastifyInstance): void {
  const guard = { preHandler: requireSession };
  setInterval(sweep, 10_000).unref();

  const touch = (r: Room | undefined, userId: number) => {
    const m = r?.members.find((x) => x.userId === userId);
    if (m) m.lastSeen = now();
  };

  app.post('/api/v1/rooms', guard, async (req) => {
    const s = sessionOf(req);
    const { game, mode, packHash, settings } = (req.body ?? {}) as {
      game?: string; mode?: string; packHash?: string; settings?: Record<string, unknown>;
    };
    leave(s.userId);
    const r: Room = {
      id: nextRoomId++,
      game: typeof game === 'string' ? game.slice(0, 32) : 'botstrike',
      hostId: s.userId,
      mode: typeof mode === 'string' ? mode.slice(0, 32) : 'classic',
      settings: settings && typeof settings === 'object' ? settings : {},
      state: 'lobby',
      matchId: 0,
      members: [{ userId: s.userId, gamertag: s.gamertag, slot: 0, key: randomBytes(8),
        packHash: typeof packHash === 'string' ? packHash.slice(0, 64) : '', lastSeen: now(), lastUdp: 0 }],
      invites: new Map(),
      createdAt: now(),
    };
    rooms.set(r.id, r);
    return { room: view(r, s.userId) };
  });

  app.get('/api/v1/rooms/current', guard, async (req) => {
    const s = sessionOf(req);
    const r = roomOf(s.userId);
    touch(r, s.userId);
    return { room: r ? view(r, s.userId) : null };
  });

  app.get('/api/v1/rooms/invites', guard, async (req) => {
    const me = sessionOf(req).userId;
    const items = [];
    for (const r of rooms.values()) {
      if (!r.invites.has(me)) continue;
      const host = r.members.find((m) => m.userId === r.hostId);
      items.push({ roomId: r.id, game: r.game, mode: r.mode, hostGamertag: host?.gamertag ?? '', members: r.members.length });
    }
    return { items };
  });

  app.post('/api/v1/rooms/:id/invite', guard, async (req, reply) => {
    const s = sessionOf(req);
    const r = rooms.get(Number((req.params as { id: string }).id));
    if (!r || !r.members.some((m) => m.userId === s.userId)) return reply.code(404).send({ error: 'sala_nao_encontrada' });
    const { userId } = (req.body ?? {}) as { userId?: number };
    if (typeof userId !== 'number') return reply.code(400).send({ error: 'campos_faltando' });
    if (!findFriendship.get(s.userId, userId, userId, s.userId)) return reply.code(403).send({ error: 'nao_sao_amigos' });
    r.invites.set(userId, now());
    return { ok: true };
  });

  app.post('/api/v1/rooms/:id/join', guard, async (req, reply) => {
    const s = sessionOf(req);
    const r = rooms.get(Number((req.params as { id: string }).id));
    if (!r) return reply.code(404).send({ error: 'sala_nao_encontrada' });
    if (!r.invites.has(s.userId) && !r.members.some((m) => m.userId === s.userId)) {
      return reply.code(403).send({ error: 'sem_convite' });
    }
    const { packHash } = (req.body ?? {}) as { packHash?: string };
    if (!r.members.some((m) => m.userId === s.userId)) {
      leave(s.userId);
      const slot = freeSlot(r);
      if (slot < 0) return reply.code(409).send({ error: 'sala_cheia' });
      const tag = (findTag.get(s.userId) as { gamertag: string } | undefined)?.gamertag ?? s.gamertag;
      r.members.push({ userId: s.userId, gamertag: tag, slot, key: randomBytes(8),
        packHash: typeof packHash === 'string' ? packHash.slice(0, 64) : '', lastSeen: now(), lastUdp: 0 });
    }
    r.invites.delete(s.userId);
    return { room: view(r, s.userId) };
  });

  app.post('/api/v1/rooms/:id/leave', guard, async (req) => {
    leave(sessionOf(req).userId);
    return { ok: true };
  });

  app.post('/api/v1/rooms/:id/settings', guard, async (req, reply) => {
    const s = sessionOf(req);
    const r = rooms.get(Number((req.params as { id: string }).id));
    if (!r || r.hostId !== s.userId) return reply.code(403).send({ error: 'so_o_host' });
    const { mode, settings, state } = (req.body ?? {}) as {
      mode?: string; settings?: Record<string, unknown>; state?: string;
    };
    if (typeof mode === 'string') r.mode = mode.slice(0, 32);
    if (settings && typeof settings === 'object') r.settings = settings;
    if (state === 'playing' && r.state !== 'playing') {
      r.state = 'playing';
      r.matchId++;
    } else if (state === 'lobby') {
      r.state = 'lobby';
    }
    touch(r, s.userId);
    return { room: view(r, s.userId) };
  });

  startRelay(app);
}

function startRelay(app: FastifyInstance): void {
  const sock = dgram.createSocket('udp4');
  sock.on('error', (err) => app.log.error({ err }, 'relay UDP'));
  sock.on('message', (msg, rinfo) => {
    if (msg.length < HEADER || msg.length > 1400 || !msg.subarray(0, 4).equals(MAGIC)) return;
    const roomId = msg.readUInt32BE(4);
    const slot = msg.readUInt8(8);
    const key = msg.subarray(9, 17);
    const type = msg.readUInt8(17);
    const target = msg.readUInt8(18);
    if (type === 9 && roomId === 0) {
      // ping sem sala: o teste de UDP do console
      sock.send(msg, rinfo.port, rinfo.address);
      return;
    }
    const r = rooms.get(roomId);
    const from = r?.members.find((m) => m.slot === slot);
    if (!r || !from || !from.key.equals(key)) return;
    from.address = rinfo.address;
    from.port = rinfo.port;
    from.lastUdp = now();
    if (type === 9) {
      sock.send(msg, rinfo.port, rinfo.address);
      return;
    }
    if (type !== 1) return;
    const out = Buffer.from(msg);
    out.fill(0, 9, 17); // a chave nao vai para os outros
    const host = r.members.find((m) => m.userId === r.hostId);
    const send = (m: Member | undefined) => {
      if (m && m !== from && m.address && m.port) sock.send(out, m.port, m.address);
    };
    if (from.userId !== r.hostId) send(host);
    else if (target === 255) r.members.forEach(send);
    else send(r.members.find((m) => m.slot === target));
  });
  sock.bind(relayPort, config.host, () => app.log.info(`relay UDP das salas na porta ${relayPort}`));
}
