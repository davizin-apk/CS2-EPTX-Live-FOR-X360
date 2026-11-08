#include "eptx_client.h"
#include "eptx_http.h"
#include "eptx_json.h"
#include "eptx_sha256.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32) || defined(_XBOX)
  #include <direct.h>
  #define EPTX_MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #define EPTX_MKDIR(p) mkdir(p, 0777)
#endif

#define MAX_TOKENS 512

/* --- coleta da resposta em memoria ---------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
} mem_sink;

static int sink_to_memory(void *user, const uint8_t *data, size_t len)
{
    mem_sink *m = (mem_sink *)user;
    if (m->len + len + 1 > m->cap) {
        m->overflow = 1;
        return 1; /* aborta: nao adianta continuar baixando o que nao cabe */
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

/* --- gravacao em arquivo --------------------------------------------------- */

typedef struct {
    FILE            *fp;
    const char      *path;
    const eptx_http_response *res;
    int              started;
    int64_t          written;
    int64_t          total;
    eptx_progress_cb on_progress;
    void            *user;
    int64_t          last_reported;
} file_sink;

static int sink_to_file(void *user, const uint8_t *data, size_t len)
{
    file_sink *f = (file_sink *)user;

    /*
     * Pedimos Range, mas um 200 quer dizer que o servidor ignorou e mandou o
     * arquivo inteiro. Anexar isso ao pedaco que ja existe daria um arquivo com o
     * comeco repetido, entao recomecamos do zero.
     */
    if (!f->started) {
        f->started = 1;
        if (f->res->status == 200 && f->written > 0) {
            fclose(f->fp);
            f->fp = fopen(f->path, "wb");
            if (!f->fp) return 1;
            f->written = 0;
            f->last_reported = 0;
        }
    }

    if (fwrite(data, 1, len, f->fp) != len) return 1;
    f->written += (int64_t)len;

    /* Relatar a cada 256 KB evita inundar a UI sem perder resolucao na barra. */
    if (f->on_progress && (f->written - f->last_reported) >= 256 * 1024) {
        f->last_reported = f->written;
        f->on_progress(f->user, f->written, f->total);
    }
    return 0;
}

/* --- infraestrutura -------------------------------------------------------- */

void eptx_client_init(eptx_client *c, const char *host, int port)
{
    memset(c, 0, sizeof(*c));
    strncpy(c->host, host, sizeof(c->host) - 1);
    c->port = port;
    c->clock_offset = 0;
    c->nonce_counter = 0;
}

static int64_t client_now(const eptx_client *c)
{
    return (int64_t)time(0) + c->clock_offset;
}

/*
 * Nonce unico por requisicao. Derivamos de token + horario + contador em vez de
 * usar rand(): o servidor rejeita nonce repetido, e rand() sem semente decente
 * (o que o console tem) colidiria depois de poucas centenas de chamadas.
 */
static void make_nonce(eptx_client *c, char out[33])
{
    eptx_sha256_ctx ctx;
    uint8_t digest[EPTX_SHA256_DIGEST_SIZE];
    char scratch[160];
    int64_t t = client_now(c);

    c->nonce_counter++;
    sprintf(scratch, "%s|%ld|%lu|%lu",
            c->token[0] ? c->token : "sem-sessao",
            (long)t,
            (unsigned long)c->nonce_counter,
            (unsigned long)(size_t)c);

    eptx_sha256_init(&ctx);
    eptx_sha256_update(&ctx, scratch, strlen(scratch));
    eptx_sha256_final(&ctx, digest);

    eptx_to_hex(digest, 16, out);
}

static void set_error(eptx_client *c, const char *msg)
{
    strncpy(c->last_error, msg, sizeof(c->last_error) - 1);
    c->last_error[sizeof(c->last_error) - 1] = '\0';
}

