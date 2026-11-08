/*
 * Cliente de linha de comando do EPTX, para rodar no PC.
 *
 * Usa exatamente o mesmo nucleo que vai rodar no console: mesma assinatura, mesmo
 * parser JSON, mesmo motor de download. A unica coisa que muda no Xbox e para onde
 * os arquivos vao e quem desenha a tela.
 *
 *   eptx --host 192.168.0.10 --user Rodrigo --pass senha --list
 *   eptx --host 192.168.0.10 --user Rodrigo --pass senha --get 2 --out ./baixados
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "eptx_client.h"
#include "eptx_http.h"
#include "eptx_json.h"

#define RESPONSE_CAP (256 * 1024)
#define MAX_TOKENS   4096

static char g_response[RESPONSE_CAP];
static eptx_json_tok g_tokens[MAX_TOKENS];

static const char *arg_value(int argc, char **argv, const char *name, const char *fallback)
{
    int i;
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return fallback;
}

static int arg_present(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) return 1;
    }
    return 0;
}

static void human_size(int64_t bytes, char *out)
{
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (gb >= 1.0) sprintf(out, "%.2f GB", gb);
    else           sprintf(out, "%.1f MB", mb);
}

static void on_progress(void *user, int64_t done, int64_t total)
{
    const char *name = (const char *)user;
    int percent = (total > 0) ? (int)((done * 100) / total) : 0;
    int filled = percent / 2;
    char bar[51];
    int i;

    for (i = 0; i < 50; i++) bar[i] = (i < filled) ? '#' : '.';
    bar[50] = '\0';

    printf("\r  [%s] %3d%%  %s", bar, percent, name);
    fflush(stdout);
}

/*
 * Traduz o caminho de instalacao do console para um caminho local.
 * "Hdd1:\Content\0000000000000000\..." vira "<out>/Content/0000000000000000/...".
 * No console esta funcao nao existe: o caminho ja vem pronto do servidor.
 */
static void install_path_to_local(const char *install_path, const char *out_dir, char *out, size_t cap)
{
    const char *p = install_path;
    size_t w;

    /* Pula o prefixo de dispositivo ("Hdd1:"). */
    {
        const char *colon = strchr(p, ':');
        if (colon) p = colon + 1;
        while (*p == '\\' || *p == '/') p++;
    }

    w = (size_t)sprintf(out, "%s/", out_dir);
    while (*p && w < cap - 1) {
        out[w++] = (*p == '\\') ? '/' : *p;
        p++;
    }
    out[w] = '\0';
}

static int cmd_list(eptx_client *c)
{
    int len = 0, n, i, count = 0;
    int items;
    int64_t total = 0;

    if (eptx_client_get(c, "/api/v1/catalog?limit=200", g_response, RESPONSE_CAP, &len) != EPTX_OK) {
        printf("Falhou ao listar: %s\n", c->last_error);
        return 1;
    }

    n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
    if (n < 1) {
        printf("JSON invalido no catalogo.\n");
        return 1;
    }

    eptx_json_get_int(g_response, g_tokens, n, 0, "total", &total);
    items = eptx_json_get(g_response, g_tokens, n, 0, "items");
    if (items < 0) {
        printf("Catalogo sem 'items'.\n");
        return 1;
    }

    printf("Catalogo (%ld titulos):\n\n", (long)total);

    for (i = 0; ; i++) {
        int item = eptx_json_at(g_tokens, n, items, i);
        int64_t id = 0, size = 0;
        char name[128], kind[16], title_id[16];
        char size_text[32];
        int shows = 0;

        if (item < 0) break;

        eptx_json_get_int(g_response, g_tokens, n, item, "id", &id);
        eptx_json_get_int(g_response, g_tokens, n, item, "totalSize", &size);
        name[0] = kind[0] = title_id[0] = '\0';
        eptx_json_get_string(g_response, g_tokens, n, item, "name", name, sizeof(name));
        eptx_json_get_string(g_response, g_tokens, n, item, "kind", kind, sizeof(kind));
        eptx_json_get_string(g_response, g_tokens, n, item, "titleId", title_id, sizeof(title_id));
        eptx_json_get_bool(g_response, g_tokens, n, item, "showsInOriginalDash", &shows);

        human_size(size, size_text);
        printf("  #%-4ld %s  %-9s %-6s %10s  %s\n",
               (long)id, title_id, shows ? "dash OK" : "-", kind, size_text, name);
        count++;
    }

    printf("\n%d titulo(s) listado(s).\n", count);
    return 0;
}

