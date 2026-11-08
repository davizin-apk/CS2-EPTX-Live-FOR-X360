/* O log do xsdk no PC: vai para o stderr. So para os testes de app.c (EPTX_PC). */
#ifndef XSDK_LOG_STUB_H
#define XSDK_LOG_STUB_H

#include <stdarg.h>
#include <stdio.h>

static inline void xsdk_log_write(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

#endif
