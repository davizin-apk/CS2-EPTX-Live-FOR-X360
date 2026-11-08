/*
 * Roda o autoteste do nucleo no PC, antes de encostar no console.
 *
 * O objetivo e eliminar tudo que nao seja ordem de bytes: se passa aqui e falha no
 * Xbox, o problema e big-endian e voce sabe exatamente onde olhar. Se falha aqui,
 * nem vale gravar no console.
 *
 *   zig cc -o selftest client/core/selftest_main.c client/core/eptx_sha256.c client/core/eptx_sign.c
 *   ./selftest
 */

#include <stdio.h>
#include "eptx_sign.h"

static void report(const char *label, const char *got, const char *want)
{
    printf("  FALHA  %s\n", label);
    printf("         obtido:  %s\n", got);
    printf("         esperado: %s\n", want);
}

void eptx_selftest_set_reporter(void (*fn)(const char *, const char *, const char *));

int main(void)
{
    int failures;

    printf("Autoteste do nucleo EPTX\n\n");
    printf("  tamanho de size_t: %d bytes\n", (int)sizeof(size_t));

    /* Descobre a ordem de bytes desta maquina, so para constar no relatorio. */
    {
        uint32_t probe = 0x01020304u;
        const unsigned char *p = (const unsigned char *)&probe;
        printf("  ordem de bytes:    %s\n\n", (p[0] == 0x01) ? "big-endian (como o Xbox 360)"
                                                             : "little-endian (PC)");
    }

    eptx_selftest_set_reporter(report);
    failures = eptx_selftest();

    if (failures == 0) {
        printf("  Todos os vetores bateram com o servidor.\n");
        return 0;
    }
    printf("\n  %d verificacao(oes) falhou(aram).\n", failures);
    return 1;
}
