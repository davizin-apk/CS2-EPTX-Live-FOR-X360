/*
 * SHA-256 e HMAC-SHA256 portaveis.
 *
 * Escrito sem nenhum cast de ponteiro para inteiro maior: todas as leituras e
 * escritas de palavras de 32 bits sao feitas byte a byte. Isso nao e preciosismo —
 * o Xbox 360 e PowerPC big-endian, e uma implementacao que le uint32_t direto da
 * memoria produz digests diferentes no console e no PC.
 *
 * Nao usamos as XeCrypt* do XDK porque elas sao da familia SHA-1; o protocolo do
 * EPTX assina com SHA-256.
 */
#ifndef EPTX_SHA256_H
#define EPTX_SHA256_H

#include "eptx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPTX_SHA256_DIGEST_SIZE 32
#define EPTX_SHA256_BLOCK_SIZE  64

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t  buf[EPTX_SHA256_BLOCK_SIZE];
    size_t   buf_len;
} eptx_sha256_ctx;

void eptx_sha256_init(eptx_sha256_ctx *ctx);
void eptx_sha256_update(eptx_sha256_ctx *ctx, const void *data, size_t len);
void eptx_sha256_final(eptx_sha256_ctx *ctx, uint8_t out[EPTX_SHA256_DIGEST_SIZE]);

/* Atalho para mensagem inteira de uma vez. */
void eptx_sha256(const void *data, size_t len, uint8_t out[EPTX_SHA256_DIGEST_SIZE]);

typedef struct {
    eptx_sha256_ctx inner;
    uint8_t         opad[EPTX_SHA256_BLOCK_SIZE];
} eptx_hmac_ctx;

void eptx_hmac_sha256_init(eptx_hmac_ctx *ctx, const void *key, size_t key_len);
void eptx_hmac_sha256_update(eptx_hmac_ctx *ctx, const void *data, size_t len);
void eptx_hmac_sha256_final(eptx_hmac_ctx *ctx, uint8_t out[EPTX_SHA256_DIGEST_SIZE]);

/* out precisa de len*2+1 bytes. Minusculas, para bater com o servidor. */
void eptx_to_hex(const uint8_t *bytes, size_t len, char *out);

/*
 * Decodifica hex para bytes. Devolve o numero de bytes escritos, ou -1 se a string
 * nao for hex valido ou nao couber em out_cap.
 */
int eptx_from_hex(const char *hex, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* EPTX_SHA256_H */