/* Requisicao sem assinatura: horario, login, pareamento. */
static int request_open(eptx_client *c, const char *method, const char *path,
                        const char *body, char *out, size_t cap, int *out_len)
{
    eptx_http_request req;
    eptx_http_response res;
    mem_sink sink;
    int rc;

    memset(&req, 0, sizeof(req));
    req.method = method;
    req.path = path;
    req.headers = body ? "Content-Type: application/json\r\n" : 0;
    req.body = body;
    req.body_len = body ? strlen(body) : 0;

    sink.buf = out;
    sink.cap = cap;
    sink.len = 0;
    sink.overflow = 0;

    rc = eptx_http_perform(c->host, c->port, &req, &res, sink_to_memory, &sink);
    if (rc != EPTX_HTTP_OK) {
        set_error(c, eptx_http_strerror(rc));
        return EPTX_ERR_HTTP;
    }
    if (sink.overflow) {
        set_error(c, "resposta maior que o buffer");
        return EPTX_ERR_BUFFER;
    }

    out[sink.len] = '\0';
    if (out_len) *out_len = (int)sink.len;
    c->last_status = res.status;

    if (res.status < 200 || res.status >= 300) {
        set_error(c, out);
        return EPTX_ERR_STATUS;
    }
    return EPTX_OK;
}

/* Requisicao assinada. */
static int request_signed(eptx_client *c, const char *method, const char *path,
                          const char *body, char *out, size_t cap, int *out_len)
{
    eptx_http_request req;
    eptx_http_response res;
    mem_sink sink;
    char headers[512];
    char nonce[33];
    char signature[EPTX_SIGNATURE_LEN];
    char timestamp[24];
    int rc;

    if (!c->token[0] || !c->secret[0]) {
        set_error(c, "sem sessao: faca login antes");
        return EPTX_ERR_NO_SESSION;
    }

    sprintf(timestamp, "%ld", (long)client_now(c));
    make_nonce(c, nonce);

    rc = eptx_sign_request(c->secret, method, path, timestamp, nonce,
                           body, body ? strlen(body) : 0, signature);
    if (rc != EPTX_SIGN_OK) {
        set_error(c, "falha ao assinar a requisicao");
        return EPTX_ERR_SIGN;
    }

    sprintf(headers,
            "X-EPTX-Session: %s\r\n"
            "X-EPTX-Timestamp: %s\r\n"
            "X-EPTX-Nonce: %s\r\n"
            "X-EPTX-Signature: %s\r\n"
            "%s",
            c->token, timestamp, nonce, signature,
            body ? "Content-Type: application/json\r\n" : "");

    memset(&req, 0, sizeof(req));
    req.method = method;
    req.path = path;
    req.headers = headers;
    req.body = body;
    req.body_len = body ? strlen(body) : 0;
    /* Long-poll do chat segura ate 25s do lado do servidor. */
    req.timeout_ms = 40000;

    sink.buf = out;
    sink.cap = cap;
    sink.len = 0;
    sink.overflow = 0;

    rc = eptx_http_perform(c->host, c->port, &req, &res, sink_to_memory, &sink);
    if (rc != EPTX_HTTP_OK) {
        set_error(c, eptx_http_strerror(rc));
        return EPTX_ERR_HTTP;
    }
    if (sink.overflow) {
        set_error(c, "resposta maior que o buffer");
        return EPTX_ERR_BUFFER;
    }

    out[sink.len] = '\0';
    if (out_len) *out_len = (int)sink.len;
    c->last_status = res.status;

    if (res.status < 200 || res.status >= 300) {
        set_error(c, out);
        return EPTX_ERR_STATUS;
    }
    return EPTX_OK;
}

int eptx_client_get(eptx_client *c, const char *path, char *out, size_t cap, int *out_len)
{
    return request_signed(c, "GET", path, 0, out, cap, out_len);
}

int eptx_client_post(eptx_client *c, const char *path, const char *json_body,
                     char *out, size_t cap, int *out_len)
{
    return request_signed(c, "POST", path, json_body, out, cap, out_len);
}

int eptx_client_sync_clock(eptx_client *c)
{
    char buf[256];
    eptx_json_tok toks[16];
    int n, rc;
    int64_t server_time = 0;

    rc = request_open(c, "GET", "/api/v1/time", 0, buf, sizeof(buf), 0);
    if (rc != EPTX_OK) return rc;

    n = eptx_json_parse(buf, strlen(buf), toks, 16);
    if (n < 1) { set_error(c, "JSON invalido em /time"); return EPTX_ERR_JSON; }
    if (eptx_json_get_int(buf, toks, n, 0, "serverTime", &server_time) != 0) {
        set_error(c, "serverTime ausente");
        return EPTX_ERR_JSON;
    }

    c->clock_offset = server_time - (int64_t)time(0);
    return EPTX_OK;
}

