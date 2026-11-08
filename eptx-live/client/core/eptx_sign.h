/*
 * Assinatura das requisicoes do EPTX.
 *
 * String canonica, com '\n' entre os campos:
 *
 *     METODO
 *     /caminho?com=query
 *     TIMESTAMP
 *     NONCE
 *     sha256hex(corpo)
 *
 * A canonica nunca e montada num buffer: ela e alimentada pedaco a pedaco direto
 * no HMAC. Assim nao existe limite de tamanho de caminho nem de corpo, e nao ha
 * um buffer com o segredo derivado passeando pela pilha.
 *
 * Qualquer divergencia aqui em relacao ao servidor quebra o login inteiro, e o
 * erro que aparece e um 401 generico. Por isso o autoteste existe: rode
 * eptx_selftest() antes da primeira requisicao.
 */
#ifndef EPTX_SIGN_H
#define EPTX_SIGN_H

#include "eptx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 64 caracteres de hex mais o terminador. */
#define EPTX_SIGNATURE_LEN 65

#define EPTX_SIGN_OK           0
#define EPTX_SIGN_BAD_SECRET  -1
#define EPTX_SIGN_BAD_ARGS    -2

/*
 * secret_hex: o campo "secret" devolvido pelo login, 64 caracteres de hex.
 * body pode ser NULL quando body_len for 0.
 * out_signature recebe 64 caracteres de hex minusculo mais o terminador.
 */
int eptx_sign_request(const char *secret_hex,
                      const char *method,
                      const char *path,
                      const char *timestamp,
                      const char *nonce,
                      const void *body,
                      size_t      body_len,
                      char        out_signature[EPTX_SIGNATURE_LEN]);

/*
 * Confere SHA-256, HMAC-SHA256 e a montagem da canonica contra vetores gerados
 * pelo codigo do servidor. Devolve 0 se tudo bateu, ou o numero de falhas.
 *
 * Se isto falhar no console mas passar no PC, o suspeito e ordem de bytes.
 */
int eptx_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* EPTX_SIGN_H */