static int cmd_get(eptx_client *c, int64_t title_id, const char *out_dir)
{
    char path[64];
    int len = 0, n, i;
    int files;
    char title_name[128];

    sprintf(path, "/api/v1/catalog/%ld", (long)title_id);
    if (eptx_client_get(c, path, g_response, RESPONSE_CAP, &len) != EPTX_OK) {
        printf("Falhou ao buscar o titulo: %s\n", c->last_error);
        return 1;
    }

    n = eptx_json_parse(g_response, (size_t)len, g_tokens, MAX_TOKENS);
    if (n < 1) {
        printf("JSON invalido no detalhe do titulo.\n");
        return 1;
    }

    title_name[0] = '\0';
    eptx_json_get_string(g_response, g_tokens, n, 0, "name", title_name, sizeof(title_name));

    files = eptx_json_get(g_response, g_tokens, n, 0, "files");
    if (files < 0) {
        printf("Titulo sem lista de arquivos.\n");
        return 1;
    }

    printf("Baixando \"%s\"\n\n", title_name);

    for (i = 0; ; i++) {
        int file = eptx_json_at(g_tokens, n, files, i);
        int64_t file_id = 0, size = 0;
        char install_path[300];
        char local_path[512];
        char label[64];
        int rc;

        if (file < 0) break;

        eptx_json_get_int(g_response, g_tokens, n, file, "id", &file_id);
        eptx_json_get_int(g_response, g_tokens, n, file, "size", &size);
        install_path[0] = '\0';
        eptx_json_get_string(g_response, g_tokens, n, file, "installPath", install_path, sizeof(install_path));

        if (!install_path[0]) {
            printf("  arquivo #%ld nao tem caminho de instalacao; pulando.\n", (long)file_id);
            continue;
        }

        install_path_to_local(install_path, out_dir, local_path, sizeof(local_path));

        /* Nome curto so para a barra de progresso. */
        {
            const char *slash = strrchr(local_path, '/');
            const char *base = slash ? slash + 1 : local_path;
            strncpy(label, base, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }

        rc = eptx_client_download(c, file_id, local_path, size, 3, on_progress, label);
        printf("\n");

        if (rc != EPTX_OK) {
            printf("  Falhou: %s\n", c->last_error);
            return 1;
        }
    }

    printf("\nConcluido. Arquivos em: %s\n", out_dir);
    printf("No console, o destino seria Hdd1:\\Content\\0000000000000000\\...\n");
    return 0;
}

int main(int argc, char **argv)
{
    eptx_client client;
    const char *host = arg_value(argc, argv, "--host", "127.0.0.1");
    const char *port_text = arg_value(argc, argv, "--port", "8360");
    const char *user = arg_value(argc, argv, "--user", 0);
    const char *pass = arg_value(argc, argv, "--pass", 0);
    const char *out_dir = arg_value(argc, argv, "--out", "./baixados");
    int rc = 0;

    if (!user || !pass) {
        printf("Uso:\n");
        printf("  eptx --host <ip> --user <gamertag> --pass <senha> --list\n");
        printf("  eptx --host <ip> --user <gamertag> --pass <senha> --get <id> --out <pasta>\n");
        return 1;
    }

    if (eptx_http_init() != EPTX_HTTP_OK) {
        printf("Nao consegui inicializar a rede.\n");
        return 1;
    }

    eptx_client_init(&client, host, atoi(port_text));

    printf("Servidor: %s:%d\n", host, atoi(port_text));

    if (eptx_client_sync_clock(&client) != EPTX_OK) {
        printf("Nao alinhou o relogio: %s\n", client.last_error);
        eptx_http_shutdown();
        return 1;
    }
    printf("Relogio alinhado (diferenca de %ld s)\n", (long)client.clock_offset);

    if (eptx_client_login(&client, user, pass, "PC-TESTE") != EPTX_OK) {
        printf("Login falhou: %s\n", client.last_error);
        eptx_http_shutdown();
        return 1;
    }
    printf("Logado como %s%s\n\n", client.gamertag, client.is_admin ? " (admin)" : "");

    if (arg_present(argc, argv, "--list")) {
        rc = cmd_list(&client);
    } else if (arg_present(argc, argv, "--get")) {
        rc = cmd_get(&client, atol(arg_value(argc, argv, "--get", "0")), out_dir);
    } else {
        printf("Nada a fazer. Use --list ou --get <id>.\n");
        rc = 1;
    }

    eptx_http_shutdown();
    return rc;
}
