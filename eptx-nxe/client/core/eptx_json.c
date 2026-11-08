#include "eptx_json.h"
#include <string.h>

typedef struct {
    size_t pos;      /* posicao atual no buffer */
    int    next;     /* proximo token livre */
    int    super;    /* token pai corrente, ou -1 */
} json_parser;

static eptx_json_tok *alloc_token(json_parser *p, eptx_json_tok *tokens, int max)
{
    eptx_json_tok *tok;
    if (p->next >= max) return 0;
    tok = &tokens[p->next++];
    tok->start = -1;
    tok->end = -1;
    tok->size = 0;
    tok->parent = -1;
    tok->type = EPTX_JSON_UNDEFINED;
    return tok;
}

/* Numeros, true, false, null: vao ate o proximo delimitador estrutural. */
static int parse_primitive(json_parser *p, const char *js, size_t len,
                           eptx_json_tok *tokens, int max)
{
    eptx_json_tok *tok;
    size_t start = p->pos;

    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        char c = js[p->pos];
        if (c == ',' || c == '}' || c == ']' ||
            c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            break;
        }
        /* Caracteres de controle nao aparecem fora de string. */
        if (c < 32 || c >= 127) {
            p->pos = start;
            return EPTX_JSON_ERR_INVALID;
        }
    }
    if (p->pos == start) {
        return EPTX_JSON_ERR_INVALID;
    }

    tok = alloc_token(p, tokens, max);
    if (!tok) {
        p->pos = start;
        return EPTX_JSON_ERR_NOMEM;
    }
    tok->type = EPTX_JSON_PRIMITIVE;
    tok->start = (int)start;
    tok->end = (int)p->pos;
    tok->parent = p->super;

    /* O laco externo tambem incrementa pos, entao recuamos um. */
    p->pos--;
    return 0;
}

static int parse_string(json_parser *p, const char *js, size_t len,
                        eptx_json_tok *tokens, int max)
{
    eptx_json_tok *tok;
    size_t start = p->pos;

    p->pos++; /* pula a aspa de abertura */

    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        char c = js[p->pos];

        if (c == '"') {
            tok = alloc_token(p, tokens, max);
            if (!tok) {
                p->pos = start;
                return EPTX_JSON_ERR_NOMEM;
            }
            tok->type = EPTX_JSON_STRING;
            tok->start = (int)start + 1;
            tok->end = (int)p->pos;
            tok->parent = p->super;
            return 0;
        }

        if (c == '\\' && p->pos + 1 < len) {
            p->pos++;
            switch (js[p->pos]) {
            case '"': case '/': case '\\': case 'b':
            case 'f': case 'r': case 'n': case 't':
                break;
            case 'u': {
                int i;
                if (p->pos + 4 >= len) {
                    p->pos = start;
                    return EPTX_JSON_ERR_INVALID;
                }
                for (i = 1; i <= 4; i++) {
                    char h = js[p->pos + i];
                    int ok = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
                    if (!ok) {
                        p->pos = start;
                        return EPTX_JSON_ERR_INVALID;
                    }
                }
                p->pos += 4;
                break;
            }
            default:
                p->pos = start;
                return EPTX_JSON_ERR_INVALID;
            }
        }
    }

    p->pos = start;
    return EPTX_JSON_ERR_PARTIAL;
}

int eptx_json_parse(const char *js, size_t len, eptx_json_tok *tokens, int max_tokens)
{
    json_parser p;
    int r, i;
    eptx_json_tok *tok;

    p.pos = 0;
    p.next = 0;
    p.super = -1;

    for (; p.pos < len && js[p.pos] != '\0'; p.pos++) {
        char c = js[p.pos];

        switch (c) {
        case '{':
        case '[':
            tok = alloc_token(&p, tokens, max_tokens);
            if (!tok) return EPTX_JSON_ERR_NOMEM;
            if (p.super != -1) {
                tokens[p.super].size++;
                tok->parent = p.super;
            }
            tok->type = (c == '{') ? EPTX_JSON_OBJECT : EPTX_JSON_ARRAY;
            tok->start = (int)p.pos;
            p.super = p.next - 1;
            break;

        case '}':
        case ']': {
            eptx_json_type type = (c == '}') ? EPTX_JSON_OBJECT : EPTX_JSON_ARRAY;
            /* Fecha o container aberto mais recente. */
            for (i = p.next - 1; i >= 0; i--) {
                tok = &tokens[i];
                if (tok->start != -1 && tok->end == -1) {
                    if (tok->type != type) return EPTX_JSON_ERR_INVALID;
                    tok->end = (int)p.pos + 1;
                    p.super = tok->parent;
                    break;
                }
            }
            if (i == -1) return EPTX_JSON_ERR_INVALID;
            break;
        }

        case '"':
            r = parse_string(&p, js, len, tokens, max_tokens);
            if (r < 0) return r;
            if (p.super != -1) tokens[p.super].size++;
            break;

        case ':':
            /* A chave recem-lida vira pai do proximo valor. */
            p.super = p.next - 1;
            break;

        case ',':
            /* Sai de dentro da chave e volta para o container. */
            if (p.super != -1 &&
                tokens[p.super].type != EPTX_JSON_ARRAY &&
                tokens[p.super].type != EPTX_JSON_OBJECT) {
                p.super = tokens[p.super].parent;
            }
            break;

        case ' ': case '\t': case '\r': case '\n':
            break;

        default:
            r = parse_primitive(&p, js, len, tokens, max_tokens);
            if (r < 0) return r;
            if (p.super != -1) tokens[p.super].size++;
            break;
        }
    }

    /* Container sem fechamento. */
    for (i = p.next - 1; i >= 0; i--) {
        if (tokens[i].start != -1 && tokens[i].end == -1) return EPTX_JSON_ERR_PARTIAL;
    }

    return p.next;
}

