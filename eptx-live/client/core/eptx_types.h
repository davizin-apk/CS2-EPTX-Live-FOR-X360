/*
 * Tipos de largura fixa.
 *
 * O compilador do XDK e da familia do Visual Studio 2010, que ja traz <stdint.h>,
 * mas o fallback custa nada e elimina uma classe inteira de "nao compila" caso
 * voce acabe numa toolchain mais velha.
 */
#ifndef EPTX_TYPES_H
#define EPTX_TYPES_H

#include <stddef.h>

#if defined(_MSC_VER) && (_MSC_VER < 1600)
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned __int64   uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef __int64            int64_t;
#else
#include <stdint.h>
#endif

#endif /* EPTX_TYPES_H */