/* Preenche a sessao a partir de uma resposta de login ou de pareamento. */
static int adopt_session(eptx_client *c, const char *json, int len)
{
    eptx_json_tok toks[MAX_TOKENS];
    int n = eptx_json_parse(json, (size_t)len, toks, MAX_TOKENS);
    int is_admin = 0;

    if (n < 1) { set_error(c, "JSON invalido na sessao"); return EPTX_ERR_JSON; }

    if (eptx_json_get_string(json, toks, n, 0, "token", c->token, sizeof(c->token)) != 0 ||
        eptx_json_get_string(json, toks, n, 0, "secret", c->secret, sizeof(c->secret)) != 0) {
        set_error(c, "resposta sem token/secret");
        return EPTX_ERR_JSON;
    }

    eptx_json_get_string(json, toks, n, 0, "gamertag", c->gamertag, sizeof(c->gamertag));
    eptx_json_get_int(json, toks, n, 0, "userId", &c->user_id);
    if (eptx_json_get_bool(json, toks, n, 0, "isAdmin", &is_admin) == 0) c->is_admin = is_admin;

    return EPTX_OK;
}

int eptx_client_login(eptx_client *c, const char *gamertag, const char *password, const char *hw_id)
{
    char body[512];
    char buf[2048];
    int len = 0, rc;

    sprintf(body, "{\"gamertag\":\"%s\",\"password\":\"%s\",\"hwId\":\"%s\"}",
            gamertag, password, hw_id ? hw_id : "");

    rc = request_open(c, "POST", "/api/v1/auth/login", body, buf, sizeof(buf), &len);
    if (rc != EPTX_OK) return rc;

    return adopt_session(c, buf, len);
}

int eptx_client_device_start(eptx_client *c, const char *hw_id,
                             char *out_device_code, size_t dc_cap,
                             char *out_user_code, size_t uc_cap)
{
    char body[192];
    char buf[1024];
    eptx_json_tok toks[64];
    int len = 0, n, rc;

    sprintf(body, "{\"hwId\":\"%s\"}", hw_id);
    rc = request_open(c, "POST", "/api/v1/auth/device/start", body, buf, sizeof(buf), &len);
    if (rc != EPTX_OK) return rc;

    n = eptx_json_parse(buf, (size_t)len, toks, 64);
    if (n < 1) { set_error(c, "JSON invalido no pareamento"); return EPTX_ERR_JSON; }

    if (eptx_json_get_string(buf, toks, n, 0, "deviceCode", out_device_code, dc_cap) != 0 ||
        eptx_json_get_string(buf, toks, n, 0, "userCode", out_user_code, uc_cap) != 0) {
        set_error(c, "resposta de pareamento incompleta");
        return EPTX_ERR_JSON;
    }
    return EPTX_OK;
}

int eptx_client_device_poll(eptx_client *c, const char *device_code)
{
    char body[192];
    char buf[2048];
    eptx_json_tok toks[MAX_TOKENS];
    char status[32];
    int len = 0, n, rc;

    sprintf(body, "{\"deviceCode\":\"%s\"}", device_code);
    rc = request_open(c, "POST", "/api/v1/auth/device/poll", body, buf, sizeof(buf), &len);
    if (rc != EPTX_OK) return rc;

    n = eptx_json_parse(buf, (size_t)len, toks, MAX_TOKENS);
    if (n < 1) { set_error(c, "JSON invalido no poll"); return EPTX_ERR_JSON; }

    if (eptx_json_get_string(buf, toks, n, 0, "status", status, sizeof(status)) == 0 &&
        strcmp(status, "pending") == 0) {
        return 1;
    }

    return adopt_session(c, buf, len);
}

int eptx_mkdir_for_file(const char *file_path)
{
    char path[512];
    size_t i, len;
    int last_sep = -1;

    len = strlen(file_path);
    if (len >= sizeof(path)) return EPTX_ERR_FILE;
    strcpy(path, file_path);

    for (i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') last_sep = (int)i;
    }
    if (last_sep <= 0) return EPTX_OK; /* arquivo na raiz, nada a criar */

    for (i = 1; i <= (size_t)last_sep; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            char saved = path[i];
            path[i] = '\0';
            /* Erro e ignorado: quase sempre significa "ja existe". */
            EPTX_MKDIR(path);
            path[i] = saved;
        }
    }
    path[last_sep] = '\0';
    EPTX_MKDIR(path);
    return EPTX_OK;
}

