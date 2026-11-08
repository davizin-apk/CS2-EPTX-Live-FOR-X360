import Fastify from 'fastify';
import { existsSync } from 'node:fs';
import { config } from './lib/config.ts';
import { db, now } from './lib/db.ts';
import { registerRawBody } from './lib/session.ts';
import { registerAuthRoutes } from './routes/auth.ts';
import { registerCatalogRoutes } from './routes/catalog.ts';
import { registerDownloadRoutes } from './routes/download.ts';
import { registerSocialRoutes } from './routes/social.ts';
import { registerChatRoutes } from './routes/chat.ts';
import { registerPushRoutes } from './routes/push.ts';
import { registerAvatarRoutes } from './routes/avatar.ts';
import { registerRoomRoutes } from './routes/rooms.ts';
import { scanLibrary } from './lib/scanner.ts';

const app = Fastify({
  logger: { level: process.env.EPTX_LOG_LEVEL ?? 'info' },
  // O 360 as vezes demora entre o cabecalho e o corpo em Wi-Fi ruim.
  requestTimeout: 60_000,
  // Long-poll do chat segura ate 25s; o padrao do Fastify derrubaria antes.
  connectionTimeout: 0,
});

registerRawBody(app);

app.get('/health', async () => ({
  ok: true,
  serverTime: now(),
  library: config.library,
  libraryExists: existsSync(config.library),
  titles: (db.prepare('SELECT COUNT(*) AS n FROM titles').get() as { n: number }).n,
  users: (db.prepare('SELECT COUNT(*) AS n FROM users').get() as { n: number }).n,
}));

registerAuthRoutes(app);
registerCatalogRoutes(app);
registerDownloadRoutes(app);
registerSocialRoutes(app);
registerChatRoutes(app);
registerPushRoutes(app);
registerAvatarRoutes(app);
registerRoomRoutes(app);

async function main(): Promise<void> {
  await app.listen({ port: config.port, host: config.host });
  app.log.info(`EPTX Live no ar em http://${config.host}:${config.port}`);

  if (!existsSync(config.library)) {
    app.log.warn(`Biblioteca nao encontrada: ${config.library} — ajuste EPTX_LIBRARY no .env`);
    return;
  }

  // Varredura inicial em segundo plano: nao seguramos o boot por causa de I/O.
  scanLibrary()
    .then((r) => app.log.info(
      `Varredura: ${r.found} titulos (${r.added} novos, ${r.updated} atualizados, ${r.removed} removidos) em ${r.durationMs}ms`,
    ))
    .catch((err) => app.log.error({ err }, 'Falha na varredura inicial'));
}

for (const sig of ['SIGINT', 'SIGTERM'] as const) {
  process.on(sig, () => {
    app.log.info('Encerrando...');
    app.close().then(() => {
      db.close();
      process.exit(0);
    });
  });
}

main().catch((err) => {
  app.log.error(err);
  process.exit(1);
});
