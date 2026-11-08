/*
 * Cliente HTTP/1.1 minimo sobre sockets.
 *
 * Usa a API WinSock, que e a mesma no Windows e no XDK do Xbox 360 — no console
 * ela vem do XNet, e a unica diferenca e o XNetStartup na inicializacao, tratado
 * dentro de eptx_http_init().
 *
 * O corpo nunca e bufferizado inteiro: ele vai saindo por um callback conforme
 * chega. Uma parte de GOD tem centenas de MB e nao existe a menor chance de
 * segurar isso na memoria do console.
 */
#ifndef EPTX_HTTP_H
#define EPTX_HTTP_H

#include "eptx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPTX_HTTP_OK             0
#define EPTX_HTTP_ERR_INIT      -1
#define EPTX_HTTP_ERR_RESOLVE   -2
#define EPTX_HTTP_ERR_CONNECT   -3
#define EPTX_HTTP_ERR_SEND      -4
#define EPTX_HTTP_ERR_RECV      -5
#define EPTX_HTTP_ERR_PROTOCOL  -6
#define EPTX_HTTP_ERR_HEADERS   -7  /* cabecalhos maiores que o buffer */
#define EPTX_HTTP_ERR_ABORTED   -8  /* o sink pediu para parar */

/*
 * Recebe cada pedaco do corpo conforme ele chega. Devolva 0 para continuar ou
 * qualquer outro valor para abortar (erro de gravacao, cancelamento do usuario).
 */
typedef int (*eptx_http_sink)(void *user, const uint8_t *data, size_t len);

typedef struct {
    const char *method;
    const char *path;      /* caminho com query, comecando em '/' */
    const char *headers;   /* linhas extras ja terminadas em CRLF, ou NULL */
    const void *body;
    size_t      body_len;
    int         timeout_ms; /* 0 usa o padrao de 30s */
} eptx_http_request;

typedef struct {
    int     status;
    int64_t content_length;  /* -1 quando ausente */
    int64_t range_total;     /* tamanho total vindo de Content-Range, ou -1 */
    char    install_path[300]; /* X-EPTX-Install-Path, string vazia se ausente */
} eptx_http_response;

int  eptx_http_init(void);
void eptx_http_shutdown(void);

/*
 * host aceita IP em notacao decimal ("192.168.0.10") ou nome. No console prefira
 * IP: a resolucao de nomes depende de um DNS configurado, e o ponto do EPTX e
 * justamente nao depender de DNS nenhum.
 */
int eptx_http_perform(const char *host, int port,
                      const eptx_http_request *req,
                      eptx_http_response *res,
                      eptx_http_sink sink, void *user);

/* Texto para um dos codigos EPTX_HTTP_ERR_*. */
const char *eptx_http_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* EPTX_HTTP_H */
