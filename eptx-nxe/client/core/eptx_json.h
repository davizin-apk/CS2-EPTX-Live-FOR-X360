/*
 * Parser JSON minimo, sem alocacao.
 *
 * O parser nao copia nada: ele produz um vetor de tokens que apontam para dentro
 * do buffer original. Isso importa no console, onde a heap e apertada e uma
 * resposta de catalogo pode ter algumas dezenas de KB — nao ha razao para manter
 * duas copias dela na memoria.
 *
 * Cabe o suficiente para as respostas da API do EPTX: objetos, vetores, strings,
 * numeros, booleanos e null, aninhados.
 */
#ifndef EPTX_JSON_H
#define EPTX_JSON_H

#include "eptx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EPTX_JSON_UNDEFINED = 0,
    EPTX_JSON_OBJECT    = 1,
    EPTX_JSON_ARRAY     = 2,
    EPTX_JSON_STRING    = 3,
    EPTX_JSON_PRIMITIVE = 4   /* numero, true, false, null */
} eptx_json_type;

typedef struct {
    eptx_json_type type;
    int start;   /* primeiro caractere do valor, no buffer original */
    int end;     /* um depois do ultimo */
    int size;    /* filhos: pares num objeto, elementos num vetor */
    int parent;  /* indice do token pai, ou -1 */
} eptx_json_tok;

#define EPTX_JSON_ERR_NOMEM   -1  /* faltou espaco no vetor de tokens */
#define EPTX_JSON_ERR_INVALID -2  /* JSON malformado */
#define EPTX_JSON_ERR_PARTIAL -3  /* acabou no meio de um valor */

/* Devolve quantos tokens foram usados, ou um dos erros acima. */
int eptx_json_parse(const char *js, size_t len, eptx_json_tok *tokens, int max_tokens);

/*
 * Indice do valor associado a uma chave dentro de um objeto, ou -1.
 * A comparacao e feita contra o texto cru; chaves com escape nao sao suportadas
 * (a API do EPTX nao usa nenhuma).
 */
int eptx_json_get(const char *js, const eptx_json_tok *tokens, int count, int object, const char *key);

/* n-esimo elemento de um vetor, ou -1. */
int eptx_json_at(const eptx_json_tok *tokens, int count, int array, int n);

/*
 * Copia uma string de token para out, resolvendo escapes (\" \\ \/ \b \f \n \r \t
 * e \uXXXX, este convertido para UTF-8). Devolve o comprimento, ou -1 se nao coube.
 */
int eptx_json_string(const char *js, const eptx_json_tok *tokens, int index, char *out, size_t cap);

/* Numero inteiro. Devolve 0 em sucesso, -1 se o token nao for numerico. */
int eptx_json_int(const char *js, const eptx_json_tok *tokens, int index, int64_t *out);

/* true/false. Devolve 0 em sucesso, -1 se nao for booleano. */
int eptx_json_bool(const char *js, const eptx_json_tok *tokens, int index, int *out);

/* Atalho: pega a chave e ja converte. Devolve 0 em sucesso. */
int eptx_json_get_string(const char *js, const eptx_json_tok *t, int n, int obj, const char *key, char *out, size_t cap);
int eptx_json_get_int(const char *js, const eptx_json_tok *t, int n, int obj, const char *key, int64_t *out);
int eptx_json_get_bool(const char *js, const eptx_json_tok *t, int n, int obj, const char *key, int *out);

#ifdef __cplusplus
}
#endif

#endif /* EPTX_JSON_H */
