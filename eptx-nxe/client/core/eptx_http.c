#include "eptx_http.h"
#include <string.h>
#include <stdio.h>

#if defined(_XBOX)
  #include <xtl.h>
  #include <winsockx.h>
  typedef int socklen_t;
  #define EPTX_CLOSESOCKET closesocket
#elif defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define EPTX_CLOSESOCKET closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <sys/time.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define EPTX_CLOSESOCKET close
#endif

#define HEADER_BUF_SIZE 4096
#define BODY_CHUNK_SIZE 8192
#define DEFAULT_TIMEOUT_MS 30000

const char *eptx_http_strerror(int code)
{
    switch (code) {
    case EPTX_HTTP_OK:            return "ok";
    case EPTX_HTTP_ERR_INIT:      return "rede nao inicializada";
    case EPTX_HTTP_ERR_RESOLVE:   return "nao resolveu o endereco do servidor";
    case EPTX_HTTP_ERR_CONNECT:   return "nao conectou no servidor";
    case EPTX_HTTP_ERR_SEND:      return "falha ao enviar a requisicao";
    case EPTX_HTTP_ERR_RECV:      return "conexao caiu durante a resposta";
    case EPTX_HTTP_ERR_PROTOCOL:  return "resposta HTTP malformada";
    case EPTX_HTTP_ERR_HEADERS:   return "cabecalhos grandes demais";
    case EPTX_HTTP_ERR_ABORTED:   return "transferencia cancelada";
    default:                      return "erro desconhecido";
    }
}

int eptx_http_init(void)
{
#if defined(_XBOX)
    XNetStartupParams xnsp;
    WSADATA wsa;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(XNetStartupParams);
    /*
     * O EPTX nao usa nada da rede segura do LIVE: sem criptografia por titulo,
     * sem checagem de par. Desligar isso evita que o XNet tente montar a stack
     * segura, que e justamente a parte que fala com a Microsoft.
     */
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0) return EPTX_HTTP_ERR_INIT;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return EPTX_HTTP_ERR_INIT;
    return EPTX_HTTP_OK;
#elif defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return EPTX_HTTP_ERR_INIT;
    return EPTX_HTTP_OK;
#else
    return EPTX_HTTP_OK;
#endif
}

void eptx_http_shutdown(void)
{
#if defined(_XBOX)
    WSACleanup();
    XNetCleanup();
#elif defined(_WIN32)
    WSACleanup();
#endif
}

static int resolve_host(const char *host, struct sockaddr_in *addr, int port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);

    /* IP literal primeiro: e o caso normal, e nao depende de DNS nenhum. */
#if defined(__XSDK__)
    /* O xsdk nao traz inet_addr; inet_pton faz o mesmo para IPv4. */
    if (inet_pton(AF_INET, host, &addr->sin_addr) == 1) return EPTX_HTTP_OK;
#else
    {
        unsigned long ip = inet_addr(host);
        if (ip != INADDR_NONE) {
            addr->sin_addr.s_addr = ip;
            return EPTX_HTTP_OK;
        }
    }
#endif

#if defined(_XBOX) || defined(__XSDK__)
    /* No console nao ha gethostbyname utilizavel; exigimos IP. */
    return EPTX_HTTP_ERR_RESOLVE;
#else
    {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) return EPTX_HTTP_ERR_RESOLVE;
        memcpy(&addr->sin_addr, he->h_addr_list[0], sizeof(addr->sin_addr));
        return EPTX_HTTP_OK;
    }
#endif
}

static void set_timeout(SOCKET s, int timeout_ms)
{
#if defined(_WIN32) || defined(_XBOX)
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static int send_all(SOCKET s, const char *data, size_t len)
{
    while (len > 0) {
        int n = send(s, data, (int)len, 0);
        if (n <= 0) return EPTX_HTTP_ERR_SEND;
        data += n;
        len -= (size_t)n;
    }
    return EPTX_HTTP_OK;
}

/* Comparacao de nome de cabecalho, que e case-insensitive por especificacao. */
static int header_is(const char *line, const char *name)
{
    size_t i = 0;
    while (name[i]) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        i++;
    }
    return line[i] == ':';
}