static int64_t file_size_of(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long size;
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fclose(fp);
    return (int64_t)size;
}

int eptx_client_download(eptx_client *c, int64_t file_id, const char *dest_path,
                         int64_t expected_size, int max_retries,
                         eptx_progress_cb on_progress, void *user)
{
    int attempt;
    int64_t offset;

    if (max_retries < 1) max_retries = 1;
    eptx_mkdir_for_file(dest_path);

    for (attempt = 0; attempt < max_retries; attempt++) {
        eptx_http_request req;
        eptx_http_response res;
        file_sink fs;
        char headers[640];
        char nonce[33];
        char signature[EPTX_SIGNATURE_LEN];
        char timestamp[24];
        char path[96];
        int rc;

        offset = file_size_of(dest_path);
        if (offset < 0) offset = 0;

        if (expected_size > 0) {
            if (offset == expected_size) {
                /* Ja esta completo; nada a fazer. */
                if (on_progress) on_progress(user, offset, expected_size);
                return EPTX_OK;
            }
            if (offset > expected_size) {
                /*
                 * Arquivo local maior que o do servidor: o catalogo mudou embaixo
                 * de nos, ou o arquivo esta corrompido. Retomar daqui produziria
                 * lixo, entao paramos e deixamos o chamador decidir.
                 */
                set_error(c, "arquivo local maior que o do servidor; apague e baixe de novo");
                return EPTX_ERR_SIZE_MISMATCH;
            }
        }

        sprintf(path, "/api/v1/download/%ld", (long)file_id);
        sprintf(timestamp, "%ld", (long)client_now(c));
        make_nonce(c, nonce);

        rc = eptx_sign_request(c->secret, "GET", path, timestamp, nonce, 0, 0, signature);
        if (rc != EPTX_SIGN_OK) {
            set_error(c, "falha ao assinar o download");
            return EPTX_ERR_SIGN;
        }

        /*
         * O Range nao entra na assinatura — ela cobre metodo, caminho, horario,
         * nonce e corpo. Isso e o que permite retomar sem reassinar nada alem do
         * novo nonce.
         */
        sprintf(headers,
                "X-EPTX-Session: %s\r\n"
                "X-EPTX-Timestamp: %s\r\n"
                "X-EPTX-Nonce: %s\r\n"
                "X-EPTX-Signature: %s\r\n"
                "Range: bytes=%ld-\r\n",
                c->token, timestamp, nonce, signature, (long)offset);

        fs.fp = fopen(dest_path, offset > 0 ? "ab" : "wb");
        if (!fs.fp) {
            set_error(c, "nao consegui abrir o arquivo de destino");
            return EPTX_ERR_FILE;
        }
        fs.path = dest_path;
        fs.res = &res;
        fs.started = 0;
        fs.written = offset;
        fs.total = expected_size;
        fs.on_progress = on_progress;
        fs.user = user;
        fs.last_reported = offset;

        memset(&req, 0, sizeof(req));
        req.method = "GET";
        req.path = path;
        req.headers = headers;
        req.timeout_ms = 60000;

        rc = eptx_http_perform(c->host, c->port, &req, &res, sink_to_file, &fs);
        if (fs.fp) fclose(fs.fp);

        if (rc == EPTX_HTTP_OK && (res.status == 200 || res.status == 206)) {
            if (on_progress) on_progress(user, fs.written, expected_size > 0 ? expected_size : fs.written);
            if (expected_size <= 0 || fs.written == expected_size) return EPTX_OK;
            /* Veio menos do que devia: tenta de novo, retomando. */
            continue;
        }

        if (rc == EPTX_HTTP_OK) {
            c->last_status = res.status;
            /* 416 aqui significa que o offset ja passou do fim: nada mais a baixar. */
            if (res.status == 416 && expected_size > 0 && offset >= expected_size) return EPTX_OK;
            set_error(c, "servidor recusou o download");
            return EPTX_ERR_STATUS;
        }

        set_error(c, eptx_http_strerror(rc));
        /* Erro de rede: proxima tentativa retoma do que ja foi gravado. */
    }

    return EPTX_ERR_HTTP;
}
