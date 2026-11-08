/*
 * Os efeitos sonoros do Freestyle Dash.
 *
 * Os .xma da skin (XMA2, que o xsdk nao decodifica) viram PCM no empacotamento
 * (pack.mjs, com o ffmpeg): Skin/Audio/<nome>.pcm, 48 kHz, estereo, 16 bits little-endian.
 * Aqui uma thread propria alimenta o driver de audio do kernel (xsdk_audio_*), misturando
 * ate SOUND_VOICES sons nas caixas da frente.
 */
#ifndef EPTX_SOUND_H
#define EPTX_SOUND_H

#ifdef __cplusplus
extern "C" {
#endif

enum { SOUND_VOICES = 8 };

/* Abre o audio e sobe a thread que o alimenta. dir e a pasta da skin (game:/Skin). */
void sound_start(const char *dir);

/* Toca um som da skin pelo nome, sem extensao ("btn_Focus"). Le o arquivo na primeira vez. */
void sound_play(const char *name);

#ifdef __cplusplus
}
#endif

#endif
