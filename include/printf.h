#ifndef PRINTF_H
#define PRINTF_H

/**
 * printf.h — Bare-metal printf interface
 */

#include <stdarg.h>

int kprintf(const char *fmt, ...);
int ksprintf(char *buf, const char *fmt, ...);
int ksnprintf(char *buf, int size, const char *fmt, ...);
int kvprintf(const char *fmt, va_list args);

#endif /* PRINTF_H */