static const char *header_value(const char *line, const char *name)
{
    const char *p = line + strlen(name) + 1;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int64_t parse_int64(const char *s)
{
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

int eptx_http_perform(const char *host, int port,
                      const eptx_http_request *req,
                      eptx_http_response *res,
                      eptx_http_sink sink, void *user)
{
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in addr;
    char header_buf[HEADER_BUF_SIZE];
    char request_head[1024];
    size_t header_len = 0;
    const char *body_start;
    size_t body_in_buf;
    int rc, chunked = 0;
    int64_t remaining;
    int head_end = -1;
    size_t i;

    if (!host || !req || !res) return EPTX_HTTP_ERR_PROTOCOL;

    memset(res, 0, sizeof(*res));
    res->status = 0;
    res->content_length = -1;
    res->range_total = -1;
    res->install_path[0] = '\0';

    rc = resolve_host(host, &addr, port);
    if (rc != EPTX_HTTP_OK) return rc;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return EPTX_HTTP_ERR_CONNECT;

    set_timeout(sock, req->timeout_ms > 0 ? req->timeout_ms : DEFAULT_TIMEOUT_MS);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        EPTX_CLOSESOCKET(sock);
        return EPTX_HTTP_ERR_CONNECT;
    }

    /*
     * Connection: close simplifica muito o cliente — sem keep-alive nao ha estado
     * de conexao para gerenciar, e o custo de um handshake TCP por requisicao e
     * irrelevante perto do tempo de baixar centenas de MB.
     */
    sprintf(request_head,
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "User-Agent: EPTX-Live/0.1 (Xbox 360)\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n",
            req->method, req->path, host, port);

    rc = send_all(sock, request_head, strlen(request_head));
    if (rc != EPTX_HTTP_OK) goto fail;

    if (req->headers && req->headers[0]) {
        rc = send_all(sock, req->headers, strlen(req->headers));
        if (rc != EPTX_HTTP_OK) goto fail;
    }

    if (req->body_len > 0) {
        char len_line[64];
        sprintf(len_line, "Content-Length: %lu\r\n", (unsigned long)req->body_len);
        rc = send_all(sock, len_line, strlen(len_line));
        if (rc != EPTX_HTTP_OK) goto fail;
    }

    rc = send_all(sock, "\r\n", 2);
    if (rc != EPTX_HTTP_OK) goto fail;

    if (req->body_len > 0) {
        rc = send_all(sock, (const char *)req->body, req->body_len);
        if (rc != EPTX_HTTP_OK) goto fail;
    }

    /* Le ate encontrar a linha em branco que separa cabecalhos do corpo. */
    for (;;) {
        int n;
        if (header_len >= sizeof(header_buf)) {
            rc = EPTX_HTTP_ERR_HEADERS;
            goto fail;
        }
        n = recv(sock, header_buf + header_len, (int)(sizeof(header_buf) - header_len), 0);
        if (n <= 0) {
            rc = EPTX_HTTP_ERR_RECV;
            goto fail;
        }
        header_len += (size_t)n;

        for (i = 3; i < header_len; i++) {
            if (header_buf[i - 3] == '\r' && header_buf[i - 2] == '\n' &&
                header_buf[i - 1] == '\r' && header_buf[i] == '\n') {
                head_end = (int)i + 1;
                break;
            }
        }
        if (head_end >= 0) break;
    }

    /* Linha de status: "HTTP/1.1 206 Partial Content" */
    if (header_len < 12 || memcmp(header_buf, "HTTP/1.", 7) != 0) {
        rc = EPTX_HTTP_ERR_PROTOCOL;
        goto fail;
    }
    res->status = (int)parse_int64(header_buf + 9);
    if (res->status == 0) {
        rc = EPTX_HTTP_ERR_PROTOCOL;
        goto fail;
    }

    /* Percorre as linhas de cabecalho, terminando cada uma em '\0' no lugar. */
    {
        char *line = header_buf;
        char *end = header_buf + head_end;

        while (line < end) {
            char *eol = line;
            while (eol < end && *eol != '\r') eol++;
            *eol = '\0';

            if (header_is(line, "Content-Length")) {
                res->content_length = parse_int64(header_value(line, "Content-Length"));
            } else if (header_is(line, "Transfer-Encoding")) {
                const char *v = header_value(line, "Transfer-Encoding");
                if (strstr(v, "chunked") || strstr(v, "Chunked")) chunked = 1;
            } else if (header_is(line, "X-EPTX-Install-Path")) {
                const char *v = header_value(line, "X-EPTX-Install-Path");
                size_t vl = strlen(v);
                if (vl >= sizeof(res->install_path)) vl = sizeof(res->install_path) - 1;
                memcpy(res->install_path, v, vl);
                res->install_path[vl] = '\0';
            } else if (header_is(line, "Content-Range")) {
                /* "bytes 1024-2047/4096" — o que interessa e o total depois da barra. */
                const char *slash = strchr(header_value(line, "Content-Range"), '/');
                if (slash) res->range_total = parse_int64(slash + 1);
            }

            line = eol + 2; /* pula o \r\n; a ultima linha cai fora do laco */
            if (line >= end) break;
        }
    }

    /*
     * head_end e o indice do primeiro byte do corpo: o laco de busca guarda i+1,
     * onde i e a posicao do '\n' final de "\r\n\r\n". Somar 1 de novo aqui comeria
     * o primeiro byte do corpo — que num JSON e justamente a chave de abertura.
     */
    body_start = header_buf + head_end;
    body_in_buf = header_len - (size_t)head_end;

    if (!sink) {
        /* Chamador so queria os cabecalhos. */
        rc = EPTX_HTTP_OK;
        goto done;
    }

    if (chunked) {
        /*
         * O servidor do EPTX sempre manda Content-Length, mas um proxy no meio
         * pode reescrever para chunked. Decodificamos em vez de entregar o corpo
         * com os marcadores de tamanho embutidos, que corromperia o arquivo em
         * silencio — o pior tipo de falha aqui.
         */
        char acc[BODY_CHUNK_SIZE * 2];
        size_t acc_len = 0;

        if (body_in_buf > sizeof(acc)) { rc = EPTX_HTTP_ERR_PROTOCOL; goto fail; }
        memcpy(acc, body_start, body_in_buf);
        acc_len = body_in_buf;

        for (;;) {
            size_t j, line_end = 0;
            int64_t chunk_size;
            int found = 0;

            for (j = 1; j < acc_len; j++) {
                if (acc[j - 1] == '\r' && acc[j] == '\n') { line_end = j + 1; found = 1; break; }
            }
            if (!found) {
                int n;
                if (acc_len >= sizeof(acc)) { rc = EPTX_HTTP_ERR_PROTOCOL; goto fail; }
                n = recv(sock, acc + acc_len, (int)(sizeof(acc) - acc_len), 0);
                if (n <= 0) { rc = EPTX_HTTP_ERR_RECV; goto fail; }
                acc_len += (size_t)n;
                continue;
            }

            chunk_size = 0;
            for (j = 0; j < line_end; j++) {
                char c = acc[j];
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                chunk_size = chunk_size * 16 + d;
            }

            memmove(acc, acc + line_end, acc_len - line_end);
            acc_len -= line_end;

            if (chunk_size == 0) { rc = EPTX_HTTP_OK; goto done; }

            while (chunk_size > 0) {
                size_t take;
                if (acc_len == 0) {
                    int n = recv(sock, acc, (int)sizeof(acc), 0);
                    if (n <= 0) { rc = EPTX_HTTP_ERR_RECV; goto fail; }
                    acc_len = (size_t)n;
                }
                take = acc_len;
                if ((int64_t)take > chunk_size) take = (size_t)chunk_size;
                if (sink(user, (const uint8_t *)acc, take) != 0) { rc = EPTX_HTTP_ERR_ABORTED; goto fail; }
                memmove(acc, acc + take, acc_len - take);
                acc_len -= take;
                chunk_size -= (int64_t)take;
            }

            /* CRLF que fecha o chunk. */
            while (acc_len < 2) {
                int n = recv(sock, acc + acc_len, (int)(sizeof(acc) - acc_len), 0);
                if (n <= 0) { rc = EPTX_HTTP_ERR_RECV; goto fail; }
                acc_len += (size_t)n;
            }
            memmove(acc, acc + 2, acc_len - 2);
            acc_len -= 2;
        }
    }

    if (body_in_buf > 0) {
        if (sink(user, (const uint8_t *)body_start, body_in_buf) != 0) {
            rc = EPTX_HTTP_ERR_ABORTED;
            goto fail;
        }
    }

    remaining = (res->content_length >= 0)
              ? res->content_length - (int64_t)body_in_buf
              : -1;

    while (remaining != 0) {
        uint8_t chunk[BODY_CHUNK_SIZE];
        int want = (int)sizeof(chunk);
        int n;

        if (remaining > 0 && remaining < (int64_t)want) want = (int)remaining;

        n = recv(sock, (char *)chunk, want, 0);
        if (n == 0) break;             /* servidor fechou: fim do corpo */
        if (n < 0) {
            /* Sem Content-Length, fechar a conexao e o unico sinal de fim. */
            if (remaining < 0) break;
            rc = EPTX_HTTP_ERR_RECV;
            goto fail;
        }

        if (sink(user, chunk, (size_t)n) != 0) {
            rc = EPTX_HTTP_ERR_ABORTED;
            goto fail;
        }
        if (remaining > 0) remaining -= n;
    }

    rc = EPTX_HTTP_OK;

done:
    EPTX_CLOSESOCKET(sock);
    return rc;

fail:
    EPTX_CLOSESOCKET(sock);
    return rc;
}
