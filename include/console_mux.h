/**
 * console_mux.h — Мультиплексор вывода: UART + HDMI framebuffer
 *
 * Все вызовы uart_puts() / uart_putc() продублировать на console_putc().
 * Для этого используем обёртки duo_puts / duo_putc.
 *
 * Подключить в kernel.c и shell.c вместо прямых uart_puts().
 */

#ifndef CONSOLE_MUX_H
#define CONSOLE_MUX_H

#include "uart.h"
#include "video.h"

/* Вывод символа в оба канала */
static inline void duo_putc(char c) {
    uart_putc(c);
    console_putc(c);
}

/* Вывод строки в оба канала */
static inline void duo_puts(const char *s) {
    uart_puts(s);
    console_puts(s);
}

#endif /* CONSOLE_MUX_H */
