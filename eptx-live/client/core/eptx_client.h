/*
 * Cliente do EPTX: sessao, requisicoes assinadas e download com retomada.
 *
 * Este arquivo nao sabe nada sobre Xbox. Ele grava em caminhos que o chamador
 * fornece, via stdio. A traducao de "onde isso vai parar no console" fica na
 * camada de plataforma — e por isso o mesmo codigo roda no PC e no 360.
 */
#ifndef EPTX_CLIENT_H
#define EPTX_CLIENT_H

#include "eptx_types.h"
#include "eptx_sign.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPTX_OK                 0
#define EPTX_ERR_HTTP          -10
#define EPTX_ERR_STATUS        -11  /* servidor respondeu com erro; veja last_status */
#define EPTX_ERR_JSON          -12
#define EPTX_ERR_NO_SESSION    -13
#define EPTX_ERR_BUFFER        -14  /* resposta maior que o buffer fornecido */
#define EPTX_ERR_FILE          -15
#define EPTX_ERR_SIGN          -16
#define EPTX_ERR_SIZE_MISMATCH -17  /* arquivo local maior que o esperado */

typedef struct {
    char    host[64];
    int     port;

    char    token[96];
    char    secret[EPTX_SIGNATURE_LEN];
    char    gamertag[40];
    int64_t user_id;
    int     is_admin;

    /* Segundos a somar no relogio local para chegar no horario do servidor. */
    int64_t clock_offset;
    uint32_t nonce_counter;

    /* Diagnostico da ultima requisicao. */
    int     last_status;
    char    last_error[128];
} eptx_client;

typedef void (*eptx_progress_cb)(void *user, int64_t done, int64_t total);

void eptx_client_init(eptx_client *c, const char *host, int port);

/*
 * Le o horario do servidor e guarda a diferenca. Obrigatorio antes do primeiro
 * request assinado: o relogio do 360 costuma estar errado, e o servidor recusa
 * assinatura fora da janela de tolerancia.
 */
int eptx_client_sync_clock(eptx_client *c);

int eptx_client_login(eptx_client *c, const char *gamertag, const char *password, const char *hw_id);

/*
 * Pareamento: o console mostra user_code na TV e fica perguntando ao servidor se
 * ja aprovaram. Evita digitar senha no controle.
 */
int eptx_client_device_start(eptx_client *c, const char *hw_id,
                             char *out_device_code, size_t dc_cap,
                             char *out_user_code, size_t uc_cap);
/* Devolve EPTX_OK quando aprovado, 1 quando ainda pendente, negativo em erro. */
int eptx_client_device_poll(eptx_client *c, const char *device_code);

/* Requisicoes assinadas. out recebe o corpo da resposta terminado em '\0'. */
int eptx_client_get(eptx_client *c, const char *path, char *out, size_t cap, int *out_len);
int eptx_client_post(eptx_client *c, const char *path, const char *json_body,
                     char *out, size_t cap, int *out_len);

/*
 * Baixa um arquivo do catalogo para dest_path, retomando se ja existir parte dele.
 * expected_size vem do catalogo; passe 0 se nao souber.
 *
 * Faz ate max_retries tentativas, cada uma retomando de onde parou. E o
 * comportamento que importa numa rede domestica com um Wi-Fi de 2008.
 */
int eptx_client_download(eptx_client *c, int64_t file_id, const char *dest_path,
                         int64_t expected_size, int max_retries,
                         eptx_progress_cb on_progress, void *user);

/* Cria a arvore de diretorios de um caminho de arquivo. */
int eptx_mkdir_for_file(const char *file_path);

#ifdef __cplusplus
}
#endif

#endif /* EPTX_CLIENT_H */