int eptx_json_get(const char *js, const eptx_json_tok *tokens, int count, int object, const char *key)
{
    int i, j;
    size_t key_len;

    if (object < 0 || object >= count) return -1;
    if (tokens[object].type != EPTX_JSON_OBJECT) return -1;

    key_len = strlen(key);

    for (i = object + 1; i < count; i++) {
        const eptx_json_tok *k = &tokens[i];
        if (k->parent != object) continue;
        if (k->type != EPTX_JSON_STRING) continue;
        if ((size_t)(k->end - k->start) != key_len) continue;
        if (memcmp(js + k->start, key, key_len) != 0) continue;

        /* O valor e o unico filho da chave. */
        for (j = i + 1; j < count; j++) {
            if (tokens[j].parent == i) return j;
        }
        return -1;
    }
    return -1;
}

int eptx_json_at(const eptx_json_tok *tokens, int count, int array, int n)
{
    int i, seen = 0;

    if (array < 0 || array >= count) return -1;
    if (tokens[array].type != EPTX_JSON_ARRAY) return -1;

    for (i = array + 1; i < count; i++) {
        if (tokens[i].parent != array) continue;
        if (seen == n) return i;
        seen++;
    }
    return -1;
}

/* Codifica um code point em UTF-8. Devolve quantos bytes foram escritos. */
static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
}

static uint32_t hex4(const char *p)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else v |= (uint32_t)(c - 'A' + 10);
    }
    return v;
}

int eptx_json_string(const char *js, const eptx_json_tok *tokens, int index, char *out, size_t cap)
{
    int i;
    size_t w = 0;

    if (index < 0 || tokens[index].type != EPTX_JSON_STRING) return -1;
    if (cap == 0) return -1;

    for (i = tokens[index].start; i < tokens[index].end; i++) {
        char c = js[i];

        if (c != '\\') {
            if (w + 1 >= cap) return -1;
            out[w++] = c;
            continue;
        }

        i++;
        if (i >= tokens[index].end) return -1;

        switch (js[i]) {
        case '"':  if (w + 1 >= cap) return -1; out[w++] = '"';  break;
        case '\\': if (w + 1 >= cap) return -1; out[w++] = '\\'; break;
        case '/':  if (w + 1 >= cap) return -1; out[w++] = '/';  break;
        case 'b':  if (w + 1 >= cap) return -1; out[w++] = '\b'; break;
        case 'f':  if (w + 1 >= cap) return -1; out[w++] = '\f'; break;
        case 'n':  if (w + 1 >= cap) return -1; out[w++] = '\n'; break;
        case 'r':  if (w + 1 >= cap) return -1; out[w++] = '\r'; break;
        case 't':  if (w + 1 >= cap) return -1; out[w++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            char enc[4];
            int n;
            if (i + 4 >= tokens[index].end) return -1;
            cp = hex4(js + i + 1);
            i += 4;
            /*
             * Par substituto: nomes de jogos com caractere fora do BMP sao raros,
             * mas emitir o code point de reposicao e melhor do que produzir UTF-8
             * invalido que a fonte do dashboard nao sabe desenhar.
             */
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
            n = utf8_encode(cp, enc);
            if (w + (size_t)n >= cap) return -1;
            memcpy(out + w, enc, (size_t)n);
            w += (size_t)n;
            break;
        }
        default:
            return -1;
        }
    }

    out[w] = '\0';
    return (int)w;
}

int eptx_json_int(const char *js, const eptx_json_tok *tokens, int index, int64_t *out)
{
    int i;
    int negative = 0;
    int64_t value = 0;
    int digits = 0;

    if (index < 0 || tokens[index].type != EPTX_JSON_PRIMITIVE) return -1;

    i = tokens[index].start;
    if (i < tokens[index].end && js[i] == '-') {
        negative = 1;
        i++;
    }
    for (; i < tokens[index].end; i++) {
        char c = js[i];
        /* Parte fracionaria e expoente sao descartados: a API so manda inteiros. */
        if (c == '.' || c == 'e' || c == 'E') break;
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
        digits++;
    }
    if (digits == 0) return -1;

    *out = negative ? -value : value;
    return 0;
}

int eptx_json_bool(const char *js, const eptx_json_tok *tokens, int index, int *out)
{
    int len;

    if (index < 0 || tokens[index].type != EPTX_JSON_PRIMITIVE) return -1;
    len = tokens[index].end - tokens[index].start;

    if (len == 4 && memcmp(js + tokens[index].start, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (len == 5 && memcmp(js + tokens[index].start, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

int eptx_json_get_string(const char *js, const eptx_json_tok *t, int n, int obj,
                         const char *key, char *out, size_t cap)
{
    int idx = eptx_json_get(js, t, n, obj, key);
    if (idx < 0) return -1;
    return (eptx_json_string(js, t, idx, out, cap) < 0) ? -1 : 0;
}

int eptx_json_get_int(const char *js, const eptx_json_tok *t, int n, int obj,
                      const char *key, int64_t *out)
{
    int idx = eptx_json_get(js, t, n, obj, key);
    if (idx < 0) return -1;
    return eptx_json_int(js, t, idx, out);
}

int eptx_json_get_bool(const char *js, const eptx_json_tok *t, int n, int obj,
                       const char *key, int *out)
{
    int idx = eptx_json_get(js, t, n, obj, key);
    if (idx < 0) return -1;
    return eptx_json_bool(js, t, idx, out);
}
