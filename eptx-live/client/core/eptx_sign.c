#include "eptx_sign.h"
#include "eptx_sha256.h"
#include <string.h>

int eptx_sign_request(const char *secret_hex,
                      const char *method,
                      const char *path,
                      const char *timestamp,
                      const char *nonce,
                      const void *body,
                      size_t      body_len,
                      char        out_signature[EPTX_SIGNATURE_LEN])
{
    uint8_t key[32];
    uint8_t body_digest[EPTX_SHA256_DIGEST_SIZE];
    char    body_hex[EPTX_SHA256_DIGEST_SIZE * 2 + 1];
    uint8_t mac[EPTX_SHA256_DIGEST_SIZE];
    eptx_hmac_ctx hmac;
    static const char NL = '\n';
    int key_len;

    if (!secret_hex || !method || !path || !timestamp || !nonce || !out_signature) {
        return EPTX_SIGN_BAD_ARGS;
    }
    if (body_len > 0 && !body) {
        return EPTX_SIGN_BAD_ARGS;
    }

    key_len = eptx_from_hex(secret_hex, key, sizeof(key));
    if (key_len != 32) {
        return EPTX_SIGN_BAD_SECRET;
    }

    /* O hash do corpo entra na canonica como texto hex, nao como bytes crus. */
    eptx_sha256(body_len > 0 ? body : "", body_len, body_digest);
    eptx_to_hex(body_digest, EPTX_SHA256_DIGEST_SIZE, body_hex);

    eptx_hmac_sha256_init(&hmac, key, (size_t)key_len);
    eptx_hmac_sha256_update(&hmac, method, strlen(method));
    eptx_hmac_sha256_update(&hmac, &NL, 1);
    eptx_hmac_sha256_update(&hmac, path, strlen(path));
    eptx_hmac_sha256_update(&hmac, &NL, 1);
    eptx_hmac_sha256_update(&hmac, timestamp, strlen(timestamp));
    eptx_hmac_sha256_update(&hmac, &NL, 1);
    eptx_hmac_sha256_update(&hmac, nonce, strlen(nonce));
    eptx_hmac_sha256_update(&hmac, &NL, 1);
    eptx_hmac_sha256_update(&hmac, body_hex, strlen(body_hex));
    eptx_hmac_sha256_final(&hmac, mac);

    eptx_to_hex(mac, EPTX_SHA256_DIGEST_SIZE, out_signature);

    memset(key, 0, sizeof(key));
    memset(mac, 0, sizeof(mac));
    return EPTX_SIGN_OK;
}

/* ------------------------------------------------------------------------- */
/* Autoteste                                                                  */
/*                                                                            */
/* Os valores abaixo saem de client/core/vectors.mjs, que os calcula usando o  */
/* mesmo codigo do servidor. Se o protocolo mudar, rode o script de novo e     */
/* atualize aqui.                                                             */
/* ------------------------------------------------------------------------- */

static int check_hex(const char *label, const char *got, const char *want,
                     void (*report)(const char *, const char *, const char *))
{
    if (strcmp(got, want) == 0) return 0;
    if (report) report(label, got, want);
    return 1;
}

/* Ligue isto a printf, a OutputDebugString, ou ao seu log na tela. */
static void (*g_report)(const char *label, const char *got, const char *want) = 0;

void eptx_selftest_set_reporter(void (*fn)(const char *, const char *, const char *));
void eptx_selftest_set_reporter(void (*fn)(const char *, const char *, const char *))
{
    g_report = fn;
}

int eptx_selftest(void)
{
    uint8_t digest[EPTX_SHA256_DIGEST_SIZE];
    char    hex[EPTX_SHA256_DIGEST_SIZE * 2 + 1];
    char    sig[EPTX_SIGNATURE_LEN];
    uint8_t key[32];
    eptx_hmac_ctx hmac;
    int failures = 0;
    int i;

    /* SHA-256 de mensagem vazia. */
    eptx_sha256("", 0, digest);
    eptx_to_hex(digest, sizeof(digest), hex);
    failures += check_hex("sha256(\"\")", hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", g_report);

    /* SHA-256 de "abc" — vetor classico do FIPS 180-4. */
    eptx_sha256("abc", 3, digest);
    eptx_to_hex(digest, sizeof(digest), hex);
    failures += check_hex("sha256(\"abc\")", hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", g_report);

    eptx_sha256("EPTX", 4, digest);
    eptx_to_hex(digest, sizeof(digest), hex);
    failures += check_hex("sha256(\"EPTX\")", hex,
        "df4bcec17ec645a58932647c3014acc1e9957c2f5268852425fd3a9b5df405ac", g_report);

    /* HMAC com chave = bytes 0x00..0x1f. */
    for (i = 0; i < 32; i++) key[i] = (uint8_t)i;

    eptx_hmac_sha256_init(&hmac, key, sizeof(key));
    eptx_hmac_sha256_update(&hmac, "", 0);
    eptx_hmac_sha256_final(&hmac, digest);
    eptx_to_hex(digest, sizeof(digest), hex);
    failures += check_hex("hmac(\"\")", hex,
        "d38b42096d80f45f826b44a9d5607de72496a415d3f4a1a8c88e3bb9da8dc1cb", g_report);

    eptx_hmac_sha256_init(&hmac, key, sizeof(key));
    eptx_hmac_sha256_update(&hmac, "abc", 3);
    eptx_hmac_sha256_final(&hmac, digest);
    eptx_to_hex(digest, sizeof(digest), hex);
    failures += check_hex("hmac(\"abc\")", hex,
        "f0133729c4163dede81e21cd47839256da58171238c8a0d874397c73b14e1e47", g_report);

    /*
     * Requisicao assinada de verdade. Este e o teste que importa: ele cobre a
     * ordem dos campos da canonica, os separadores, e o hash do corpo em hex.
     * Segredo = 32 bytes 0xAA.
     */
    failures += (eptx_sign_request(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "GET", "/api/v1/catalog?limit=50", "1788000000", "deadbeef",
        0, 0, sig) != EPTX_SIGN_OK) ? 1 : 0;
    failures += check_hex("assinatura GET sem corpo", sig,
        "3c187063dfbe29510e2b8314d86f092dcd7d4f7181cb39d6e05e60ff8b8cec05", g_report);

    failures += (eptx_sign_request(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "POST", "/api/v1/push", "1788000000", "cafe1234",
        "{\"titleId\":2}", 13, sig) != EPTX_SIGN_OK) ? 1 : 0;
    failures += check_hex("assinatura POST com corpo", sig,
        "6fa6aa08542f5f7d57831c96bcd71f1b0a2fe26f284f57e0bef15098a7f78d9b", g_report);

    /* Segredo mal formado tem que ser recusado, nao assinado com lixo. */
    if (eptx_sign_request("nao-e-hex", "GET", "/", "1", "1", 0, 0, sig) != EPTX_SIGN_BAD_SECRET) {
        failures++;
        if (g_report) g_report("segredo invalido deveria falhar", "aceitou", "EPTX_SIGN_BAD_SECRET");
    }

    return failures;
}
