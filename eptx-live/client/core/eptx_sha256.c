#include "eptx_sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/* Le 4 bytes big-endian sem depender da ordem de bytes da maquina. */
static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be32(block + i * 4);
    }
    for (i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void eptx_sha256_init(eptx_sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bit_len = 0;
    ctx->buf_len = 0;
}

void eptx_sha256_update(eptx_sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->bit_len += (uint64_t)len * 8;

    /* Completa o bloco parcial que sobrou da chamada anterior. */
    if (ctx->buf_len > 0) {
        size_t need = EPTX_SHA256_BLOCK_SIZE - ctx->buf_len;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == EPTX_SHA256_BLOCK_SIZE) {
            sha256_block(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    while (len >= EPTX_SHA256_BLOCK_SIZE) {
        sha256_block(ctx->state, p);
        p += EPTX_SHA256_BLOCK_SIZE;
        len -= EPTX_SHA256_BLOCK_SIZE;
    }

    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buf_len = len;
    }
}

void eptx_sha256_final(eptx_sha256_ctx *ctx, uint8_t out[EPTX_SHA256_DIGEST_SIZE])
{
    uint64_t bits = ctx->bit_len;
    int i;

    /* Padding: 0x80, zeros, e o comprimento em bits em 8 bytes big-endian. */
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > EPTX_SHA256_BLOCK_SIZE - 8) {
        while (ctx->buf_len < EPTX_SHA256_BLOCK_SIZE) ctx->buf[ctx->buf_len++] = 0;
        sha256_block(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < EPTX_SHA256_BLOCK_SIZE - 8) ctx->buf[ctx->buf_len++] = 0;

    for (i = 7; i >= 0; i--) {
        ctx->buf[EPTX_SHA256_BLOCK_SIZE - 8 + (7 - i)] = (uint8_t)(bits >> (i * 8));
    }
    sha256_block(ctx->state, ctx->buf);

    for (i = 0; i < 8; i++) store_be32(out + i * 4, ctx->state[i]);
}

void eptx_sha256(const void *data, size_t len, uint8_t out[EPTX_SHA256_DIGEST_SIZE])
{
    eptx_sha256_ctx ctx;
    eptx_sha256_init(&ctx);
    eptx_sha256_update(&ctx, data, len);
    eptx_sha256_final(&ctx, out);
}

void eptx_hmac_sha256_init(eptx_hmac_ctx *ctx, const void *key, size_t key_len)
{
    uint8_t block[EPTX_SHA256_BLOCK_SIZE];
    uint8_t ipad[EPTX_SHA256_BLOCK_SIZE];
    size_t i;

    memset(block, 0, sizeof(block));
    if (key_len > EPTX_SHA256_BLOCK_SIZE) {
        /* Chave longa demais e substituida pelo proprio digest dela. */
        eptx_sha256(key, key_len, block);
    } else {
        memcpy(block, key, key_len);
    }

    for (i = 0; i < EPTX_SHA256_BLOCK_SIZE; i++) {
        ipad[i]      = (uint8_t)(block[i] ^ 0x36);
        ctx->opad[i] = (uint8_t)(block[i] ^ 0x5c);
    }

    eptx_sha256_init(&ctx->inner);
    eptx_sha256_update(&ctx->inner, ipad, EPTX_SHA256_BLOCK_SIZE);

    memset(block, 0, sizeof(block));
    memset(ipad, 0, sizeof(ipad));
}

void eptx_hmac_sha256_update(eptx_hmac_ctx *ctx, const void *data, size_t len)
{
    eptx_sha256_update(&ctx->inner, data, len);
}

void eptx_hmac_sha256_final(eptx_hmac_ctx *ctx, uint8_t out[EPTX_SHA256_DIGEST_SIZE])
{
    uint8_t inner_digest[EPTX_SHA256_DIGEST_SIZE];
    eptx_sha256_ctx outer;

    eptx_sha256_final(&ctx->inner, inner_digest);

    eptx_sha256_init(&outer);
    eptx_sha256_update(&outer, ctx->opad, EPTX_SHA256_BLOCK_SIZE);
    eptx_sha256_update(&outer, inner_digest, EPTX_SHA256_DIGEST_SIZE);
    eptx_sha256_final(&outer, out);

    memset(ctx->opad, 0, sizeof(ctx->opad));
    memset(inner_digest, 0, sizeof(inner_digest));
}

void eptx_to_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[i * 2]     = digits[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int eptx_from_hex(const char *hex, uint8_t *out, size_t out_cap)
{
    size_t len = strlen(hex);
    size_t i;

    if ((len % 2) != 0) return -1;
    if (len / 2 > out_cap) return -1;

    for (i = 0; i < len; i += 2) {
        int hi = hex_value(hex[i]);
        int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(len / 2);
}
